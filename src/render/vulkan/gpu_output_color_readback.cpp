#include <bloom/render/gpu_output_color_readback.hpp>

#include "gpu_device_private.hpp"
#include "gpu_image_private.hpp"
#include "gpu_native_buffer_allocation_private.hpp"
#include "gpu_output_color_readback_fault.hpp"
#include "gpu_output_color_readback_private.hpp"
#include "gpu_output_color_readback_retirement_private.hpp"
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

void drainReadbacks(DeviceAllocatorState& state) noexcept {
    if (state.owner == std::this_thread::get_id() &&
        state.ownerLifetimeToken == vulkan_detail::deviceOwnerLifetimeToken())
        static_cast<void>(retireQuarantine(state));
}
void closeReadbacks(DeviceAllocatorState& state) noexcept {
    drainReadbacks(state);
    std::lock_guard lock(reservationMutex());
    auto& slot = reservation();
    if (slotMatches(slot, state))
        slot.ownerClosed = true;
}
bool readbackPendingFor(const DeviceAllocatorState& state) noexcept {
    std::lock_guard lock(reservationMutex());
    return slotMatches(reservation(), state);
}

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

    FinalRgba32fStorage processPixels;
    FinalRgba32fStorage encodedRgba32fPixels;
    FinalRgba8Storage encodedRgba8Pixels;

    std::shared_ptr<GpuProcessingPhaseLease> phaseLease;
    std::uint64_t hostByteLimit = 0;

    bool cancelRequested = false;
    bool submitted = false;
    std::uint64_t slotToken = 0;
    std::uint64_t quarantinedToken = 0;
    std::uint64_t ownerLifetimeToken = 0;
    std::thread::id ownerThread{};
    std::chrono::steady_clock::time_point submittedAt{};
    GpuOutputColorReadbackCounters counters;

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return ownerThread == std::this_thread::get_id() && ownerLifetimeToken != 0 &&
               ownerLifetimeToken == vulkan_detail::deviceOwnerLifetimeToken() &&
               (!deviceState || deviceState->ownerLifetimeToken == ownerLifetimeToken);
    }

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
        phaseLease.reset();
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
    bool quarantine(const bool unknown = true) noexcept {
        if (!submitted) {
            releaseRetired();
            return true;
        }
        std::lock_guard lock(output_color_readback_detail::reservationMutex());
        Reservation& slot = output_color_readback_detail::reservation();
        if (!deviceState || slot.ownerClosed || slot.state != ReservationState::Reserved ||
            slot.token != slotToken ||
            !output_color_readback_detail::slotMatches(slot, *deviceState)) {
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
        if (unknown)
            output_color_readback_detail::outputColorReadbackFuse().store(true);
        quarantinedToken = slotToken;
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
    if (!impl_->submitted)
        return;
    if (!impl_->onOwnerThread()) {
        if (!impl_->quarantine(false)) {
            [[maybe_unused]] const auto* const retained = impl_.release();
        }
        return;
    }
    cancel();
    static_cast<void>(poll());
    if (impl_->submitted && !impl_->quarantine()) {
        [[maybe_unused]] const auto* const retained = impl_.release();
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
    return impl_ != nullptr && impl_->onOwnerThread() && impl_->deviceState != nullptr;
}

bool GpuOutputColorReadback::begin(std::shared_ptr<const GpuImage> processImage,
                                   std::shared_ptr<const GpuImage> encodedProcessImage,
                                   std::optional<GpuDisplayImage> encodedDisplayImage,
                                   const std::uint64_t byteBudget,
                                   const std::shared_ptr<GpuProcessingPhaseLease>& phase) noexcept {
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
        if (phase != processImpl->state->processingPhase ||
            processImpl->state->processingPhaseUnproven || (phase && phase->isUnproven())) {
            impl_->setFailure(GpuOutputColorReadbackCode::DeviceUnavailable,
                              "readback requires the exact proven retained processing phase");
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
        if (totalBytes > byteBudget / 2) {
            impl_->setFailure(GpuOutputColorReadbackCode::OverBudget,
                              "the combined readback exceeds the requested byte budget");
            return false;
        }

        const std::uint64_t hostMetadata =
            phase ? detail::packedHostAllocationMetadataBytes() * (encodedBytes > 0 ? 2U : 1U) : 0;
        if (hostMetadata > byteBudget - totalBytes ||
            totalBytes > byteBudget - totalBytes - hostMetadata) {
            impl_->setFailure(GpuOutputColorReadbackCode::OverBudget,
                              "the actual host payload headers exceed the readback budget");
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

        static_cast<void>(output_color_readback_detail::retireQuarantine(*state));
        // One preowned slot, exact generation and owner lifetime; Reserved is never pollable.
        {
            std::lock_guard lock(output_color_readback_detail::reservationMutex());
            Reservation& slot = output_color_readback_detail::reservation();
            if (slot.state != ReservationState::Free) {
                impl_->setFailure(GpuOutputColorReadbackCode::DeviceUnavailable,
                                  "the combined readback slot is still owned");
                return false;
            }
            slot.state = ReservationState::Reserved;
            slot.ownerThread = self;
            slot.ownerLifetimeToken = state->ownerLifetimeToken;
            slot.epoch = state->presentationEpoch;
            slot.binding = state;
            slot.ownerClosed = false;
            ++slot.token;
            if (slot.token == 0)
                ++slot.token;
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
        const auto allocationOutcome = vulkan_detail::allocateCoveredBuffer(
            *state, bufferInfo, stagingAllocation, byteBudget - totalBytes - hostMetadata,
            staging.buffer, staging.allocation, stagingInfo, staging.storage);
        if (allocationOutcome != vulkan_detail::NativeBufferAllocationOutcome::Allocated) {
            const bool budget =
                allocationOutcome == vulkan_detail::NativeBufferAllocationOutcome::PhaseRefused ||
                allocationOutcome ==
                    vulkan_detail::NativeBufferAllocationOutcome::TransferBudgetRefused;
            impl_->fail(budget ? GpuOutputColorReadbackCode::OverBudget
                               : GpuOutputColorReadbackCode::ReadbackFailed,
                        budget
                            ? "the admitted native/planned-host staging or readback limit refused"
                            : "the combined readback staging buffer could not be created");
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
        // Make every copied destination byte visible to the host before fence retirement
        // and the subsequent allocation-relative invalidate.
        VkBufferMemoryBarrier toHost{};
        toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.buffer = staging.buffer;
        toHost.size = totalBytes;
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0,
                                         nullptr);

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
        impl_->phaseLease = phase;
        impl_->hostByteLimit = byteBudget;
        impl_->processBytes = processBytes;
        impl_->encodedBytes = encodedBytes;
        impl_->totalBytes = totalBytes;
        impl_->payloadCount = (encodedBytes > 0) ? 2U : 1U;
        impl_->encodedIsDisplay = encodedDisplayImpl != nullptr;
        impl_->cancelRequested = false;
        impl_->ownerThread = self;
        impl_->ownerLifetimeToken = impl_->deviceState->ownerLifetimeToken;
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
    if (!impl.onOwnerThread()) {
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
            const auto makeFloat = [&](const std::uint64_t bytes,
                                       const HostPixelStorageDomain domain) {
                const auto count = static_cast<std::size_t>(bytes / sizeof(Rgba32f));
                if (!impl.phaseLease)
                    return FinalRgba32fStorage::legacy(
                        std::vector<Rgba32f>(count, Rgba32f::transparent()));
                auto result = FinalRgba32fStorage::create(
                    count, impl.phaseLease, static_cast<std::size_t>(impl.hostByteLimit), domain);
                if (!result) {
                    if (result.error()->code == ImageErrorCode::PixelStorageBudgetExceeded)
                        throw detail::PackedHostCapacityError{};
                    throw std::bad_alloc{};
                }
                return std::move(*result.value());
            };
            impl.processPixels = makeFloat(impl.processBytes, HostPixelStorageDomain::FinalProcess);
            std::memcpy(impl.processPixels.data(), base,
                        static_cast<std::size_t>(impl.processBytes));
            if (impl.encodedBytes > 0) {
                const auto* const encodedBase = base + impl.processBytes;
                if (impl.encodedIsDisplay) {
                    const auto count = static_cast<std::size_t>(impl.encodedBytes / sizeof(Rgba8));
                    if (impl.phaseLease) {
                        auto result = FinalRgba8Storage::create(
                            count, impl.phaseLease, static_cast<std::size_t>(impl.hostByteLimit),
                            HostPixelStorageDomain::FinalEncoded);
                        if (!result) {
                            if (result.error()->code == ImageErrorCode::PixelStorageBudgetExceeded)
                                throw detail::PackedHostCapacityError{};
                            throw std::bad_alloc{};
                        }
                        impl.encodedRgba8Pixels = std::move(*result.value());
                    } else {
                        impl.encodedRgba8Pixels =
                            FinalRgba8Storage::legacy(std::vector<Rgba8>(count));
                    }
                    std::memcpy(impl.encodedRgba8Pixels.data(), encodedBase,
                                static_cast<std::size_t>(impl.encodedBytes));
                } else {
                    impl.encodedRgba32fPixels =
                        makeFloat(impl.encodedBytes, HostPixelStorageDomain::FinalEncoded);
                    std::memcpy(impl.encodedRgba32fPixels.data(), encodedBase,
                                static_cast<std::size_t>(impl.encodedBytes));
                }
            }
        } catch (const detail::PackedHostCapacityError&) {
            impl.processPixels.reset();
            impl.encodedRgba32fPixels.reset();
            impl.encodedRgba8Pixels.reset();
            impl.releaseRetired();
            impl.setFailure(GpuOutputColorReadbackCode::OverBudget,
                            "the actual final host payload capacity was refused");
            return impl.state;
        } catch (const std::bad_alloc&) {
            impl.processPixels.reset();
            impl.encodedRgba32fPixels.reset();
            impl.encodedRgba8Pixels.reset();
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

GpuOutputColorReadbackPayloads GpuOutputColorReadback::take(const bool publishStorage) noexcept {
    GpuOutputColorReadbackPayloads payloads;
    if (impl_ == nullptr || !impl_->onOwnerThread() ||
        impl_->state != GpuOutputColorReadbackState::Ready) {
        return payloads;
    }
    if (publishStorage && impl_->processPixels.authority() &&
        !PackedHostStorageAuthority::promoteFinalProduct(
            impl_->processPixels.authority(), impl_->encodedIsDisplay
                                                  ? impl_->encodedRgba8Pixels.authority()
                                                  : impl_->encodedRgba32fPixels.authority())) {
        impl_->processPixels.reset();
        impl_->encodedRgba32fPixels.reset();
        impl_->encodedRgba8Pixels.reset();
        impl_->setFailure(GpuOutputColorReadbackCode::DeviceUnavailable,
                          "the complete final host product could not be published");
        return payloads;
    }
    payloads.process = std::move(impl_->processPixels);
    payloads.encodedRgba32f = std::move(impl_->encodedRgba32fPixels);
    payloads.encodedRgba8 = std::move(impl_->encodedRgba8Pixels);
    payloads.counters = impl_->counters;
    impl_->processPixels.reset();
    impl_->encodedRgba32fPixels.reset();
    impl_->encodedRgba8Pixels.reset();
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
    if (!impl_)
        return false;
    if (impl_->submitted)
        return true;
    if (impl_->quarantinedToken == 0)
        return false;
    std::lock_guard lock(output_color_readback_detail::reservationMutex());
    const auto& slot = output_color_readback_detail::reservation();
    return slot.state != ReservationState::Free && slot.token == impl_->quarantinedToken;
}

} // namespace bloom::render
