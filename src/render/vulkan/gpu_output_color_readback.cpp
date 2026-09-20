#include <bloom/render/gpu_output_color_readback.hpp>

#include "gpu_device_private.hpp"
#include "gpu_image_private.hpp"
#include "gpu_output_color_readback_fault.hpp"
#include "gpu_output_color_readback_private.hpp"
#include "gpu_resident_display_private.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::render {

namespace output_color_readback_detail {

std::atomic<std::uint8_t>& readbackFault() noexcept {
    static std::atomic<std::uint8_t> fault{static_cast<std::uint8_t>(ReadbackFault::None)};
    return fault;
}

bool outputColorQuarantineOccupied() noexcept { return quarantineOccupied(); }

bool outputColorRetireQuarantineForOwner() noexcept { return retireQuarantineForOwner(); }

} // namespace output_color_readback_detail

namespace {

using output_color_readback_detail::checkedImageBytes;
using output_color_readback_detail::ReadbackFault;
using output_color_readback_detail::recordRgba32fCopy;
using output_color_readback_detail::recordRgba8Copy;
using output_color_readback_detail::Reservation;
using output_color_readback_detail::ReservationState;
using output_color_readback_detail::StagingBuffer;
using vulkan_detail::DeviceAllocatorState;

// Hard ceiling for one owner-thread readback submission before it is quarantined.
constexpr std::uint64_t kReadbackDeadlineNanoseconds = 5ULL * 1000ULL * 1000ULL * 1000ULL;

[[nodiscard]] ReadbackFault activeFault() noexcept {
    return static_cast<ReadbackFault>(output_color_readback_detail::readbackFault().load());
}

} // namespace

struct GpuOutputColorReadback::Impl final {
    GpuOutputColorReadbackState state = GpuOutputColorReadbackState::Idle;
    GpuOutputColorReadbackDiagnostic diagnostic;

    std::shared_ptr<DeviceAllocatorState> deviceState;
    std::shared_ptr<const GpuImage> process;
    std::shared_ptr<const GpuImage> encodedProcess;
    std::shared_ptr<const GpuDisplayImage> encodedDisplay;

    StagingBuffer staging;
    vk::raii::CommandPool pool{nullptr};
    vk::raii::CommandBuffer buffer{nullptr};
    vk::raii::Fence fence{nullptr};

    std::uint64_t processBytes = 0;
    std::uint64_t encodedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t payloadCount = 0;
    bool encodedIsDisplay = false;

    std::vector<Rgba32f> processPixels;
    std::vector<Rgba32f> encodedRgba32fPixels;
    std::vector<Rgba8> encodedRgba8Pixels;

    bool cancelRequested = false;
    bool submitted = false;
    std::uint64_t slotToken = 0;
    std::thread::id ownerThread{};
    std::chrono::steady_clock::time_point submittedAt{};
    GpuOutputColorReadbackCounters counters;

    // Frees local native resources and returns a proven-retired claim to Free. Owner-thread only.
    void releaseRetired() noexcept {
        staging.release();
        fence = vk::raii::Fence{nullptr};
        buffer = vk::raii::CommandBuffer{nullptr};
        pool = vk::raii::CommandPool{nullptr};
        encodedDisplay.reset();
        encodedProcess.reset();
        process.reset();
        if (slotToken != 0) {
            std::lock_guard lock(output_color_readback_detail::reservationMutex());
            Reservation& slot = output_color_readback_detail::reservation();
            if (output_color_readback_detail::isReservationOwnerLocked(slot) &&
                slot.token == slotToken) {
                slot.state = ReservationState::Free;
                slot.ownerThread = std::thread::id{};
            }
            slotToken = 0;
        }
        deviceState.reset();
        submitted = false;
    }

    // Returns a Reserved claim taken before submission. Owner-thread only. Never frees another
    // owner's Quarantined resources.
    void releaseClaim() noexcept {
        if (slotToken == 0) {
            return;
        }
        std::lock_guard lock(output_color_readback_detail::reservationMutex());
        Reservation& slot = output_color_readback_detail::reservation();
        if (output_color_readback_detail::isReservationOwnerLocked(slot) &&
            slot.token == slotToken) {
            slot.state = ReservationState::Free;
            slot.ownerThread = std::thread::id{};
        }
        slotToken = 0;
    }

    // Transfers a possibly in-flight submission into the reservation as Quarantined. Owner-thread
    // only. On success the Impl becomes empty; the reservation retains the exact pins.
    bool quarantine() noexcept {
        if (!submitted) {
            releaseRetired();
            return true;
        }
        output_color_readback_detail::outputColorReadbackFuse().store(true);
        std::lock_guard lock(output_color_readback_detail::reservationMutex());
        Reservation& slot = output_color_readback_detail::reservation();
        if (!output_color_readback_detail::isReservationOwnerLocked(slot) ||
            slot.token != slotToken) {
            return false;
        }
        slot.deviceState = std::move(deviceState);
        slot.process = std::move(process);
        slot.encodedProcess = std::move(encodedProcess);
        slot.encodedDisplay = std::move(encodedDisplay);
        slot.staging = std::move(staging);
        slot.pool = std::move(pool);
        slot.buffer = std::move(buffer);
        slot.fence = std::move(fence);
        slot.state = ReservationState::Quarantined;
        deviceState.reset();
        process.reset();
        encodedProcess.reset();
        encodedDisplay.reset();
        staging = {};
        pool = vk::raii::CommandPool{nullptr};
        buffer = vk::raii::CommandBuffer{nullptr};
        fence = vk::raii::Fence{nullptr};
        slotToken = 0;
        submitted = false;
        return true;
    }

    void setFailure(const GpuOutputColorReadbackCode code,
                    const std::string_view message) noexcept {
        state = GpuOutputColorReadbackState::Failure;
        diagnostic.code = code;
        try {
            diagnostic.message.assign(message);
        } catch (...) {
            diagnostic.message.clear();
        }
    }

    void fail(const GpuOutputColorReadbackCode code, const std::string_view message) noexcept {
        setFailure(code, message);
        if (!submitted) {
            releaseClaim();
        }
    }
};

GpuOutputColorReadback::GpuOutputColorReadback() : impl_(std::make_unique<Impl>()) {}

GpuOutputColorReadback::~GpuOutputColorReadback() {
    if (impl_ == nullptr) {
        return;
    }
    // Only the owner thread may touch native resources. A destructor on a foreign thread while a
    // submission is live retains the whole Impl (bounded by the one occupied reservation) rather
    // than destroying a possibly in-flight submission from the wrong thread.
    if (impl_->submitted && impl_->ownerThread != std::this_thread::get_id()) {
        [[maybe_unused]] const auto* const retained = impl_.release();
        return;
    }
    if (impl_->submitted) {
        cancel();
        const auto deadline =
            impl_->submittedAt + std::chrono::nanoseconds(kReadbackDeadlineNanoseconds);
        while (impl_->submitted && impl_->state == GpuOutputColorReadbackState::Pending &&
               std::chrono::steady_clock::now() < deadline) {
            static_cast<void>(poll());
            if (impl_->submitted && impl_->state == GpuOutputColorReadbackState::Pending) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (impl_->submitted) {
            // Could not prove retirement: quarantine the exact submission; never an unbounded leak.
            if (!impl_->quarantine()) {
                [[maybe_unused]] const auto* const retained = impl_.release();
            } else {
                impl_->setFailure(GpuOutputColorReadbackCode::ReadbackFailed,
                                  "the readback did not retire before destruction; retained");
            }
        }
    }
}

GpuOutputColorReadbackState GpuOutputColorReadback::state() const noexcept {
    return impl_ == nullptr ? GpuOutputColorReadbackState::Idle : impl_->state;
}

const GpuOutputColorReadbackDiagnostic& GpuOutputColorReadback::diagnostic() const noexcept {
    static const GpuOutputColorReadbackDiagnostic empty{};
    return impl_ == nullptr ? empty : impl_->diagnostic;
}

bool GpuOutputColorReadback::isOwnerThread() const noexcept {
    return impl_ != nullptr && impl_->ownerThread == std::this_thread::get_id() &&
           impl_->deviceState != nullptr;
}

bool GpuOutputColorReadback::begin(std::shared_ptr<const GpuImage> processImage,
                                   std::shared_ptr<const GpuImage> encodedProcessImage,
                                   std::optional<GpuDisplayImage> encodedDisplayImage,
                                   const std::uint64_t byteBudget) noexcept {
    try {
        if (impl_ == nullptr) {
            return false;
        }
        // A pre-submit failure leaves no native submission, so the object is reusable. A live
        // submission (submitted) or a Ready payload must be resolved first.
        if (impl_->submitted || impl_->state == GpuOutputColorReadbackState::Pending ||
            impl_->state == GpuOutputColorReadbackState::Ready) {
            return false;
        }
        impl_->state = GpuOutputColorReadbackState::Idle;
        impl_->diagnostic = {};
        if (processImage == nullptr || gpuImageImpl(*processImage) == nullptr) {
            impl_->setFailure(GpuOutputColorReadbackCode::InvalidArgument, "no process image");
            return false;
        }
        const bool hasEncodedProcess = encodedProcessImage != nullptr;
        const bool hasEncodedDisplay = encodedDisplayImage.has_value();
        if (hasEncodedProcess && hasEncodedDisplay) {
            impl_->setFailure(GpuOutputColorReadbackCode::InvalidArgument,
                              "both an RGBA32F and an RGBA8 encoded source were supplied");
            return false;
        }
        const GpuImageImpl* const processImpl = gpuImageImpl(*processImage);
        if (processImpl->image == VK_NULL_HANDLE) {
            impl_->setFailure(GpuOutputColorReadbackCode::DeviceUnavailable, "no process image");
            return false;
        }
        const std::thread::id self = std::this_thread::get_id();
        if (!processImpl->onOwnerThread()) {
            impl_->setFailure(GpuOutputColorReadbackCode::WrongThread,
                              "readback must run on the device owner thread");
            return false;
        }
        if (processImpl->deviceLost) {
            impl_->setFailure(GpuOutputColorReadbackCode::DeviceLost, "the device was lost");
            return false;
        }
        if (processImpl->submissionUnretired) {
            impl_->setFailure(GpuOutputColorReadbackCode::DeviceUnavailable,
                              "a prior readback of the process image is not retired");
            return false;
        }
        std::uint64_t processBytes = 0;
        if (!checkedImageBytes(processImpl->width, processImpl->height, sizeof(Rgba32f),
                               processBytes) ||
            processBytes > byteBudget) {
            impl_->setFailure(GpuOutputColorReadbackCode::OverBudget,
                              "the process readback exceeds the requested byte budget");
            return false;
        }

        const GpuImageImpl* encodedProcessImpl = nullptr;
        const GpuDisplayImageImpl* encodedDisplayImpl = nullptr;
        std::uint64_t encodedBytes = 0;
        std::shared_ptr<const GpuDisplayImage> displayOwner;
        if (hasEncodedProcess) {
            encodedProcessImpl = gpuImageImpl(*encodedProcessImage);
            if (encodedProcessImpl == nullptr || encodedProcessImpl->image == VK_NULL_HANDLE) {
                impl_->setFailure(GpuOutputColorReadbackCode::InvalidArgument,
                                  "the encoded RGBA32F source is not resident");
                return false;
            }
            if (!encodedProcessImpl->onOwnerThread() || encodedProcessImpl->deviceLost ||
                encodedProcessImpl->submissionUnretired) {
                impl_->setFailure(GpuOutputColorReadbackCode::WrongThread,
                                  "the encoded RGBA32F source is not readable on this thread");
                return false;
            }
            if (!checkedImageBytes(encodedProcessImpl->width, encodedProcessImpl->height,
                                   sizeof(Rgba32f), encodedBytes)) {
                impl_->setFailure(GpuOutputColorReadbackCode::InvalidArgument,
                                  "the encoded RGBA32F geometry is invalid");
                return false;
            }
        } else if (hasEncodedDisplay) {
            displayOwner = std::make_shared<GpuDisplayImage>(std::move(*encodedDisplayImage));
            encodedDisplayImpl = gpuDisplayImageImpl(*displayOwner);
            if (encodedDisplayImpl == nullptr || encodedDisplayImpl->image == VK_NULL_HANDLE) {
                impl_->setFailure(GpuOutputColorReadbackCode::InvalidArgument,
                                  "the encoded RGBA8 source is not resident");
                return false;
            }
            if (!encodedDisplayImpl->onOwnerThread() || encodedDisplayImpl->deviceLost ||
                encodedDisplayImpl->submissionUnretired) {
                impl_->setFailure(GpuOutputColorReadbackCode::WrongThread,
                                  "the encoded RGBA8 source is not readable on this thread");
                return false;
            }
            if (!checkedImageBytes(encodedDisplayImpl->width, encodedDisplayImpl->height,
                                   sizeof(Rgba8), encodedBytes)) {
                impl_->setFailure(GpuOutputColorReadbackCode::InvalidArgument,
                                  "the encoded RGBA8 geometry is invalid");
                return false;
            }
        }

        const std::uint64_t totalBytes =
            processBytes > UINT64_MAX - encodedBytes ? UINT64_MAX : processBytes + encodedBytes;
        if (totalBytes > byteBudget) {
            impl_->setFailure(GpuOutputColorReadbackCode::OverBudget,
                              "the combined readback exceeds the requested byte budget");
            return false;
        }

        auto state = processImpl->state;
        if (hasEncodedProcess && encodedProcessImpl->state.get() != state.get()) {
            impl_->setFailure(GpuOutputColorReadbackCode::InvalidArgument,
                              "the encoded source belongs to a different device");
            return false;
        }
        if (encodedDisplayImpl != nullptr && encodedDisplayImpl->state.get() != state.get()) {
            impl_->setFailure(GpuOutputColorReadbackCode::InvalidArgument,
                              "the encoded display belongs to a different device");
            return false;
        }

        // Claim the bounded reservation BEFORE any native allocation or submit, so a failure never
        // accumulates more than one occupied submission process-wide.
        {
            std::lock_guard lock(output_color_readback_detail::reservationMutex());
            Reservation& slot = output_color_readback_detail::reservation();
            if (slot.state == ReservationState::Reserved) {
                impl_->setFailure(GpuOutputColorReadbackCode::DeviceUnavailable,
                                  "the readback reservation is held by a live submission");
                return false;
            }
            if (slot.state == ReservationState::Quarantined) {
                if (slot.ownerThread != self) {
                    impl_->setFailure(GpuOutputColorReadbackCode::DeviceUnavailable,
                                      "a foreign owner thread holds the readback quarantine");
                    return false;
                }
                if (!output_color_readback_detail::retireQuarantinedLocked(slot)) {
                    impl_->setFailure(GpuOutputColorReadbackCode::DeviceUnavailable,
                                      "a prior readback submission is not retired");
                    return false;
                }
            }
            slot.state = ReservationState::Reserved;
            slot.ownerThread = self;
            ++slot.token;
            if (slot.token == 0) {
                ++slot.token;
            }
            impl_->slotToken = slot.token;
        }

        const VkDevice device = static_cast<VkDevice>(*state->device);
        const auto* dispatcher = state->device.getDispatcher();
        if (activeFault() == ReadbackFault::FailStagingAllocation) {
            impl_->fail(GpuOutputColorReadbackCode::ReadbackFailed,
                        "the combined readback staging allocation was forced to fail");
            return false;
        }

        StagingBuffer staging;
        staging.state = state.get();
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = totalBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo stagingAllocation{};
        stagingAllocation.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAllocation.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo stagingInfo{};
        if (vmaCreateBuffer(state->allocator, &bufferInfo, &stagingAllocation, &staging.buffer,
                            &staging.allocation, &stagingInfo) != VK_SUCCESS) {
            impl_->fail(GpuOutputColorReadbackCode::ReadbackFailed,
                        "the combined readback staging buffer could not be created");
            return false;
        }
        staging.armed = true;

        const std::uint64_t actualStagingBytes = static_cast<std::uint64_t>(stagingInfo.size);
        if (actualStagingBytes > byteBudget || totalBytes > byteBudget - actualStagingBytes) {
            impl_->fail(
                GpuOutputColorReadbackCode::OverBudget,
                "the actual staging buffer plus host vectors exceed the requested byte budget");
            return false;
        }

        vk::raii::CommandPool pool{nullptr};
        {
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolInfo.queueFamilyIndex = state->computeQueueFamily;
            VkCommandPool raw = VK_NULL_HANDLE;
            if (dispatcher->vkCreateCommandPool(device, &poolInfo, nullptr, &raw) != VK_SUCCESS) {
                impl_->fail(GpuOutputColorReadbackCode::ReadbackFailed,
                            "the readback command pool could not be created");
                return false;
            }
            pool = vk::raii::CommandPool(state->device, raw);
        }
        vk::raii::CommandBuffer commandBuffer{nullptr};
        {
            VkCommandBufferAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocateInfo.commandPool = *pool;
            allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocateInfo.commandBufferCount = 1;
            VkCommandBuffer raw = VK_NULL_HANDLE;
            if (dispatcher->vkAllocateCommandBuffers(device, &allocateInfo, &raw) != VK_SUCCESS) {
                impl_->fail(GpuOutputColorReadbackCode::ReadbackFailed,
                            "the readback command buffer could not be allocated");
                return false;
            }
            commandBuffer = vk::raii::CommandBuffer(state->device, raw, *pool);
        }
        vk::raii::Fence fence{nullptr};
        {
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence raw = VK_NULL_HANDLE;
            if (dispatcher->vkCreateFence(device, &fenceInfo, nullptr, &raw) != VK_SUCCESS) {
                impl_->fail(GpuOutputColorReadbackCode::ReadbackFailed,
                            "the readback fence could not be created");
                return false;
            }
            fence = vk::raii::Fence(state->device, raw);
        }

        const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*commandBuffer);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (dispatcher->vkBeginCommandBuffer(rawCommandBuffer, &beginInfo) != VK_SUCCESS) {
            impl_->fail(GpuOutputColorReadbackCode::ReadbackFailed,
                        "the readback command buffer could not begin");
            return false;
        }
        recordRgba32fCopy(rawCommandBuffer, *processImpl, staging.buffer, 0);
        if (encodedProcessImpl != nullptr) {
            recordRgba32fCopy(rawCommandBuffer, *encodedProcessImpl, staging.buffer, processBytes);
        } else if (encodedDisplayImpl != nullptr) {
            recordRgba8Copy(rawCommandBuffer, *encodedDisplayImpl, staging.buffer, processBytes);
        }
        if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
            impl_->fail(GpuOutputColorReadbackCode::ReadbackFailed,
                        "the readback command buffer could not end");
            return false;
        }

        const VkFence rawFence = static_cast<VkFence>(*fence);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &rawCommandBuffer;
        if (activeFault() == ReadbackFault::FailSubmit) {
            impl_->fail(GpuOutputColorReadbackCode::ReadbackFailed,
                        "the readback submission was forced to fail");
            return false;
        }
        const VkResult submittedResult = dispatcher->vkQueueSubmit(
            static_cast<VkQueue>(*state->computeQueue), 1, &submit, rawFence);
        if (submittedResult == VK_ERROR_DEVICE_LOST) {
            impl_->fail(GpuOutputColorReadbackCode::DeviceLost,
                        "the device was lost during the readback submit");
            return false;
        }
        if (submittedResult != VK_SUCCESS) {
            impl_->fail(GpuOutputColorReadbackCode::ReadbackFailed,
                        "the readback submission failed");
            return false;
        }

        impl_->deviceState = std::move(state);
        impl_->process = std::move(processImage);
        if (encodedProcessImpl != nullptr) {
            impl_->encodedProcess = std::move(encodedProcessImage);
        }
        if (displayOwner != nullptr) {
            impl_->encodedDisplay = std::move(displayOwner);
        }
        impl_->staging = std::move(staging);
        impl_->pool = std::move(pool);
        impl_->buffer = std::move(commandBuffer);
        impl_->fence = std::move(fence);
        impl_->processBytes = processBytes;
        impl_->encodedBytes = encodedBytes;
        impl_->totalBytes = totalBytes;
        impl_->payloadCount = (encodedBytes > 0) ? 2U : 1U;
        impl_->encodedIsDisplay = encodedDisplayImpl != nullptr;
        impl_->cancelRequested = false;
        impl_->ownerThread = self;
        impl_->submitted = true;
        impl_->submittedAt = std::chrono::steady_clock::now();
        impl_->state = GpuOutputColorReadbackState::Pending;
        return true;
    } catch (const std::bad_alloc&) {
        impl_->fail(GpuOutputColorReadbackCode::ReadbackFailed,
                    "the readback submission state could not be allocated");
        return false;
    } catch (...) {
        impl_->fail(GpuOutputColorReadbackCode::ReadbackFailed,
                    "the readback submission failed unexpectedly");
        return false;
    }
}

GpuOutputColorReadbackState GpuOutputColorReadback::poll() noexcept {
    if (impl_ == nullptr || impl_->state != GpuOutputColorReadbackState::Pending) {
        return impl_ == nullptr ? GpuOutputColorReadbackState::Idle : impl_->state;
    }
    Impl& impl = *impl_;
    if (impl.ownerThread != std::this_thread::get_id()) {
        impl.diagnostic = {GpuOutputColorReadbackCode::WrongThread,
                           "readback poll must run on the device owner thread"};
        return GpuOutputColorReadbackState::Pending;
    }
    if (impl.deviceState == nullptr) {
        impl.fail(GpuOutputColorReadbackCode::DeviceUnavailable, "the readback lost its device");
        return impl.state;
    }
    const bool forcedTimeout = activeFault() == ReadbackFault::ForceFenceTimeout;
    const auto* dispatcher = impl.deviceState->device.getDispatcher();
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    const VkResult status = forcedTimeout
                                ? VK_NOT_READY
                                : dispatcher->vkGetFenceStatus(
                                      static_cast<VkDevice>(*impl.deviceState->device), rawFence);
    if (status == VK_NOT_READY) {
        const bool deadlineExceeded =
            forcedTimeout ||
            std::chrono::steady_clock::now() >=
                impl.submittedAt + std::chrono::nanoseconds(kReadbackDeadlineNanoseconds);
        if (deadlineExceeded) {
            const bool cancelled = impl.cancelRequested;
            if (!impl.quarantine()) {
                [[maybe_unused]] const auto* const retained = impl_.release();
                return GpuOutputColorReadbackState::Failure;
            }
            impl.setFailure(cancelled ? GpuOutputColorReadbackCode::Cancelled
                                      : GpuOutputColorReadbackCode::ReadbackFailed,
                            cancelled
                                ? "the readback was cancelled; the submission is retained until "
                                  "retired"
                                : "the readback did not retire within the deadline; the submission "
                                  "is retained");
        }
        return impl.state;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        impl.releaseRetired();
        impl.setFailure(GpuOutputColorReadbackCode::DeviceLost,
                        "the device was lost during the readback wait");
        return impl.state;
    }
    if (status != VK_SUCCESS) {
        if (!impl.quarantine()) {
            [[maybe_unused]] const auto* const retained = impl_.release();
            return GpuOutputColorReadbackState::Failure;
        }
        impl.setFailure(GpuOutputColorReadbackCode::ReadbackFailed,
                        "the readback completion is unknown; the submission was retained");
        return impl.state;
    }

    const bool cancelled = impl.cancelRequested;
    if (!cancelled) {
        if (vmaInvalidateAllocation(impl.deviceState->allocator, impl.staging.allocation, 0,
                                    impl.totalBytes) != VK_SUCCESS) {
            impl.releaseRetired();
            impl.setFailure(GpuOutputColorReadbackCode::ReadbackFailed,
                            "the readback buffer could not be invalidated");
            return impl.state;
        }
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(impl.deviceState->allocator, impl.staging.allocation, &info);
        if (info.pMappedData == nullptr) {
            impl.releaseRetired();
            impl.setFailure(GpuOutputColorReadbackCode::ReadbackFailed,
                            "the readback buffer was not host-visible");
            return impl.state;
        }
        try {
            const auto* const base = static_cast<const std::byte*>(info.pMappedData);
            impl.processPixels.resize(static_cast<std::size_t>(impl.processBytes / sizeof(Rgba32f)),
                                      Rgba32f::transparent());
            std::memcpy(impl.processPixels.data(), base,
                        static_cast<std::size_t>(impl.processBytes));
            if (impl.encodedBytes > 0) {
                const auto* const encodedBase = base + impl.processBytes;
                if (impl.encodedIsDisplay) {
                    impl.encodedRgba8Pixels.resize(
                        static_cast<std::size_t>(impl.encodedBytes / sizeof(Rgba8)),
                        Rgba8{0, 0, 0, 0});
                    std::memcpy(impl.encodedRgba8Pixels.data(), encodedBase,
                                static_cast<std::size_t>(impl.encodedBytes));
                } else {
                    impl.encodedRgba32fPixels.resize(
                        static_cast<std::size_t>(impl.encodedBytes / sizeof(Rgba32f)),
                        Rgba32f::transparent());
                    std::memcpy(impl.encodedRgba32fPixels.data(), encodedBase,
                                static_cast<std::size_t>(impl.encodedBytes));
                }
            }
        } catch (const std::bad_alloc&) {
            impl.releaseRetired();
            impl.setFailure(GpuOutputColorReadbackCode::ReadbackFailed,
                            "the readback host buffers could not be allocated");
            return impl.state;
        }
    }
    impl.counters = {1, impl.payloadCount, impl.totalBytes, impl.processBytes, impl.encodedBytes};
    impl.releaseRetired();
    if (cancelled) {
        impl.setFailure(GpuOutputColorReadbackCode::Cancelled,
                        "the readback was cancelled; no payload was published");
    } else {
        impl.state = GpuOutputColorReadbackState::Ready;
        impl.diagnostic = {};
    }
    return impl.state;
}

GpuOutputColorReadbackPayloads GpuOutputColorReadback::take() noexcept {
    GpuOutputColorReadbackPayloads payloads;
    if (impl_ == nullptr || impl_->state != GpuOutputColorReadbackState::Ready) {
        return payloads;
    }
    payloads.process = std::move(impl_->processPixels);
    payloads.encodedRgba32f = std::move(impl_->encodedRgba32fPixels);
    payloads.encodedRgba8 = std::move(impl_->encodedRgba8Pixels);
    payloads.counters = impl_->counters;
    impl_->processPixels.clear();
    impl_->encodedRgba32fPixels.clear();
    impl_->encodedRgba8Pixels.clear();
    impl_->counters = {};
    impl_->state = GpuOutputColorReadbackState::Idle;
    impl_->diagnostic = {};
    return payloads;
}

void GpuOutputColorReadback::cancel() noexcept {
    if (impl_ != nullptr && impl_->state == GpuOutputColorReadbackState::Pending) {
        impl_->cancelRequested = true;
    }
}

bool GpuOutputColorReadback::hasUnretiredSubmission() const noexcept {
    return impl_ != nullptr && impl_->submitted;
}

} // namespace bloom::render
