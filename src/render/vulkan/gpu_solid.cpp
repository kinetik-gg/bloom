#include "gpu_solid_private.hpp"

#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
#include "gpu_scene_executor_fault_injection.hpp"
#endif

#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>

namespace bloom::render {
namespace {

using vulkan_detail::DeviceAllocatorState;
using GpuRendererAccess = bloom::render::GpuRendererAccess;

constexpr std::uint32_t kWorkgroupSizeX = 256;
constexpr std::uint64_t kDrainTimeoutNanoseconds = 2ULL * 1000ULL * 1000ULL * 1000ULL;

} // namespace

GpuSolidDiagnostic gpuSolidDiagnostic(const GpuSolidDiagnosticCode code, std::string message) {
    return GpuSolidDiagnostic{code, std::move(message)};
}

GpuSolid::GpuSolid(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuSolid::GpuSolid(GpuSolid&& other) noexcept = default;
GpuSolid& GpuSolid::operator=(GpuSolid&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuSolid::~GpuSolid() { releaseImpl(); }

void GpuSolid::releaseImpl() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    if (!impl_->onOwnerThread()) {
        // Foreign thread: never destroy native state. An Impl that owns a resident slot is
        // preserved in that same slot (orphaned) for owner retirement; an Impl with no slot owns no
        // Vulkan objects (the pipeline is created lazily under a slot) and can be destroyed here.
        if (impl_->residentSlot != kSolidNoResidentSlot) {
            impl_->orphanResidentSlot();
            [[maybe_unused]] const auto* const retained = impl_.release();
        } else {
            impl_.reset();
        }
        return;
    }
    // Owner thread: prove retirement if needed, then free native resources and return the slot. An
    // unproven submission is retained in the bounded pool for a later owner drain rather than
    // destroyed in flight.
    if (impl_->residentSlot != kSolidNoResidentSlot) {
        if (impl_->queueSubmitted) {
            cancel();
            if (!impl_->drainAndRetire()) {
                impl_->orphanResidentSlot();
                [[maybe_unused]] const auto* const retained = impl_.release();
                return;
            }
        }
        impl_->releaseResidentSlot();
    }
    impl_.reset();
}

GpuSolidJobState GpuSolid::state() const noexcept {
    return impl_ != nullptr ? impl_->jobState : GpuSolidJobState::Idle;
}
const GpuSolidDiagnostic& GpuSolid::diagnostic() const noexcept {
    static const GpuSolidDiagnostic none{};
    if (impl_ != nullptr) {
        return impl_->jobDiagnostic;
    }
    return none;
}
bool GpuSolid::isBoundTo(GpuDevice& device) const noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const auto deviceState = GpuRendererAccess::state(device);
    return deviceState != nullptr && deviceState == impl_->control;
}

bool GpuSolid::hasUnretiredSubmission() const noexcept {
    return impl_ != nullptr && impl_->queueSubmitted;
}

std::uint64_t GpuSolid::lastJobAllocationBytes() const noexcept {
    return impl_ != nullptr ? impl_->lastJobBytes : 0;
}

bool GpuSolid::Impl::drainAndRetire() noexcept {
    if (!queueSubmitted) {
        return true;
    }
    if (deviceLost) {
        queueSubmitted = false;
        return true;
    }
    const VkFence rawFence = static_cast<VkFence>(*fence);
    const VkResult waited = control->device.getDispatcher()->vkWaitForFences(
        static_cast<VkDevice>(*control->device), 1, &rawFence, VK_TRUE, kDrainTimeoutNanoseconds);
    if (waited == VK_SUCCESS || waited == VK_ERROR_DEVICE_LOST) {
        queueSubmitted = false;
        deviceLost = deviceLost || waited == VK_ERROR_DEVICE_LOST;
        return true;
    }
    return false;
}

GpuSolid::Impl::~Impl() {
    // A slot-less Impl owns no native Vulkan resources (the pipeline is created lazily under a
    // slot, and a failed creation is reset before the slot is released), so it may be destroyed
    // from any thread. A slot-holding Impl is only ever destroyed on its owner thread: a foreign
    // destruction orphans the slot instead.
    assert(residentSlot == kSolidNoResidentSlot);
    releaseResident();
    coveredPalette.release();
    coveredMask.release();
    resetPipelineResources();
}

GpuSolidCreateResult GpuSolid::create(GpuDevice& device, const GpuSolidBudgets& budgets) {
    if (budgets.maxImageBytes == 0) {
        return {nullptr, gpuSolidDiagnostic(GpuSolidDiagnosticCode::InvalidArgument,
                                            "the SolidV1 budget is out of range")};
    }
    if (device.state() != GpuDeviceState::Ready) {
        return {nullptr, gpuSolidDiagnostic(GpuSolidDiagnosticCode::DeviceUnavailable,
                                            "the GPU device is not Ready")};
    }
    if (GpuRendererAccess::owner(device) != std::this_thread::get_id()) {
        return {nullptr, gpuSolidDiagnostic(GpuSolidDiagnosticCode::WrongThread,
                                            "the SolidV1 pipeline must be created on the device "
                                            "owner thread")};
    }
    // Retire orphaned foreign-released residents on the owner thread so admission recovers.
    Impl::drainResidentOrphansOnOwnerThread();
    auto control = GpuRendererAccess::state(device);
    if (control == nullptr) {
        return {nullptr, gpuSolidDiagnostic(GpuSolidDiagnosticCode::DeviceUnavailable,
                                            "the GPU device exposes no renderer state")};
    }
    // `maxImageBytes` is a configured UPPER BOUND, not an allocation. A permissive maximum must not
    // refuse the pipeline: the actual requested image is validated against the device's real
    // maxResourceSize in begin() (querySolidImageSupport), so a small request runs even when the
    // configured maximum is larger than any physical image.
    // Lazy creation: an idle GpuSolid allocates no native resources and holds no resident slot. The
    // pipeline is created on the first begin under the bounded slot, so many pre-created instances
    // are bounded by the fixed pool rather than each owning native state.
    auto impl = std::make_unique<Impl>();
    impl->owner = std::this_thread::get_id();
    impl->control = std::move(control);
    impl->budgets = budgets;
    impl->expectedGeneration = impl->control->generation;
    return {std::unique_ptr<GpuSolid>(new GpuSolid(std::move(impl))), GpuSolidDiagnostic{}};
}

GpuSolidDiagnostic GpuSolid::begin(const GpuSolidParameters& parameters,
                                   const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::DeviceUnavailable,
                                  "the SolidV1 pipeline is not initialized");
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::WrongThread,
                                  "begin must run on the device owner thread");
    }
    // Opportunistic, non-blocking retirement of orphaned foreign-released residents.
    Impl::drainResidentOrphansOnOwnerThread();
    if (impl.deviceLost) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::DeviceLost,
                                  "the device was lost; this generation must not be reused");
    }
    if (impl.queueSubmitted || impl.jobState == GpuSolidJobState::Pending) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::Busy, "one job is already in flight");
    }
    const std::uint32_t width = parameters.dataWindow.extent().width();
    const std::uint32_t height = parameters.dataWindow.extent().height();
    if (width == 0 || height == 0) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::InvalidArgument,
                                  "the data window is empty");
    }
    if (const auto& checker = parameters.checker) {
        constexpr std::uint32_t kMaximum = 16384;
        // At the boundary the largest product is (8*(16384-1)+7)*16384,
        // below 2^31; all shader numerator and denominator products remain uint32.
        static_assert((8ULL * (kMaximum - 1ULL) + 7ULL) * kMaximum < (1ULL << 31));
        static_assert(8ULL * kMaximum * kMaximum == (1ULL << 31));
        const std::uint32_t dimensions[]{width,
                                         height,
                                         checker->authorWidth,
                                         checker->authorHeight,
                                         checker->cellWidth,
                                         checker->cellHeight,
                                         checker->compositionWidth,
                                         checker->compositionHeight,
                                         checker->proxyWidth,
                                         checker->proxyHeight};
        for (const auto dimension : dimensions) {
            if (dimension == 0 || dimension > kMaximum)
                return gpuSolidDiagnostic(
                    GpuSolidDiagnosticCode::Unsupported,
                    "CheckerV1 integer-rational device geometry exceeds its 16384-pixel limit");
        }
        if (parameters.dataWindow.originX() != 0 || parameters.dataWindow.originY() != 0)
            return gpuSolidDiagnostic(GpuSolidDiagnosticCode::Unsupported,
                                      "CheckerV1 requires a source-local data window");
    }
    const std::uint64_t required = static_cast<std::uint64_t>(width) * height * sizeof(Rgba32f);
    const std::uint64_t allowed =
        byteBudget < impl.budgets.maxImageBytes ? byteBudget : impl.budgets.maxImageBytes;
    if (required > allowed) {
        return gpuSolidDiagnostic(
            GpuSolidDiagnosticCode::OverBudget,
            "the resident image exceeds the configured or requested byte budget");
    }
    if (impl.control->generation != impl.expectedGeneration) {
        impl.deviceLost = true;
        return gpuSolidDiagnostic(
            GpuSolidDiagnosticCode::DeviceLost,
            "the device generation changed; this pipeline must not be reused");
    }
    const SolidImageSupport support = querySolidImageSupport(*impl.control, width, height);
    if (!support.supported) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::Unsupported, support.reason);
    }
    if (required > support.maxImageBytes) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::OverBudget,
                                  "the resident image exceeds the device resource limit");
    }

    // Acquire the bounded resident slot BEFORE the first native allocation, and create the pipeline
    // lazily under it. A full pool refuses cleanly without allocating anything.
    if (!impl.acquireResidentSlot()) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::DeviceUnavailable,
                                  "the bounded SolidV1 resident pool is full; no native resources "
                                  "were allocated");
    }
    if (!impl.pipelineReady) {
        if (!impl.createPipeline()) {
            impl.resetPipelineResources();
            impl.releaseResidentSlot();
            return impl.createDiagnostic;
        }
        impl.pipelineReady = true;
    }
    if (parameters.checker && !impl.createCheckerPipeline()) {
        const auto diagnostic = impl.createDiagnostic;
        impl.resetPipelineResources();
        impl.releaseResidentSlot();
        return diagnostic;
    }

    impl.clearJob();
    auto resident = std::make_unique<GpuImageImpl>();
    resident->state = impl.control;
    resident->dataWindow = parameters.dataWindow;
    resident->displayWindow = parameters.displayWindow;
    resident->pixelAspect = parameters.pixelAspect;
    resident->generation = impl.control->generation;
    if (!createResidentImage(*impl.control, width, height, *resident)) {
        return gpuSolidDiagnostic(GpuSolidDiagnosticCode::AllocationFailed,
                                  "the resident image could not be allocated");
    }
    GpuImageImpl* const residentRaw = resident.get();
    impl.residentImage = std::make_unique<GpuImage>(makeGpuImage(std::move(resident)));
    impl.lastJobBytes = impl.residentImage->allocationBytes();

    const VkDevice rawDevice = static_cast<VkDevice>(*impl.control->device);
    const auto* dispatcher = impl.control->device.getDispatcher();
    const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*impl.commandBuffer);
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    if (dispatcher->vkResetFences(rawDevice, 1, &rawFence) != VK_SUCCESS ||
        dispatcher->vkResetCommandBuffer(rawCommandBuffer, 0) != VK_SUCCESS) {
        impl.releaseResident();
        impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable,
                  "the SolidV1 fence or command buffer could not be reset");
        return impl.jobDiagnostic;
    }

    vk::DescriptorImageInfo imageInfo{};
    imageInfo.imageView = residentRaw->view;
    imageInfo.imageLayout = vk::ImageLayout::eGeneral;
    vk::WriteDescriptorSet write{};
    write.dstSet = *impl.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eStorageImage;
    write.pImageInfo = &imageInfo;
    dispatcher->vkUpdateDescriptorSets(
        rawDevice, 1, reinterpret_cast<const VkWriteDescriptorSet*>(&write), 0, nullptr);

    vk::CommandBufferBeginInfo beginInfo{};
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    if (dispatcher->vkBeginCommandBuffer(
            rawCommandBuffer, reinterpret_cast<const VkCommandBufferBeginInfo*>(&beginInfo)) !=
        VK_SUCCESS) {
        impl.releaseResident();
        impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable,
                  "the SolidV1 command buffer could not begin");
        return impl.jobDiagnostic;
    }

    VkImageMemoryBarrier toGeneral{};
    toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGeneral.srcAccessMask = 0;
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image = residentRaw->image;
    toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toGeneral);

    const auto selectedPipeline = parameters.checker ? *impl.checkerPipeline : *impl.pipeline;
    const auto selectedLayout =
        parameters.checker ? *impl.checkerPipelineLayout : *impl.pipelineLayout;
    impl.commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, selectedPipeline);
    impl.commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, selectedLayout, 0,
                                          {*impl.descriptorSet}, {});
    if (const auto& checker = parameters.checker) {
        CheckerPushConstants push{};
        push.colorA[0] = parameters.pixel.red();
        push.colorA[1] = parameters.pixel.green();
        push.colorA[2] = parameters.pixel.blue();
        push.colorA[3] = parameters.pixel.alpha();
        push.colorB[0] = checker->colorB.red();
        push.colorB[1] = checker->colorB.green();
        push.colorB[2] = checker->colorB.blue();
        push.colorB[3] = checker->colorB.alpha();
        push.width = width;
        push.height = height;
        push.authorWidth = checker->authorWidth;
        push.authorHeight = checker->authorHeight;
        push.cellWidth = checker->cellWidth;
        push.cellHeight = checker->cellHeight;
        push.compositionWidth = checker->compositionWidth;
        push.compositionHeight = checker->compositionHeight;
        push.proxyWidth = checker->proxyWidth;
        push.proxyHeight = checker->proxyHeight;
        impl.commandBuffer.pushConstants(selectedLayout, vk::ShaderStageFlagBits::eCompute, 0,
                                         sizeof(push), &push);
    } else {
        SolidPushConstants push{};
        push.pixel[0] = parameters.pixel.red();
        push.pixel[1] = parameters.pixel.green();
        push.pixel[2] = parameters.pixel.blue();
        push.pixel[3] = parameters.pixel.alpha();
        push.width = width;
        push.height = height;
        impl.commandBuffer.pushConstants(selectedLayout, vk::ShaderStageFlagBits::eCompute, 0,
                                         sizeof(push), &push);
    }
    const std::uint64_t groupCount =
        (static_cast<std::uint64_t>(width) + kWorkgroupSizeX - 1U) / kWorkgroupSizeX;
    const auto groups = static_cast<std::uint32_t>(groupCount);
    impl.commandBuffer.dispatch(groups, height, 1);

    VkImageMemoryBarrier toRead = toGeneral;
    toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toRead);

    if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
        impl.releaseResident();
        impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable,
                  "the SolidV1 command buffer could not end");
        return impl.jobDiagnostic;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &rawCommandBuffer;
    const VkResult submitted = dispatcher->vkQueueSubmit(
        static_cast<VkQueue>(*impl.control->computeQueue), 1, &submit, rawFence);
    if (submitted == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        impl.releaseResident();
        impl.fail(GpuSolidDiagnosticCode::DeviceLost, "the device was lost during submission");
        return impl.jobDiagnostic;
    }
    if (submitted != VK_SUCCESS) {
        impl.releaseResident();
        impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable,
                  "the SolidV1 dispatch could not submit");
        return impl.jobDiagnostic;
    }
    impl.queueSubmitted = true;
    impl.jobState = GpuSolidJobState::Pending;
    impl.jobDiagnostic = GpuSolidDiagnostic{};
    return {};
}

GpuSolidPollResult GpuSolid::poll() {
    if (impl_ == nullptr) {
        return GpuSolidPollResult::Failure;
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return GpuSolidPollResult::WrongThread;
    }
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
    // TEST-ONLY fault hook, inert in every production build. It lets the executor retirement tests
    // observe a real submitted job as stalled, device-lost or unproven against the real fence.
    if (const auto injected = gpu_scene_executor_fault::take();
        injected != gpu_scene_executor_fault::PollFault::None) {
        if (injected == gpu_scene_executor_fault::PollFault::StallPending) {
            return GpuSolidPollResult::Pending;
        }
        if (injected == gpu_scene_executor_fault::PollFault::DeviceLost) {
            // Bounded wait proves the REAL submission retired before pretending loss. Only
            // VK_SUCCESS may clear the submission or release the covered inputs; a timeout or an
            // unknown wait result must preserve them and fail safe, because the fence is not proven
            // signalled and the queue may still reference those buffers.
            const VkFence faultFence = static_cast<VkFence>(*impl.fence);
            const VkResult faultWait = impl.control->device.getDispatcher()->vkWaitForFences(
                static_cast<VkDevice>(*impl.control->device), 1, &faultFence, VK_TRUE,
                1'000'000'000ULL);
            if (faultWait == VK_SUCCESS) {
                impl.deviceLost = true;
                impl.queueSubmitted = false;
                impl.coveredPalette.release();
                impl.coveredMask.release();
                impl.fail(GpuSolidDiagnosticCode::DeviceLost,
                          "injected device loss after proven retirement");
            } else {
                impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable,
                          "injected device loss could not prove fence retirement; the submission "
                          "is retained");
            }
            return GpuSolidPollResult::Failure;
        }
        impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable,
                  "injected unknown fence status; the submission is not retired");
        return GpuSolidPollResult::Failure;
    }
#endif
    if (impl.jobState == GpuSolidJobState::Ready) {
        return GpuSolidPollResult::Ready;
    }
    const bool pending = impl.jobState == GpuSolidJobState::Pending;
    // A live submission can outlive a Failure job state (an unexpected fence
    // result). Keep querying the fence in that case so a later successful owner
    // poll retires it without publishing a frame.
    if (!pending && !impl.queueSubmitted) {
        return GpuSolidPollResult::Failure;
    }
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    const VkResult status = impl.control->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*impl.control->device), rawFence);
    if (status == VK_NOT_READY) {
        return pending ? GpuSolidPollResult::Pending : GpuSolidPollResult::Failure;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        impl.queueSubmitted = false;
        impl.coveredPalette.release();
        impl.coveredMask.release();
        if (pending) {
            impl.fail(GpuSolidDiagnosticCode::DeviceLost, "the device was lost while polling");
        }
        return GpuSolidPollResult::Failure;
    }
    if (status != VK_SUCCESS) {
        // Unknown status: the fence is NOT proved signalled; keep queueSubmitted so
        // the submission stays retired-only-on-known-completion.
        if (pending) {
            impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable,
                      "the SolidV1 fence returned an unexpected status; the "
                      "submission is not retired");
        }
        return GpuSolidPollResult::Failure;
    }
    impl.queueSubmitted = false;
    // The dispatch is proved complete, so the covered input buffers are no longer
    // referenced by the queue.
    impl.coveredPalette.release();
    impl.coveredMask.release();
    if (!pending) {
        // Retired after an already-published failure; nothing to publish.
        return GpuSolidPollResult::Failure;
    }
    if (impl.discardRequested.load()) {
        impl.clearJob();
        impl.fail(GpuSolidDiagnosticCode::Cancelled, "the SolidV1 job was cancelled");
        return GpuSolidPollResult::Failure;
    }
    if (impl.residentImage == nullptr) {
        impl.fail(GpuSolidDiagnosticCode::DeviceUnavailable, "the resident image is missing");
        return GpuSolidPollResult::Failure;
    }
    impl.jobState = GpuSolidJobState::Ready;
    impl.jobDiagnostic = GpuSolidDiagnostic{};
    return GpuSolidPollResult::Ready;
}

const GpuImage* GpuSolid::image() const noexcept {
    if (impl_ == nullptr || impl_->jobState != GpuSolidJobState::Ready) {
        return nullptr;
    }
    return impl_->residentImage.get();
}

GpuImage GpuSolid::takeImage() noexcept {
    // Only a Ready job may be taken. While a job is Pending, its resident image and (for the
    // covered variant) its mask/palette buffers are still referenced by an unretired submission;
    // refusing here leaves the job, its queue submission and its retirement state untouched
    // (mirrors GpuComposite::takeImage).
    if (impl_ == nullptr || impl_->residentImage == nullptr ||
        impl_->jobState != GpuSolidJobState::Ready) {
        return GpuImage{};
    }
    GpuImage taken = std::move(*impl_->residentImage);
    impl_->residentImage.reset();
    impl_->clearJob();
    return taken;
}

GpuImageReadback GpuSolid::readback() noexcept {
    if (impl_ == nullptr || impl_->residentImage == nullptr) {
        GpuImageReadback result;
        result.code = GpuImageReadbackCode::DeviceUnavailable;
        result.message = "no resident image to read back";
        return result;
    }
    return readbackResidentImage(*impl_->residentImage, impl_->budgets.maxImageBytes);
}

void GpuSolid::cancel() noexcept {
    if (impl_ != nullptr) {
        impl_->discardRequested.store(true);
    }
}

bool GpuSolid::teardownDrainIncomplete() noexcept {
    // Recoverable pressure, not a permanent fuse: true while a foreign-released or unproven
    // resident is retained in the bounded pool, and false again once the rightful owner drains it.
    return solid_detail::solidResidentOrphaned() > 0;
}

} // namespace bloom::render
