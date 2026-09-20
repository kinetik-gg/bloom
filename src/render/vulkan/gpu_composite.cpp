#include <bloom/render/gpu_composite.hpp>

#include "gpu_composite_fault.hpp"
#include "gpu_composite_private.hpp"

#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
#include "gpu_scene_executor_fault_injection.hpp"
#endif

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::render {
namespace {

constexpr std::uint64_t kDrainTimeoutNanoseconds = 2ULL * 1000ULL * 1000ULL * 1000ULL;

[[nodiscard]] GpuCompositeDiagnostic makeDiagnostic(const GpuCompositeDiagnosticCode code,
                                                    std::string message) {
    return compositeDiagnostic(code, std::move(message));
}

} // namespace

std::vector<GpuAxisSample> prepareTranslationAxis(const std::uint32_t outputExtent,
                                                  const std::uint32_t sourceExtent,
                                                  const double translation) {
    std::vector<GpuAxisSample> axis(outputExtent);
    for (std::uint32_t local = 0; local < outputExtent; ++local) {
        // Exact CPU arithmetic: Float64 subtraction, then floor and a single Float32 rounding of
        // the fractional part. The sentinel mirrors the CPU's `sampleLocal <= -1.0 || >=
        // sourceExtent` transparent branch.
        const auto sample = static_cast<double>(local) - translation;
        if (sample <= -1.0 || sample >= static_cast<double>(sourceExtent)) {
            axis[local] = GpuAxisSample{kGpuAxisOutOfRange, 0.0F};
            continue;
        }
        const auto base = static_cast<std::int64_t>(std::floor(sample));
        axis[local] = GpuAxisSample{static_cast<std::int32_t>(base),
                                    static_cast<float>(sample - static_cast<double>(base))};
    }
    return axis;
}

void GpuComposite::Impl::fail(const GpuCompositeDiagnosticCode code, std::string message) {
    jobState = GpuCompositeJobState::Failure;
    jobDiagnostic = compositeDiagnostic(code, std::move(message));
}

void GpuComposite::Impl::clearJob() {
    jobState = GpuCompositeJobState::Idle;
    jobDiagnostic = GpuCompositeDiagnostic{};
    discardRequested.store(false);
    residentImage.reset();
    retainedSource.reset();
    retainedDestination.reset();
    axisX.release();
    axisY.release();
    status.release();
    statusMapped = nullptr;
}

GpuComposite::GpuComposite(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuComposite::GpuComposite(GpuComposite&& other) noexcept = default;
GpuComposite& GpuComposite::operator=(GpuComposite&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuComposite::~GpuComposite() { releaseImpl(); }

void GpuComposite::releaseImpl() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    if (!impl_->onOwnerThread()) {
        // Foreign thread: never destroy native state. An Impl that owns a resident slot is
        // preserved in that same slot (orphaned) for owner retirement; an Impl with no slot owns no
        // Vulkan objects (the pipelines are created lazily under a slot) and can be destroyed here.
        if (impl_->residentSlot != kCompositeNoResidentSlot) {
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
    if (impl_->residentSlot != kCompositeNoResidentSlot) {
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

GpuCompositeJobState GpuComposite::state() const noexcept {
    return impl_ != nullptr ? impl_->jobState : GpuCompositeJobState::Idle;
}
const GpuCompositeDiagnostic& GpuComposite::diagnostic() const noexcept {
    static const GpuCompositeDiagnostic none{};
    if (impl_ == nullptr) {
        return none;
    }
    return impl_->jobDiagnostic;
}
bool GpuComposite::isBoundTo(GpuDevice& device) const noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const auto deviceState = GpuRendererAccess::state(device);
    return deviceState != nullptr && deviceState == impl_->control;
}

bool GpuComposite::hasUnretiredSubmission() const noexcept {
    return impl_ != nullptr && impl_->queueSubmitted;
}

std::uint64_t GpuComposite::lastJobAllocationBytes() const noexcept {
    return impl_ != nullptr ? impl_->lastJobBytes : 0;
}

bool GpuComposite::Impl::drainAndRetire() noexcept {
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

GpuComposite::Impl::~Impl() {
    // A slot-less Impl owns no native Vulkan resources (the pipelines are created lazily under a
    // slot, and a failed creation is reset before the slot is released), so it may be destroyed
    // from any thread. A slot-holding Impl is only ever destroyed on its owner thread: a foreign
    // destruction orphans the slot instead.
    assert(residentSlot == kCompositeNoResidentSlot);
    retainedSource.reset();
    retainedDestination.reset();
    residentImage.reset();
}

GpuCompositeCreateResult GpuComposite::create(GpuDevice& device,
                                              const GpuCompositeBudgets& budgets) {
    if (budgets.maxImageBytes == 0 || budgets.maxMetadataBytes == 0) {
        return {nullptr, makeDiagnostic(GpuCompositeDiagnosticCode::InvalidArgument,
                                        "the composite budget is out of range")};
    }
    if (device.state() != GpuDeviceState::Ready) {
        return {nullptr, makeDiagnostic(GpuCompositeDiagnosticCode::DeviceUnavailable,
                                        "the GPU device is not Ready")};
    }
    if (GpuRendererAccess::owner(device) != std::this_thread::get_id()) {
        return {nullptr,
                makeDiagnostic(GpuCompositeDiagnosticCode::WrongThread,
                               "the composite pipeline must be created on the device owner "
                               "thread")};
    }
    // Retire orphaned foreign-released residents on the owner thread so admission recovers.
    Impl::drainResidentOrphansOnOwnerThread();
    auto control = GpuRendererAccess::state(device);
    if (control == nullptr) {
        return {nullptr, makeDiagnostic(GpuCompositeDiagnosticCode::DeviceUnavailable,
                                        "the GPU device exposes no renderer state")};
    }
    // Lazy creation: an idle GpuComposite allocates no native resources and holds no resident slot.
    // Both pipelines are created on the first begin under the bounded slot, so many pre-created
    // instances are bounded by the fixed pool rather than each owning native state.
    auto impl = std::make_unique<Impl>();
    impl->owner = std::this_thread::get_id();
    impl->control = std::move(control);
    impl->budgets = budgets;
    impl->expectedGeneration = impl->control->generation;
    return {std::unique_ptr<GpuComposite>(new GpuComposite(std::move(impl))),
            GpuCompositeDiagnostic{}};
}

bool GpuComposite::Impl::checkStatusFlag() {
    if (statusMapped == nullptr) {
        return true;
    }
    std::uint32_t flag = 0;
    std::memcpy(&flag, statusMapped, sizeof(flag));
    if (flag != 0) {
        fail(GpuCompositeDiagnosticCode::StatusFlagRejected,
             "the composite kernel rejected the frame (non-finite or subnormal input); use the CPU "
             "reference path");
        return false;
    }
    return true;
}

GpuCompositeDiagnostic GpuComposite::beginTranslation(const GpuTranslationParameters& parameters,
                                                      const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::DeviceUnavailable,
                              "the composite pipeline is not initialized");
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::WrongThread,
                              "beginTranslation must run on the device owner thread");
    }
    // Opportunistic, non-blocking retirement of orphaned foreign-released residents.
    Impl::drainResidentOrphansOnOwnerThread();
    if (impl.deviceLost) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::DeviceLost,
                              "the device was lost; this generation must not be reused");
    }
    if (impl.queueSubmitted || impl.jobState == GpuCompositeJobState::Pending) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::Busy, "one job is already in flight");
    }
    if (parameters.source == nullptr || !parameters.source->isValid()) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::InvalidArgument,
                              "the translation source image is missing");
    }
    const auto sourceDataWindow = parameters.source->dataWindow();
    if (!sourceDataWindow.has_value()) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::InvalidArgument,
                              "the source image has no data window");
    }
    const std::uint32_t sourceWidth = sourceDataWindow->extent().width();
    const std::uint32_t sourceHeight = sourceDataWindow->extent().height();
    const std::uint32_t outputWidth = parameters.outputWindow.extent().width();
    const std::uint32_t outputHeight = parameters.outputWindow.extent().height();
    if (sourceWidth == 0 || sourceHeight == 0 || outputWidth == 0 || outputHeight == 0) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::InvalidArgument, "an extent is empty");
    }
    if (!std::isfinite(parameters.translationX) || !std::isfinite(parameters.translationY) ||
        !std::isfinite(parameters.opacity) || parameters.opacity < 0.0F ||
        parameters.opacity > 1.0F) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::InvalidArgument,
                              "the translation parameters are out of domain");
    }
    // Requested byte math is overflow-checked: two uint32 extents can overflow uint64.
    std::uint64_t imageBytes = 0;
    std::uint64_t axisXBytes = 0;
    std::uint64_t axisYBytes = 0;
    std::uint64_t axisBytes = 0;
    std::uint64_t metadataBytes = 0;
    if (compositeMultiplyOverflows(outputWidth, outputHeight, imageBytes) ||
        compositeMultiplyOverflows(imageBytes, sizeof(Rgba32f), imageBytes) ||
        compositeMultiplyOverflows(outputWidth, sizeof(CompositeAxisSampleGpu), axisXBytes) ||
        compositeMultiplyOverflows(outputHeight, sizeof(CompositeAxisSampleGpu), axisYBytes) ||
        compositeAddOverflows(axisXBytes, axisYBytes, axisBytes) ||
        compositeAddOverflows(axisBytes, sizeof(std::uint32_t), metadataBytes)) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::OverBudget,
                              "the translation request overflows the byte arithmetic");
    }
    const std::uint64_t allowedImage =
        byteBudget < impl.budgets.maxImageBytes ? byteBudget : impl.budgets.maxImageBytes;
    if (imageBytes > allowedImage || metadataBytes > impl.budgets.maxMetadataBytes) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::OverBudget,
                              "the translation image or metadata exceeds the byte budget");
    }
    if (impl.control->generation != impl.expectedGeneration) {
        impl.deviceLost = true;
        return makeDiagnostic(GpuCompositeDiagnosticCode::DeviceLost,
                              "the device generation changed; this pipeline must not be reused");
    }
    // The source must belong to exactly this device generation BEFORE any driver resource is
    // created or bound. A foreign-device image would otherwise be bound into this device's
    // descriptor set.
    if (!compositeImageBelongsTo(gpuImageImpl(*parameters.source), impl.control)) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::InvalidArgument,
                              "the translation source image does not belong to this device");
    }
    const SolidImageSupport support =
        querySolidImageSupport(*impl.control, outputWidth, outputHeight);
    if (!support.supported || imageBytes > support.maxImageBytes) {
        return makeDiagnostic(support.supported ? GpuCompositeDiagnosticCode::OverBudget
                                                : GpuCompositeDiagnosticCode::Unsupported,
                              support.supported ? "the output image exceeds the device limit"
                                                : support.reason);
    }

    // Acquire the bounded resident slot BEFORE the first native allocation, and create the
    // pipelines lazily under it. A full pool refuses cleanly without allocating anything.
    if (!impl.ensureResidentReady()) {
        return impl.createDiagnostic;
    }

    impl.clearJob();
    // Prepare host metadata first so any allocation failure is before any device work.
    const auto axisX = prepareTranslationAxis(outputWidth, sourceWidth, parameters.translationX);
    const auto axisY = prepareTranslationAxis(outputHeight, sourceHeight, parameters.translationY);
    if (!createCompositeBuffer(*impl.control, axisX.size() * sizeof(CompositeAxisSampleGpu), false,
                               axisX.data(), impl.axisX) ||
        !createCompositeBuffer(*impl.control, axisY.size() * sizeof(CompositeAxisSampleGpu), false,
                               axisY.data(), impl.axisY)) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::AllocationFailed,
                              "the axis metadata buffers could not be allocated");
    }
    const std::uint32_t zeroFlag = 0;
    if (!createCompositeBuffer(*impl.control, sizeof(zeroFlag), true, &zeroFlag, impl.status)) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::AllocationFailed,
                              "the status buffer could not be allocated");
    }
    VmaAllocationInfo statusInfo{};
    vmaGetAllocationInfo(impl.control->allocator, impl.status.allocation, &statusInfo);
    impl.statusMapped = statusInfo.pMappedData;

    auto resident = std::make_unique<GpuImageImpl>();
    resident->state = impl.control;
    // Data window is the requested output window; DISPLAY window and pixel aspect are preserved
    // from the source so a nonzero-origin composition keeps its geometry.
    resident->dataWindow = parameters.outputWindow;
    resident->displayWindow = parameters.source->displayWindow();
    resident->pixelAspect = parameters.source->pixelAspect();
    resident->generation = impl.control->generation;
    if (!createResidentImage(*impl.control, outputWidth, outputHeight, *resident)) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::AllocationFailed,
                              "the translation output image could not be allocated");
    }
    GpuImageImpl* const outputRaw = resident.get();

    // The budget is enforced on the ACTUAL VMA allocation sizes (allocator rounding included), not
    // the requested pixel bytes, plus the per-call transient peak from the staging uploads. A
    // refusal here still leaves the input and the original pipeline usable.
    std::uint64_t retainedActual = 0;
    std::uint64_t peakActual = 0;
    {
        const std::uint64_t outputActual = compositeImageAllocationBytes(*outputRaw);
        const std::uint64_t axisXActual = compositeBufferAllocationBytes(impl.axisX);
        const std::uint64_t axisYActual = compositeBufferAllocationBytes(impl.axisY);
        const std::uint64_t statusActual = compositeBufferAllocationBytes(impl.status);
        std::uint64_t sum = 0;
        std::uint64_t staging = axisXActual > axisYActual ? axisXActual : axisYActual;
        if (compositeAddOverflows(outputActual, axisXActual, sum) ||
            compositeAddOverflows(sum, axisYActual, sum) ||
            compositeAddOverflows(sum, statusActual, sum) ||
            compositeAddOverflows(sum, staging, peakActual)) {
            return makeDiagnostic(GpuCompositeDiagnosticCode::OverBudget,
                                  "the translation allocation sizes overflow the byte arithmetic");
        }
        retainedActual = sum;
    }
    if (retainedActual > byteBudget || peakActual > byteBudget) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::OverBudget,
                              "the actual translation allocations or transient peak exceed the "
                              "byte budget");
    }
    impl.lastJobBytes = retainedActual > peakActual ? retainedActual : peakActual;

    impl.residentImage = std::make_unique<GpuImage>(makeGpuImage(std::move(resident)));
    impl.retainedSource = parameters.source;
    impl.translationJob = true;

    const VkDevice rawDevice = static_cast<VkDevice>(*impl.control->device);
    const auto* dispatcher = impl.control->device.getDispatcher();
    const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*impl.commandBuffer);
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    if (dispatcher->vkResetFences(rawDevice, 1, &rawFence) != VK_SUCCESS ||
        dispatcher->vkResetCommandBuffer(rawCommandBuffer, 0) != VK_SUCCESS) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::DeviceUnavailable,
                              "the composite fence or command buffer could not be reset");
    }

    const GpuImageImpl* const sourceRaw = gpuImageImpl(*impl.retainedSource);
    vk::DescriptorImageInfo sourceInfo{};
    sourceInfo.imageView = sourceRaw->view;
    sourceInfo.imageLayout = vk::ImageLayout::eGeneral;
    vk::DescriptorImageInfo outputInfo{};
    outputInfo.imageView = outputRaw->view;
    outputInfo.imageLayout = vk::ImageLayout::eGeneral;
    vk::DescriptorBufferInfo axisXInfo{};
    axisXInfo.buffer = impl.axisX.buffer;
    axisXInfo.range = VK_WHOLE_SIZE;
    vk::DescriptorBufferInfo axisYInfo{};
    axisYInfo.buffer = impl.axisY.buffer;
    axisYInfo.range = VK_WHOLE_SIZE;
    vk::DescriptorBufferInfo statusInfoWrite{};
    statusInfoWrite.buffer = impl.status.buffer;
    statusInfoWrite.range = VK_WHOLE_SIZE;
    std::array<vk::WriteDescriptorSet, kTranslationBindingCount> writes{};
    writes[0] = vk::WriteDescriptorSet{};
    writes[0].dstSet = *impl.translationSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = vk::DescriptorType::eStorageImage;
    writes[0].pImageInfo = &sourceInfo;
    writes[1] = writes[0];
    writes[1].dstBinding = 1;
    writes[1].pImageInfo = &outputInfo;
    writes[2] = vk::WriteDescriptorSet{};
    writes[2].dstSet = *impl.translationSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[2].pBufferInfo = &axisXInfo;
    writes[3] = writes[2];
    writes[3].dstBinding = 3;
    writes[3].pBufferInfo = &axisYInfo;
    writes[4] = writes[2];
    writes[4].dstBinding = 4;
    writes[4].pBufferInfo = &statusInfoWrite;
    dispatcher->vkUpdateDescriptorSets(rawDevice, static_cast<std::uint32_t>(writes.size()),
                                       reinterpret_cast<const VkWriteDescriptorSet*>(writes.data()),
                                       0, nullptr);

    vk::CommandBufferBeginInfo beginInfo{};
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    if (dispatcher->vkBeginCommandBuffer(
            rawCommandBuffer, reinterpret_cast<const VkCommandBufferBeginInfo*>(&beginInfo)) !=
        VK_SUCCESS) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::DeviceUnavailable,
                              "the composite command buffer could not begin");
    }
    // Both input and output must be GENERAL for storage-image access. The source is produced by an
    // earlier operation and already GENERAL; the output is fresh (UNDEFINED).
    VkImageMemoryBarrier outputBarrier{};
    outputBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    outputBarrier.srcAccessMask = 0;
    outputBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    outputBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    outputBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    outputBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputBarrier.image = outputRaw->image;
    outputBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &outputBarrier);

    impl.commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, *impl.translation.pipeline);
    impl.commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                          *impl.translation.pipelineLayout, 0,
                                          {*impl.translationSet}, {});
    const CompositeTranslationPush push{outputWidth, outputHeight, sourceWidth, sourceHeight,
                                        parameters.opacity};
    impl.commandBuffer.pushConstants(*impl.translation.pipelineLayout,
                                     vk::ShaderStageFlagBits::eCompute, 0,
                                     sizeof(CompositeTranslationPush), &push);
    // One invocation per output pixel: X tiles the row in 256-wide groups, Y is the row.
    const auto groups = static_cast<std::uint32_t>(
        compositeDispatchGroupCount(static_cast<std::uint64_t>(outputWidth)));
    impl.commandBuffer.dispatch(groups, outputHeight, 1);

    VkImageMemoryBarrier toRead = outputBarrier;
    toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toRead);

    if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::DeviceUnavailable,
                              "the composite command buffer could not end");
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &rawCommandBuffer;
    const VkResult submitted = dispatcher->vkQueueSubmit(
        static_cast<VkQueue>(*impl.control->computeQueue), 1, &submit, rawFence);
    if (submitted == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        return makeDiagnostic(GpuCompositeDiagnosticCode::DeviceLost,
                              "the device was lost during submission");
    }
    if (submitted != VK_SUCCESS) {
        return makeDiagnostic(GpuCompositeDiagnosticCode::DeviceUnavailable,
                              "the translation dispatch could not submit");
    }
    impl.queueSubmitted = true;
    impl.jobState = GpuCompositeJobState::Pending;
    impl.jobDiagnostic = GpuCompositeDiagnostic{};
    return {};
}

GpuCompositePollResult GpuComposite::poll() {
    if (impl_ == nullptr) {
        return GpuCompositePollResult::Failure;
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return GpuCompositePollResult::WrongThread;
    }
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
    // TEST-ONLY fault hook, inert in every production build. It lets the executor retirement tests
    // observe a real submitted job as stalled, device-lost or unproven against the real fence.
    if (const auto injected = gpu_scene_executor_fault::take();
        injected != gpu_scene_executor_fault::PollFault::None) {
        if (injected == gpu_scene_executor_fault::PollFault::StallPending) {
            return GpuCompositePollResult::Pending;
        }
        if (injected == gpu_scene_executor_fault::PollFault::DeviceLost) {
            // Bounded wait proves the REAL submission retired before pretending loss. Only
            // VK_SUCCESS may clear the submission; a timeout or an unknown wait result must
            // preserve it and fail safe, because the fence is not proven signalled.
            const VkFence faultFence = static_cast<VkFence>(*impl.fence);
            const VkResult faultWait = impl.control->device.getDispatcher()->vkWaitForFences(
                static_cast<VkDevice>(*impl.control->device), 1, &faultFence, VK_TRUE,
                1'000'000'000ULL);
            if (faultWait == VK_SUCCESS) {
                impl.deviceLost = true;
                impl.queueSubmitted = false;
                impl.fail(GpuCompositeDiagnosticCode::DeviceLost,
                          "injected device loss after proven retirement");
            } else {
                impl.fail(GpuCompositeDiagnosticCode::DeviceUnavailable,
                          "injected device loss could not prove fence retirement; the submission "
                          "is retained");
            }
            return GpuCompositePollResult::Failure;
        }
        impl.fail(GpuCompositeDiagnosticCode::DeviceUnavailable,
                  "injected unknown fence status; the submission is not retired");
        return GpuCompositePollResult::Failure;
    }
#endif
    if (impl.jobState == GpuCompositeJobState::Ready) {
        return GpuCompositePollResult::Ready;
    }
    const bool pending = impl.jobState == GpuCompositeJobState::Pending;
    if (!pending && !impl.queueSubmitted) {
        return GpuCompositePollResult::Failure;
    }
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    const VkResult status = impl.control->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*impl.control->device), rawFence);
    if (status == VK_NOT_READY) {
        return pending ? GpuCompositePollResult::Pending : GpuCompositePollResult::Failure;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        impl.queueSubmitted = false;
        if (pending) {
            impl.fail(GpuCompositeDiagnosticCode::DeviceLost, "the device was lost while polling");
        }
        return GpuCompositePollResult::Failure;
    }
    if (status != VK_SUCCESS) {
        if (pending) {
            impl.fail(GpuCompositeDiagnosticCode::DeviceUnavailable,
                      "the composite fence returned an unexpected status; the submission is not "
                      "retired");
        }
        return GpuCompositePollResult::Failure;
    }
    impl.queueSubmitted = false;
    if (!pending) {
        return GpuCompositePollResult::Failure;
    }
    if (impl.discardRequested.load()) {
        impl.clearJob();
        impl.fail(GpuCompositeDiagnosticCode::Cancelled, "the composite job was cancelled");
        return GpuCompositePollResult::Failure;
    }
    if (impl.residentImage == nullptr) {
        impl.fail(GpuCompositeDiagnosticCode::DeviceUnavailable, "the resident image is missing");
        return GpuCompositePollResult::Failure;
    }
    // The status flag is only meaningful after the fence; a nonzero flag rejects the frame.
    if (!impl.checkStatusFlag()) {
        return GpuCompositePollResult::Failure;
    }
    impl.jobState = GpuCompositeJobState::Ready;
    impl.jobDiagnostic = GpuCompositeDiagnostic{};
    return GpuCompositePollResult::Ready;
}

const GpuImage* GpuComposite::image() const noexcept {
    if (impl_ == nullptr || impl_->jobState != GpuCompositeJobState::Ready) {
        return nullptr;
    }
    return impl_->residentImage.get();
}

GpuImage GpuComposite::takeImage() noexcept {
    if (impl_ == nullptr || impl_->residentImage == nullptr ||
        impl_->jobState != GpuCompositeJobState::Ready) {
        return GpuImage{};
    }
    GpuImage taken = std::move(*impl_->residentImage);
    impl_->clearJob();
    return taken;
}

void GpuComposite::cancel() noexcept {
    if (impl_ != nullptr) {
        impl_->discardRequested.store(true);
    }
}

bool GpuComposite::teardownDrainIncomplete() noexcept {
    // Recoverable pressure, not a permanent fuse: true while a foreign-released or unproven
    // resident is retained in the bounded pool, and false again once the rightful owner drains it.
    return composite_detail::compositeResidentOrphaned() > 0;
}

} // namespace bloom::render
