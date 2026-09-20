#include <bloom/render/gpu_process_readback.hpp>

#include "gpu_device_private.hpp"
#include "gpu_image_private.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::render {
namespace {

using vulkan_detail::DeviceAllocatorState;

constexpr std::uint64_t kProcessReadbackDeadlineNanoseconds = 5ULL * 1000ULL * 1000ULL * 1000ULL;

struct StagingBuffer final {
    StagingBuffer() = default;
    StagingBuffer(const StagingBuffer&) = delete;
    StagingBuffer& operator=(const StagingBuffer&) = delete;
    StagingBuffer(StagingBuffer&& other) noexcept { *this = std::move(other); }
    StagingBuffer& operator=(StagingBuffer&& other) noexcept {
        if (this != &other) {
            release();
            state = other.state;
            buffer = other.buffer;
            allocation = other.allocation;
            armed = other.armed;
            other.state = nullptr;
            other.buffer = VK_NULL_HANDLE;
            other.allocation = VK_NULL_HANDLE;
            other.armed = false;
        }
        return *this;
    }
    ~StagingBuffer() { release(); }

    void release() noexcept {
        if (armed && state != nullptr) {
            vmaDestroyBuffer(state->allocator, buffer, allocation);
        }
        state = nullptr;
        buffer = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
        armed = false;
    }

    DeviceAllocatorState* state = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    bool armed = false;
};

// The bounded global process readback slot. It has exactly three states:
//   * Free        -- reusable by any owner thread.
//   * Reserved    -- owner A claimed it and its live Impl still owns every native resource. A
//                    Reservation is NOT pollable and NOT reclaimable by anyone (same or foreign):
//                    any new claim must refuse Busy. The live Impl is the sole owner until it
//                    retires or quarantines.
//   * Quarantined -- owner A transferred a possibly-in-flight submission (with its owned fence,
//                    staging, pool, buffer, source, device generation) into the slot. Only the same
//                    owner thread whose fence is proven retired may reclaim/refill it; a foreign
//                    thread may never make a driver call or destroy it.
// `ownerThread` is the authoritative identity. The slot NEVER duplicates ownership metadata to
// pretend a live Reservation is Quarantined.
enum class ProcessReadbackSlotState : std::uint8_t {
    Free,
    Reserved,
    Quarantined,
};

struct ProcessReadbackSlot final {
    ProcessReadbackSlotState slotState = ProcessReadbackSlotState::Free;
    std::thread::id ownerThread{};
    std::uint64_t token = 0;

    std::shared_ptr<DeviceAllocatorState> state;
    std::shared_ptr<const GpuImage> source;
    StagingBuffer staging;
    vk::raii::CommandPool pool{nullptr};
    vk::raii::CommandBuffer buffer{nullptr};
    vk::raii::Fence fence{nullptr};
};

ProcessReadbackSlot& processSlot() {
    static auto* const slot = new ProcessReadbackSlot();
    return *slot;
}

std::mutex& processSlotMutex() {
    static auto* const mutex = new std::mutex();
    return *mutex;
}

std::atomic<bool>& processReadbackFuse() {
    static std::atomic<bool> fuse{false};
    return fuse;
}

// Frees a Quarantined slot's native resources. The caller must hold the mutex and must have
// established that this thread is the slot owner (or the device is lost); a Reserved slot is owned
// by a live Impl and is never freed here.
void freeQuarantinedSlotLocked(ProcessReadbackSlot& slot) noexcept {
    slot.staging.release();
    slot.fence = vk::raii::Fence{nullptr};
    slot.buffer = vk::raii::CommandBuffer{nullptr};
    slot.pool = vk::raii::CommandPool{nullptr};
    slot.source.reset();
    slot.state.reset();
    slot.slotState = ProcessReadbackSlotState::Free;
    slot.ownerThread = std::thread::id{};
}

[[nodiscard]] bool isSlotOwnerLocked(const ProcessReadbackSlot& slot) noexcept {
    return slot.slotState != ProcessReadbackSlotState::Free &&
           slot.ownerThread == std::this_thread::get_id();
}

// Best-effort non-blocking retirement of a quarantined slot. Only the slot owner may call this (the
// caller holds the mutex and has checked the owner thread). Returns true when the fence is proven
// signalled or the device is lost, in which case the resources are freed.
[[nodiscard]] bool retireQuarantinedSlotLocked(ProcessReadbackSlot& slot) noexcept {
    if (slot.slotState != ProcessReadbackSlotState::Quarantined || slot.state == nullptr ||
        slot.fence == vk::raii::Fence{nullptr}) {
        return false;
    }
    const VkFence rawFence = static_cast<VkFence>(*slot.fence);
    const VkResult status = slot.state->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*slot.state->device), rawFence);
    if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) {
        return false;
    }
    freeQuarantinedSlotLocked(slot);
    return true;
}

[[nodiscard]] bool checkedReadbackBytes(const std::uint32_t width, const std::uint32_t height,
                                        std::uint64_t& out) noexcept {
    constexpr std::uint64_t kComponentBytes = sizeof(Rgba32f);
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (width == 0 || height == 0) {
        return false;
    }
    if (pixels > UINT64_MAX / kComponentBytes) {
        return false;
    }
    out = pixels * kComponentBytes;
    return true;
}

} // namespace

struct GpuProcessReadback::Impl final {
    GpuProcessReadbackState state = GpuProcessReadbackState::Idle;
    GpuProcessReadbackDiagnostic diagnostic;

    std::shared_ptr<DeviceAllocatorState> deviceState;
    std::shared_ptr<const GpuImage> source;
    StagingBuffer staging;
    vk::raii::CommandPool pool{nullptr};
    vk::raii::CommandBuffer buffer{nullptr};
    vk::raii::Fence fence{nullptr};
    std::uint64_t bytes = 0;
    std::vector<Rgba32f> pixels;
    bool cancelRequested = false;
    // True from the moment vkQueueSubmit succeeded until the fence is proven retired by THIS owner.
    // A wrong-thread poll never clears it; only the owner thread may.
    bool submitted = false;
    // Nonzero while this submission owns the global slot claim.
    std::uint64_t slotToken = 0;
    std::thread::id ownerThread{};
    std::chrono::steady_clock::time_point submittedAt{};

    // Frees this submission's local resources and releases its slot claim after the owner proved
    // retirement (or nothing was submitted). Owner-thread only. The live Impl owns every resource
    // until this point; releasing the Reserved claim just returns the slot to Free.
    void releaseRetired() noexcept {
        staging.release();
        fence = vk::raii::Fence{nullptr};
        buffer = vk::raii::CommandBuffer{nullptr};
        pool = vk::raii::CommandPool{nullptr};
        source.reset();
        if (slotToken != 0) {
            std::lock_guard lock(processSlotMutex());
            ProcessReadbackSlot& slot = processSlot();
            if (isSlotOwnerLocked(slot) && slot.token == slotToken) {
                slot.slotState = ProcessReadbackSlotState::Free;
                slot.ownerThread = std::thread::id{};
            }
            slotToken = 0;
        }
        deviceState.reset();
        submitted = false;
    }

    // Releases a Reserved claim taken before submission. Owner-thread only. Never frees another
    // owner's Quarantined resources.
    void releaseClaim() noexcept {
        if (slotToken == 0) {
            return;
        }
        std::lock_guard lock(processSlotMutex());
        ProcessReadbackSlot& slot = processSlot();
        if (isSlotOwnerLocked(slot) && slot.token == slotToken) {
            slot.slotState = ProcessReadbackSlotState::Free;
            slot.ownerThread = std::thread::id{};
        }
        slotToken = 0;
    }

    // Transfers this submission's (possibly in-flight) resources into the global slot as
    // Quarantined. Owner-thread only. This is the SOLE ownership transfer: the live Impl becomes
    // empty. If the Reserved claim is no longer owned (impossible while held, but defended), the
    // resources are retained in this Impl (`submitted` stays true) rather than destroyed.
    bool quarantine() noexcept {
        if (!submitted) {
            releaseRetired();
            return true;
        }
        processReadbackFuse().store(true);
        std::lock_guard lock(processSlotMutex());
        ProcessReadbackSlot& slot = processSlot();
        if (!isSlotOwnerLocked(slot) || slot.token != slotToken) {
            return false;
        }
        slot.state = std::move(deviceState);
        slot.source = std::move(source);
        slot.staging = std::move(staging);
        slot.pool = std::move(pool);
        slot.buffer = std::move(buffer);
        slot.fence = std::move(fence);
        slot.slotState = ProcessReadbackSlotState::Quarantined;
        deviceState.reset();
        source.reset();
        staging = {};
        pool = vk::raii::CommandPool{nullptr};
        buffer = vk::raii::CommandBuffer{nullptr};
        fence = vk::raii::Fence{nullptr};
        slotToken = 0;
        submitted = false;
        return true;
    }

    void setFailure(GpuProcessReadbackCode code, std::string_view message) noexcept {
        state = GpuProcessReadbackState::Failure;
        diagnostic.code = code;
        try {
            diagnostic.message.assign(message);
        } catch (...) {
            diagnostic.message.clear();
        }
    }

    void fail(GpuProcessReadbackCode code, std::string_view message) noexcept {
        setFailure(code, message);
        if (!submitted) {
            releaseClaim();
        }
    }
};

GpuProcessReadback::GpuProcessReadback() : impl_(std::make_unique<Impl>()) {}

GpuProcessReadback::~GpuProcessReadback() {
    if (impl_ == nullptr) {
        return;
    }
    // Only the owner thread may touch native resources. A destructor on a foreign thread while a
    // submission is live retains the whole Impl (leaked intentionally, bounded by the process fuse)
    // rather than destroying a possibly in-flight submission from the wrong thread.
    if (impl_->submitted && impl_->ownerThread != std::this_thread::get_id()) {
        [[maybe_unused]] const auto* const retained = impl_.release();
        return;
    }
    // A submitted-but-not-retired submission must be retired or quarantined for ANY state, not only
    // Pending: a non-Pending state with `submitted` still true means retirement was never proven.
    if (impl_->submitted) {
        cancel();
        const auto deadline =
            impl_->submittedAt + std::chrono::nanoseconds(kProcessReadbackDeadlineNanoseconds);
        while (impl_->submitted && impl_->state == GpuProcessReadbackState::Pending &&
               std::chrono::steady_clock::now() < deadline) {
            static_cast<void>(poll());
            if (impl_->submitted && impl_->state == GpuProcessReadbackState::Pending) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (impl_->submitted) {
            // Could not prove retirement: quarantine (retain) the live submission.
            if (!impl_->quarantine()) {
                [[maybe_unused]] const auto* const retained = impl_.release();
            } else {
                impl_->setFailure(GpuProcessReadbackCode::ReadbackFailed,
                                  "the readback did not retire before destruction; retained");
            }
        }
    }
}

GpuProcessReadbackState GpuProcessReadback::state() const noexcept {
    return impl_ == nullptr ? GpuProcessReadbackState::Idle : impl_->state;
}

const GpuProcessReadbackDiagnostic& GpuProcessReadback::diagnostic() const noexcept {
    static const GpuProcessReadbackDiagnostic empty{};
    return impl_ == nullptr ? empty : impl_->diagnostic;
}

bool GpuProcessReadback::isOwnerThread() const noexcept {
    return impl_ != nullptr && impl_->ownerThread == std::this_thread::get_id() &&
           impl_->deviceState != nullptr;
}

bool GpuProcessReadback::begin(std::shared_ptr<const GpuImage> image,
                               const std::uint64_t byteBudget) noexcept {
    try {
        if (impl_ == nullptr || impl_->state != GpuProcessReadbackState::Idle) {
            return false;
        }
        impl_->diagnostic = {};
        if (image == nullptr || gpuImageImpl(*image) == nullptr) {
            impl_->setFailure(GpuProcessReadbackCode::DeviceUnavailable, "no resident image");
            return false;
        }
        const GpuImageImpl* source = gpuImageImpl(*image);
        if (source->image == VK_NULL_HANDLE) {
            impl_->setFailure(GpuProcessReadbackCode::DeviceUnavailable, "no resident image");
            return false;
        }
        if (!source->onOwnerThread()) {
            impl_->setFailure(GpuProcessReadbackCode::WrongThread,
                              "readback must run on the device owner thread");
            return false;
        }
        if (source->deviceLost) {
            impl_->setFailure(GpuProcessReadbackCode::DeviceLost, "the device was lost");
            return false;
        }
        if (source->submissionUnretired) {
            impl_->setFailure(GpuProcessReadbackCode::DeviceUnavailable,
                              "a prior readback submission is not retired");
            return false;
        }
        std::uint64_t bytes = 0;
        if (!checkedReadbackBytes(source->width, source->height, bytes) || bytes > byteBudget) {
            impl_->setFailure(GpuProcessReadbackCode::OverBudget,
                              "the readback exceeds the requested byte budget");
            return false;
        }

        const std::thread::id self = std::this_thread::get_id();
        // Claim the global slot. A Reserved slot is owned by a live Impl and a Quarantined slot by
        // a possibly in-flight submission; neither may be reused by a foreign OR same owner thread
        // until proven retired. Only a same-owner quarantined slot whose fence is proven retired
        // may be reclaimed. A Reserved slot is never reclaimed (the owning Impl still holds
        // resources).
        {
            std::lock_guard lock(processSlotMutex());
            ProcessReadbackSlot& slot = processSlot();
            if (slot.slotState == ProcessReadbackSlotState::Reserved) {
                impl_->setFailure(GpuProcessReadbackCode::DeviceUnavailable,
                                  "the readback slot is reserved by a live submission");
                return false;
            }
            if (slot.slotState == ProcessReadbackSlotState::Quarantined) {
                if (slot.ownerThread != self) {
                    impl_->setFailure(GpuProcessReadbackCode::DeviceUnavailable,
                                      "a foreign owner thread holds the readback slot");
                    return false;
                }
                if (!retireQuarantinedSlotLocked(slot)) {
                    impl_->setFailure(GpuProcessReadbackCode::DeviceUnavailable,
                                      "a prior readback submission is not retired");
                    return false;
                }
            }
            slot.slotState = ProcessReadbackSlotState::Reserved;
            slot.ownerThread = self;
            ++slot.token;
            if (slot.token == 0) {
                ++slot.token;
            }
            impl_->slotToken = slot.token;
        }

        auto state = source->state;
        const VkDevice device = static_cast<VkDevice>(*state->device);
        const auto* dispatcher = state->device.getDispatcher();

        StagingBuffer staging;
        staging.state = state.get();
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo stagingAllocation{};
        stagingAllocation.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAllocation.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo stagingInfo{};
        if (vmaCreateBuffer(state->allocator, &bufferInfo, &stagingAllocation, &staging.buffer,
                            &staging.allocation, &stagingInfo) != VK_SUCCESS) {
            impl_->fail(GpuProcessReadbackCode::ReadbackFailed,
                        "the readback staging buffer could not be created");
            return false;
        }
        staging.armed = true;

        // Actual-size budget: this submission concurrently owns the VMA staging allocation
        // (stagingInfo.size, allocator-rounded) and, once the fence retires, the host pixel vector
        // of `bytes`. Both must fit `byteBudget` together; the logical `bytes <= byteBudget` check
        // before the claim is only a cheap preflight. Overflow-safe.
        const std::uint64_t actualStagingBytes = static_cast<std::uint64_t>(stagingInfo.size);
        if (actualStagingBytes > byteBudget || bytes > byteBudget - actualStagingBytes) {
            impl_->fail(GpuProcessReadbackCode::OverBudget,
                        "the actual staging buffer plus host vector exceeds the byte budget");
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
                impl_->fail(GpuProcessReadbackCode::ReadbackFailed,
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
                impl_->fail(GpuProcessReadbackCode::ReadbackFailed,
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
                impl_->fail(GpuProcessReadbackCode::ReadbackFailed,
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
            impl_->fail(GpuProcessReadbackCode::ReadbackFailed,
                        "the readback command buffer could not begin");
            return false;
        }

        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = source->image;
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                         1, &toTransfer);

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {source->width, source->height, 1};
        dispatcher->vkCmdCopyImageToBuffer(rawCommandBuffer, source->image,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1,
                                           &copy);

        VkImageMemoryBarrier toGeneral = toTransfer;
        toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                         nullptr, 1, &toGeneral);

        if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
            impl_->fail(GpuProcessReadbackCode::ReadbackFailed,
                        "the readback command buffer could not end");
            return false;
        }

        const VkFence rawFence = static_cast<VkFence>(*fence);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &rawCommandBuffer;
        const VkResult submitted = dispatcher->vkQueueSubmit(
            static_cast<VkQueue>(*state->computeQueue), 1, &submit, rawFence);
        if (submitted == VK_ERROR_DEVICE_LOST) {
            impl_->fail(GpuProcessReadbackCode::DeviceLost,
                        "the device was lost during the readback submit");
            return false;
        }
        if (submitted != VK_SUCCESS) {
            impl_->fail(GpuProcessReadbackCode::ReadbackFailed, "the readback submission failed");
            return false;
        }

        // The live Impl owns every native resource while Pending; the global slot stays RESERVED
        // until this submission retires (releaseRetired) or is quarantined (quarantine()). No
        // ownership metadata is copied into the slot here; the slot is not pollable/reclaimable
        // while reserved.
        impl_->deviceState = std::move(state);
        impl_->source = std::move(image);
        impl_->staging = std::move(staging);
        impl_->pool = std::move(pool);
        impl_->buffer = std::move(commandBuffer);
        impl_->fence = std::move(fence);
        impl_->bytes = bytes;
        impl_->cancelRequested = false;
        impl_->ownerThread = self;
        impl_->submitted = true;
        impl_->submittedAt = std::chrono::steady_clock::now();
        impl_->state = GpuProcessReadbackState::Pending;
        return true;
    } catch (const std::bad_alloc&) {
        impl_->fail(GpuProcessReadbackCode::ReadbackFailed,
                    "the readback submission state could not be allocated");
        return false;
    } catch (...) {
        impl_->fail(GpuProcessReadbackCode::ReadbackFailed, "the readback submission failed");
        return false;
    }
}

GpuProcessReadbackState GpuProcessReadback::poll() noexcept {
    if (impl_ == nullptr || impl_->state != GpuProcessReadbackState::Pending) {
        return impl_ == nullptr ? GpuProcessReadbackState::Idle : impl_->state;
    }
    Impl& impl = *impl_;
    // Wrong-thread poll must NOT alter the native job state or retirement ownership. It reports a
    // typed WrongThread failure for the caller but keeps Pending and `submitted` unchanged so the
    // real owner can still complete/retire.
    if (impl.ownerThread != std::this_thread::get_id()) {
        impl.diagnostic = {GpuProcessReadbackCode::WrongThread,
                           "readback poll must run on the device owner thread"};
        return GpuProcessReadbackState::Pending;
    }
    if (impl.deviceState == nullptr) {
        impl.fail(GpuProcessReadbackCode::DeviceUnavailable, "the readback lost its device");
        return impl.state;
    }
    const auto* dispatcher = impl.deviceState->device.getDispatcher();
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    const VkResult status =
        dispatcher->vkGetFenceStatus(static_cast<VkDevice>(*impl.deviceState->device), rawFence);
    if (status == VK_NOT_READY) {
        const bool deadlineExceeded =
            std::chrono::steady_clock::now() >=
            impl.submittedAt + std::chrono::nanoseconds(kProcessReadbackDeadlineNanoseconds);
        if (deadlineExceeded) {
            const bool cancelled = impl.cancelRequested;
            impl.quarantine();
            impl.setFailure(cancelled ? GpuProcessReadbackCode::Cancelled
                                      : GpuProcessReadbackCode::ReadbackFailed,
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
        impl.setFailure(GpuProcessReadbackCode::DeviceLost,
                        "the device was lost during the readback wait");
        return impl.state;
    }
    if (status != VK_SUCCESS) {
        impl.quarantine();
        impl.setFailure(GpuProcessReadbackCode::ReadbackFailed,
                        "the readback completion is unknown; the submission was retained");
        return impl.state;
    }

    const bool cancelled = impl.cancelRequested;
    if (!cancelled) {
        if (vmaInvalidateAllocation(impl.deviceState->allocator, impl.staging.allocation, 0,
                                    impl.bytes) != VK_SUCCESS) {
            impl.releaseRetired();
            impl.setFailure(GpuProcessReadbackCode::ReadbackFailed,
                            "the readback buffer could not be invalidated");
            return impl.state;
        }
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(impl.deviceState->allocator, impl.staging.allocation, &info);
        if (info.pMappedData == nullptr) {
            impl.releaseRetired();
            impl.setFailure(GpuProcessReadbackCode::ReadbackFailed,
                            "the readback buffer was not host-visible");
            return impl.state;
        }
        try {
            impl.pixels.resize(static_cast<std::size_t>(impl.bytes / sizeof(Rgba32f)),
                               Rgba32f::transparent());
            std::memcpy(impl.pixels.data(), info.pMappedData, static_cast<std::size_t>(impl.bytes));
        } catch (const std::bad_alloc&) {
            impl.releaseRetired();
            impl.setFailure(GpuProcessReadbackCode::ReadbackFailed,
                            "the readback host buffer could not be allocated");
            return impl.state;
        }
    }
    impl.releaseRetired();
    if (cancelled) {
        impl.setFailure(GpuProcessReadbackCode::Cancelled,
                        "the readback was cancelled; no frame was published");
    } else {
        impl.state = GpuProcessReadbackState::Ready;
        impl.diagnostic = {};
    }
    return impl.state;
}

std::vector<Rgba32f> GpuProcessReadback::take() noexcept {
    if (impl_ == nullptr || impl_->state != GpuProcessReadbackState::Ready) {
        return {};
    }
    std::vector<Rgba32f> pixels = std::move(impl_->pixels);
    impl_->pixels.clear();
    impl_->state = GpuProcessReadbackState::Idle;
    impl_->diagnostic = {};
    return pixels;
}

void GpuProcessReadback::cancel() noexcept {
    if (impl_ != nullptr && impl_->state == GpuProcessReadbackState::Pending) {
        impl_->cancelRequested = true;
    }
}

} // namespace bloom::render
