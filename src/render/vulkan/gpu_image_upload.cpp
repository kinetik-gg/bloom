#include "gpu_image_upload_private.hpp"

#include <bloom/render/image.hpp>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>

#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
#include "gpu_scene_executor_fault_injection.hpp"
#endif

namespace bloom::render {
namespace {

using vulkan_detail::DeviceAllocatorState;
using GpuRendererAccess = bloom::render::GpuRendererAccess;

constexpr std::uint64_t kDrainTimeoutNanoseconds = 2ULL * 1000ULL * 1000ULL * 1000ULL;

} // namespace

GpuImageUpload::Impl::~Impl() {
    // A slot-less Impl owns no native Vulkan resources (the command resources are created lazily
    // under a slot, and a failed creation is reset before the slot is released), so it may be
    // destroyed from any thread. A slot-holding Impl is only ever destroyed on its owner thread: a
    // foreign destruction orphans the slot instead.
    assert(residentSlot == kUploadNoResidentSlot);
    releaseResident();
    staging.release();
}

void GpuImageUpload::Impl::resetResources() noexcept {
    commandPool = vk::raii::CommandPool{nullptr};
    commandBuffer = vk::raii::CommandBuffer{nullptr};
    fence = vk::raii::Fence{nullptr};
    resourcesReady = false;
}

GpuImageUpload::GpuImageUpload(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

GpuImageUpload::GpuImageUpload(GpuImageUpload&& other) noexcept = default;

GpuImageUpload& GpuImageUpload::operator=(GpuImageUpload&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

GpuImageUpload::~GpuImageUpload() { releaseImpl(); }

void GpuImageUpload::releaseImpl() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    if (!impl_->onOwnerThread()) {
        // Foreign thread: never destroy native state. An Impl that owns a resident slot is
        // preserved in that same slot (orphaned) for owner retirement; an Impl with no slot owns no
        // Vulkan objects (the command resources are created lazily under a slot) and can be
        // destroyed here.
        if (impl_->residentSlot != kUploadNoResidentSlot) {
            impl_->orphanResidentSlot();
            (void)impl_.release();
        } else {
            impl_.reset();
        }
        return;
    }
    // Owner thread: prove retirement if needed, then free native resources and return the slot. An
    // unproven submission is retained in the bounded pool for a later owner drain rather than
    // destroyed in flight.
    if (impl_->residentSlot != kUploadNoResidentSlot) {
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

GpuImageUploadJobState GpuImageUpload::state() const noexcept {
    return impl_ != nullptr ? impl_->jobState : GpuImageUploadJobState::Idle;
}

const GpuImageUploadDiagnostic& GpuImageUpload::diagnostic() const noexcept {
    static const GpuImageUploadDiagnostic none{};
    if (impl_ != nullptr) {
        return impl_->jobDiagnostic;
    }
    return none;
}

bool GpuImageUpload::isBoundTo(GpuDevice& device) const noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const auto deviceState = GpuRendererAccess::state(device);
    return deviceState != nullptr && deviceState == impl_->control;
}

bool GpuImageUpload::hasUnretiredSubmission() const noexcept {
    return impl_ != nullptr && impl_->queueSubmitted;
}

std::uint64_t GpuImageUpload::lastJobAllocationBytes() const noexcept {
    return impl_ != nullptr ? impl_->lastJobBytes : 0;
}

bool GpuImageUpload::Impl::createResources() {
    const VkDevice rawDevice = static_cast<VkDevice>(*control->device);
    const auto* dispatcher = control->device.getDispatcher();

    vk::CommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    commandPoolInfo.queueFamilyIndex = control->computeQueueFamily;
    VkCommandPool rawCommandPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateCommandPool(
            rawDevice, reinterpret_cast<const VkCommandPoolCreateInfo*>(&commandPoolInfo), nullptr,
            &rawCommandPool) != VK_SUCCESS) {
        createDiagnostic = uploadDiagnostic(GpuImageUploadDiagnosticCode::AllocationFailed,
                                            "the upload command pool could not be created");
        return false;
    }
    commandPool = vk::raii::CommandPool(control->device, rawCommandPool);

    vk::CommandBufferAllocateInfo commandBufferInfo{};
    commandBufferInfo.commandPool = *commandPool;
    commandBufferInfo.level = vk::CommandBufferLevel::ePrimary;
    commandBufferInfo.commandBufferCount = 1;
    VkCommandBuffer rawCommandBuffer = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateCommandBuffers(
            rawDevice, reinterpret_cast<const VkCommandBufferAllocateInfo*>(&commandBufferInfo),
            &rawCommandBuffer) != VK_SUCCESS) {
        createDiagnostic = uploadDiagnostic(GpuImageUploadDiagnosticCode::AllocationFailed,
                                            "the upload command buffer could not be allocated");
        return false;
    }
    commandBuffer = vk::raii::CommandBuffer(control->device, rawCommandBuffer, *commandPool);

    vk::FenceCreateInfo fenceInfo{};
    VkFence rawFence = VK_NULL_HANDLE;
    if (dispatcher->vkCreateFence(rawDevice, reinterpret_cast<const VkFenceCreateInfo*>(&fenceInfo),
                                  nullptr, &rawFence) != VK_SUCCESS) {
        createDiagnostic = uploadDiagnostic(GpuImageUploadDiagnosticCode::AllocationFailed,
                                            "the upload fence could not be created");
        return false;
    }
    fence = vk::raii::Fence(control->device, rawFence);
    return true;
}

GpuImageUploadCreateResult GpuImageUpload::create(GpuDevice& device,
                                                  const GpuImageUploadBudgets& budgets) {
    if (budgets.maxImageBytes == 0 || budgets.maxStagingBytes == 0) {
        return {nullptr, uploadDiagnostic(GpuImageUploadDiagnosticCode::InvalidArgument,
                                          "the upload budget is out of range")};
    }
    if (device.state() != GpuDeviceState::Ready) {
        return {nullptr, uploadDiagnostic(GpuImageUploadDiagnosticCode::DeviceUnavailable,
                                          "the GPU device is not Ready")};
    }
    if (GpuRendererAccess::owner(device) != std::this_thread::get_id()) {
        return {nullptr, uploadDiagnostic(GpuImageUploadDiagnosticCode::WrongThread,
                                          "the upload pipeline must be created on the device owner "
                                          "thread")};
    }
    // Retire orphaned foreign-released residents on the owner thread so admission recovers.
    Impl::drainResidentOrphansOnOwnerThread();
    auto control = GpuRendererAccess::state(device);
    if (control == nullptr) {
        return {nullptr, uploadDiagnostic(GpuImageUploadDiagnosticCode::DeviceUnavailable,
                                          "the GPU device exposes no renderer state")};
    }
    // Lazy creation: an idle GpuImageUpload allocates no native resources and holds no resident
    // slot. The command resources are created on the first begin under the bounded slot, so many
    // pre-created instances are bounded by the fixed pool rather than each owning native state.
    auto impl = std::make_unique<Impl>();
    impl->owner = std::this_thread::get_id();
    impl->control = std::move(control);
    impl->budgets = budgets;
    impl->expectedGeneration = impl->control->generation;
    return {std::unique_ptr<GpuImageUpload>(new GpuImageUpload(std::move(impl))),
            GpuImageUploadDiagnostic{}};
}

GpuImageUploadDiagnostic GpuImageUpload::begin(const GpuImageUploadParameters& parameters,
                                               const std::uint64_t byteBudget) {
    if (impl_ == nullptr) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::DeviceUnavailable,
                                "the upload pipeline is not initialized");
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::WrongThread,
                                "begin must run on the device owner thread");
    }
    // Opportunistic, non-blocking retirement of orphaned foreign-released residents.
    Impl::drainResidentOrphansOnOwnerThread();
    if (impl.deviceLost) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::DeviceLost,
                                "the device was lost; this generation must not be reused");
    }
    if (impl.queueSubmitted || impl.jobState == GpuImageUploadJobState::Pending) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::Busy, "one job is already in flight");
    }
    if (parameters.source == nullptr) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::InvalidArgument,
                                "the upload source is null");
    }
    const Rgba32fImage& source = *parameters.source;
    const Rgba32fImageDescriptor* const descriptor = source.descriptor();
    if (descriptor == nullptr) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::InvalidArgument,
                                "the upload source has no descriptor");
    }
    const std::uint32_t width = descriptor->dataWindow().extent().width();
    const std::uint32_t height = descriptor->dataWindow().extent().height();
    if (width == 0 || height == 0) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::InvalidArgument,
                                "the upload source data window is empty");
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (source.pixels().size() != pixels) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::InvalidArgument,
                                "the upload source pixel count does not match its data window");
    }
    const std::uint64_t imageBytes = pixels * sizeof(Rgba32f);
    if (imageBytes > impl.budgets.maxImageBytes) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::OverBudget,
                                "the resident image exceeds the configured byte budget");
    }
    if (imageBytes > impl.budgets.maxStagingBytes) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::OverBudget,
                                "the upload staging buffer exceeds the configured byte budget");
    }
    if (imageBytes > byteBudget || imageBytes > byteBudget - imageBytes) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::OverBudget,
                                "the image plus its staging peak exceed the requested byte budget");
    }
    if (impl.control->generation != impl.expectedGeneration) {
        impl.deviceLost = true;
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::DeviceLost,
                                "the device generation changed; this pipeline must not be reused");
    }
    const SolidImageSupport support = querySolidImageSupport(*impl.control, width, height);
    if (!support.supported) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::Unsupported, support.reason);
    }
    if (imageBytes > support.maxImageBytes) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::OverBudget,
                                "the resident image exceeds the device resource limit");
    }

    // Acquire the bounded resident slot BEFORE the first native allocation, and create the command
    // resources lazily under it. A full pool refuses cleanly without allocating anything.
    if (!impl.acquireResidentSlot()) {
        return uploadDiagnostic(
            GpuImageUploadDiagnosticCode::DeviceUnavailable,
            "the bounded upload resident pool is full; no native resources were "
            "allocated");
    }
    if (!impl.resourcesReady) {
        if (!impl.createResources()) {
            impl.resetResources();
            impl.releaseResidentSlot();
            return impl.createDiagnostic;
        }
        impl.resourcesReady = true;
    }

    impl.clearJob();
    try {
        if (!createUploadStagingBuffer(*impl.control, imageBytes, impl.staging)) {
            impl.fail(GpuImageUploadDiagnosticCode::AllocationFailed,
                      "the upload staging buffer could not be allocated");
            return impl.jobDiagnostic;
        }
        VmaAllocationInfo stagingInfo{};
        vmaGetAllocationInfo(impl.control->allocator, impl.staging.allocation, &stagingInfo);
        if (stagingInfo.pMappedData == nullptr) {
            impl.staging.release();
            impl.fail(GpuImageUploadDiagnosticCode::AllocationFailed,
                      "the upload staging buffer could not be mapped");
            return impl.jobDiagnostic;
        }
        std::memcpy(stagingInfo.pMappedData, source.pixels().data(),
                    static_cast<std::size_t>(imageBytes));
        if (vmaFlushAllocation(impl.control->allocator, impl.staging.allocation, 0,
                               VK_WHOLE_SIZE) != VK_SUCCESS) {
            impl.staging.release();
            impl.fail(GpuImageUploadDiagnosticCode::AllocationFailed,
                      "the upload staging buffer could not be flushed");
            return impl.jobDiagnostic;
        }

        auto resident = std::make_unique<GpuImageImpl>();
        resident->state = impl.control;
        resident->dataWindow = descriptor->dataWindow();
        resident->displayWindow = descriptor->displayWindow();
        resident->pixelAspect = descriptor->pixelAspect();
        resident->generation = impl.control->generation;
        if (!createResidentImage(*impl.control, width, height, *resident)) {
            impl.staging.release();
            impl.fail(GpuImageUploadDiagnosticCode::AllocationFailed,
                      "the resident image could not be allocated");
            return impl.jobDiagnostic;
        }
        GpuImageImpl* const residentRaw = resident.get();

        // Authoritative budget check against the ACTUAL VMA allocation sizes, including allocator
        // rounding, for both the retained image and the peak with staging still alive.
        const std::uint64_t actualImage =
            uploadAllocationBytes(*impl.control, resident->allocation);
        const std::uint64_t actualStaging =
            uploadAllocationBytes(*impl.control, impl.staging.allocation);
        if (actualImage > impl.budgets.maxImageBytes ||
            actualStaging > impl.budgets.maxStagingBytes || actualImage > byteBudget ||
            actualStaging > byteBudget - actualImage) {
            impl.staging.release();
            impl.fail(GpuImageUploadDiagnosticCode::OverBudget,
                      "actual VMA allocation sizes exceed the configured or requested byte budget");
            return impl.jobDiagnostic;
        }
        // Peak retained by this job while the staging buffer is still referenced.
        impl.lastJobBytes = actualImage + actualStaging;

        impl.residentImage = std::make_unique<GpuImage>(makeGpuImage(std::move(resident)));

        const VkDevice rawDevice = static_cast<VkDevice>(*impl.control->device);
        const auto* dispatcher = impl.control->device.getDispatcher();
        const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*impl.commandBuffer);
        const VkFence rawFence = static_cast<VkFence>(*impl.fence);
        if (dispatcher->vkResetFences(rawDevice, 1, &rawFence) != VK_SUCCESS ||
            dispatcher->vkResetCommandBuffer(rawCommandBuffer, 0) != VK_SUCCESS) {
            impl.releaseResident();
            impl.staging.release();
            impl.fail(GpuImageUploadDiagnosticCode::DeviceUnavailable,
                      "the upload fence or command buffer could not be reset");
            return impl.jobDiagnostic;
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (dispatcher->vkBeginCommandBuffer(rawCommandBuffer, &beginInfo) != VK_SUCCESS) {
            impl.releaseResident();
            impl.staging.release();
            impl.fail(GpuImageUploadDiagnosticCode::DeviceUnavailable,
                      "the upload command buffer could not begin");
            return impl.jobDiagnostic;
        }

        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = residentRaw->image;
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                         1, &toTransfer);

        VkBufferImageCopy copy{};
        copy.bufferOffset = 0;
        copy.bufferRowLength = 0;
        copy.bufferImageHeight = 0;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {0, 0, 0};
        copy.imageExtent = {width, height, 1};
        dispatcher->vkCmdCopyBufferToImage(rawCommandBuffer, impl.staging.buffer,
                                           residentRaw->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           1, &copy);

        VkImageMemoryBarrier toGeneral = toTransfer;
        toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                         nullptr, 1, &toGeneral);

        if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
            impl.releaseResident();
            impl.staging.release();
            impl.fail(GpuImageUploadDiagnosticCode::DeviceUnavailable,
                      "the upload command buffer could not end");
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
            impl.staging.release();
            impl.fail(GpuImageUploadDiagnosticCode::DeviceLost,
                      "the device was lost during submission");
            return impl.jobDiagnostic;
        }
        if (submitted != VK_SUCCESS) {
            impl.releaseResident();
            impl.staging.release();
            impl.fail(GpuImageUploadDiagnosticCode::DeviceUnavailable,
                      "the upload copy could not submit");
            return impl.jobDiagnostic;
        }
        impl.queueSubmitted = true;
        impl.jobState = GpuImageUploadJobState::Pending;
        impl.jobDiagnostic = GpuImageUploadDiagnostic{};
        return {};
    } catch (const std::bad_alloc&) {
        impl.releaseResident();
        impl.staging.release();
        impl.fail(GpuImageUploadDiagnosticCode::AllocationFailed,
                  "the upload job could not be allocated");
        return impl.jobDiagnostic;
    } catch (...) {
        impl.releaseResident();
        impl.staging.release();
        impl.fail(GpuImageUploadDiagnosticCode::DeviceUnavailable,
                  "the upload job failed unexpectedly");
        return impl.jobDiagnostic;
    }
}

bool GpuImageUpload::Impl::drainAndRetire() noexcept {
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

GpuImageUploadPollResult GpuImageUpload::poll() {
    if (impl_ == nullptr) {
        return GpuImageUploadPollResult::Failure;
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return GpuImageUploadPollResult::WrongThread;
    }
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
    // TEST-ONLY fault hook, inert in every production build. It lets the executor retirement tests
    // observe a real submitted upload as stalled, device-lost or unproven against the real fence.
    if (const auto injected = gpu_scene_executor_fault::take();
        injected != gpu_scene_executor_fault::PollFault::None) {
        if (injected == gpu_scene_executor_fault::PollFault::StallPending) {
            return GpuImageUploadPollResult::Pending;
        }
        if (injected == gpu_scene_executor_fault::PollFault::DeviceLost) {
            // Bounded wait proves the REAL submission retired before pretending loss. Only
            // VK_SUCCESS may clear the submission or release the staging buffer; a timeout or an
            // unknown wait result must preserve them and fail safe, because the fence is not proven
            // signalled and the queue may still reference the staging bytes.
            const VkFence faultFence = static_cast<VkFence>(*impl.fence);
            const VkResult faultWait = impl.control->device.getDispatcher()->vkWaitForFences(
                static_cast<VkDevice>(*impl.control->device), 1, &faultFence, VK_TRUE,
                1'000'000'000ULL);
            if (faultWait == VK_SUCCESS) {
                impl.deviceLost = true;
                impl.queueSubmitted = false;
                impl.staging.release();
                impl.fail(GpuImageUploadDiagnosticCode::DeviceLost,
                          "injected device loss after proven retirement");
            } else {
                impl.fail(GpuImageUploadDiagnosticCode::DeviceUnavailable,
                          "injected device loss could not prove fence retirement; the submission "
                          "is retained");
            }
            return GpuImageUploadPollResult::Failure;
        }
        impl.fail(GpuImageUploadDiagnosticCode::DeviceUnavailable,
                  "injected unknown fence status; the submission is not retired");
        return GpuImageUploadPollResult::Failure;
    }
#endif
    if (impl.jobState == GpuImageUploadJobState::Ready) {
        return GpuImageUploadPollResult::Ready;
    }
    const bool pending = impl.jobState == GpuImageUploadJobState::Pending;
    if (!pending && !impl.queueSubmitted) {
        return GpuImageUploadPollResult::Failure;
    }
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    const VkResult status = impl.control->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*impl.control->device), rawFence);
    if (status == VK_NOT_READY) {
        return pending ? GpuImageUploadPollResult::Pending : GpuImageUploadPollResult::Failure;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        impl.queueSubmitted = false;
        impl.staging.release();
        if (pending) {
            impl.fail(GpuImageUploadDiagnosticCode::DeviceLost,
                      "the device was lost while polling");
        }
        return GpuImageUploadPollResult::Failure;
    }
    if (status != VK_SUCCESS) {
        // Unknown status: the fence is NOT proved signalled. Staging stays retained with the job.
        if (pending) {
            impl.fail(GpuImageUploadDiagnosticCode::DeviceUnavailable,
                      "the upload fence returned an unexpected status; the submission is not "
                      "retired");
        }
        return GpuImageUploadPollResult::Failure;
    }
    impl.queueSubmitted = false;
    // The copy is proved complete, so the staging bytes are no longer referenced.
    impl.staging.release();
    if (!pending) {
        return GpuImageUploadPollResult::Failure;
    }
    if (impl.discardRequested.load()) {
        impl.clearJob();
        impl.fail(GpuImageUploadDiagnosticCode::Cancelled, "the upload job was cancelled");
        return GpuImageUploadPollResult::Failure;
    }
    if (impl.residentImage == nullptr) {
        impl.fail(GpuImageUploadDiagnosticCode::DeviceUnavailable, "the resident image is missing");
        return GpuImageUploadPollResult::Failure;
    }
    impl.jobState = GpuImageUploadJobState::Ready;
    impl.jobDiagnostic = GpuImageUploadDiagnostic{};
    return GpuImageUploadPollResult::Ready;
}

const GpuImage* GpuImageUpload::image() const noexcept {
    if (impl_ == nullptr || impl_->jobState != GpuImageUploadJobState::Ready) {
        return nullptr;
    }
    return impl_->residentImage.get();
}

GpuImage GpuImageUpload::takeImage() noexcept {
    // Only a Ready job may be taken. While a job is Pending, its resident image and staging buffer
    // are still referenced by an unretired submission; refusing here leaves the job, its queue
    // submission and its retirement state untouched (mirrors GpuComposite::takeImage).
    if (impl_ == nullptr || impl_->residentImage == nullptr ||
        impl_->jobState != GpuImageUploadJobState::Ready) {
        return GpuImage{};
    }
    GpuImage taken = std::move(*impl_->residentImage);
    impl_->residentImage.reset();
    impl_->clearJob();
    return taken;
}

GpuImageReadback GpuImageUpload::readback() noexcept {
    if (impl_ == nullptr || impl_->residentImage == nullptr) {
        GpuImageReadback result;
        result.code = GpuImageReadbackCode::DeviceUnavailable;
        result.message = "no resident image to read back";
        return result;
    }
    return readbackResidentImage(*impl_->residentImage, impl_->budgets.maxImageBytes);
}

void GpuImageUpload::cancel() noexcept {
    if (impl_ != nullptr) {
        impl_->discardRequested.store(true);
    }
}

bool GpuImageUpload::teardownDrainIncomplete() noexcept {
    // Recoverable pressure, not a permanent fuse: true while a foreign-released or unproven
    // resident is retained in the bounded pool, and false again once the rightful owner drains it.
    return upload_detail::uploadResidentOrphaned() > 0;
}

} // namespace bloom::render
