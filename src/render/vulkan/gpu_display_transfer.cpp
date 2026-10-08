#include <bloom/render/gpu_display_transfer.hpp>

#include "gpu_device_private.hpp"
#include "gpu_display_transfer_fault.hpp"
#include "gpu_resident_display_private.hpp"

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
struct GpuDisplayTransferRetention::State final {
    std::atomic<std::uint64_t> reservedBytes{0}, sourceBytes{0}, stagingBytes{0}, hostBytes{0};
    void releaseEmptyReservation() noexcept {
        if (sourceBytes.load() == 0 && stagingBytes.load() == 0 && hostBytes.load() == 0)
            reservedBytes.store(0);
    }
    void releaseNative() noexcept {
        sourceBytes.store(0);
        stagingBytes.store(0);
        releaseEmptyReservation();
    }
    void releaseHost() noexcept {
        hostBytes.store(0);
        releaseEmptyReservation();
    }
};
GpuDisplayTransferRetentionSnapshot GpuDisplayTransferRetention::snapshot() const noexcept {
    return {state_->reservedBytes.load(), state_->sourceBytes.load(), state_->stagingBytes.load(),
            state_->hostBytes.load()};
}
namespace {

using vulkan_detail::DeviceAllocatorState;

constexpr std::uint64_t kDisplayTransferDeadlineNanoseconds = 5ULL * 1000ULL * 1000ULL * 1000ULL;

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

// The bounded global process display transfer slot. It has exactly three states:
//   * Free        -- reusable by any owner thread.
//   * Reserved    -- owner A claimed it and its live Impl still owns every native resource. A
//                    Reservation is NOT pollable and NOT reclaimable by anyone (same or foreign):
//                    any new claim must refuse Busy. The live Impl is the sole owner until it
//                    retires or quarantines.
//   * Quarantined -- owner A transferred a possibly-in-flight submission (with its owned fence,
//                    staging, pool, buffer, source, device generation) into the slot. Only the same
//                    owner thread whose fence is proven retired may reclaim/refill it; a foreign
//                    thread may never make a driver call or destroy it.
// Thread id AND the nonzero owner-lifetime token are the authoritative identity. The slot NEVER
// duplicates ownership metadata to pretend a live Reservation is Quarantined.
enum class DisplayTransferSlotState : std::uint8_t {
    Free,
    Reserved,
    Quarantined,
};

struct DisplayTransferSlot final {
    DisplayTransferSlotState slotState = DisplayTransferSlotState::Free;
    std::thread::id ownerThread{};
    std::uint64_t ownerLifetimeToken = 0;
    std::uint64_t token = 0;

    std::shared_ptr<DeviceAllocatorState> state;
    std::shared_ptr<GpuDisplayTransferRetention::State> retention;
    std::shared_ptr<const GpuDisplayImage> source;
    StagingBuffer staging;
    vk::raii::CommandPool pool{nullptr};
    vk::raii::CommandBuffer buffer{nullptr};
    vk::raii::Fence fence{nullptr};
};

DisplayTransferSlot& displayTransferSlot() {
    static auto* const slot = new DisplayTransferSlot();
    return *slot;
}

std::mutex& displayTransferSlotMutex() {
    static auto* const mutex = new std::mutex();
    return *mutex;
}

std::atomic<bool>& displayTransferFuse() {
    static std::atomic<bool> fuse{false};
    return fuse;
}

// Frees a Quarantined slot's native resources. The caller must hold the mutex and must have
// established that this thread is the slot owner (or the device is lost); a Reserved slot is owned
// by a live Impl and is never freed here.
void freeQuarantinedSlotLocked(DisplayTransferSlot& slot) noexcept {
    slot.staging.release();
    slot.fence = vk::raii::Fence{nullptr};
    slot.buffer = vk::raii::CommandBuffer{nullptr};
    slot.pool = vk::raii::CommandPool{nullptr};
    if (slot.source)
        gpuDisplayImageImpl(*slot.source)
            ->submissionUnretired.store(false, std::memory_order_release);
    slot.source.reset();
    slot.state.reset();
    if (slot.retention)
        slot.retention->releaseNative();
    slot.retention.reset();
    slot.slotState = DisplayTransferSlotState::Free;
    slot.ownerThread = std::thread::id{};
    slot.ownerLifetimeToken = 0;
}

[[nodiscard]] bool isSlotOwnerLocked(const DisplayTransferSlot& slot) noexcept {
    return slot.slotState != DisplayTransferSlotState::Free &&
           slot.ownerThread == std::this_thread::get_id() && slot.ownerLifetimeToken != 0 &&
           slot.ownerLifetimeToken == vulkan_detail::deviceOwnerLifetimeToken() &&
           (!slot.state || slot.state->ownerLifetimeToken == slot.ownerLifetimeToken);
}

// Best-effort non-blocking retirement of a quarantined slot. Only the slot owner may call this (the
// caller holds the mutex and has checked the owner thread). Returns true when the fence is proven
// signalled or the device is lost, in which case the resources are freed.
[[nodiscard]] bool retireQuarantinedSlotLocked(DisplayTransferSlot& slot) noexcept {
    if (!isSlotOwnerLocked(slot) || slot.slotState != DisplayTransferSlotState::Quarantined ||
        slot.state == nullptr || static_cast<VkFence>(*slot.fence) == VK_NULL_HANDLE) {
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
    constexpr std::uint64_t kComponentBytes = sizeof(Rgba8);
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

struct GpuDisplayTransfer::Impl final {
    GpuDisplayTransferState state = GpuDisplayTransferState::Idle;
    GpuDisplayTransferDiagnostic diagnostic;

    std::shared_ptr<DeviceAllocatorState> deviceState;
    std::shared_ptr<GpuDisplayTransferRetention::State> retentionState;
    std::shared_ptr<const GpuDisplayTransferRetention> retentionView;
    std::shared_ptr<const GpuDisplayImage> source;
    StagingBuffer staging;
    vk::raii::CommandPool pool{nullptr};
    vk::raii::CommandBuffer buffer{nullptr};
    vk::raii::Fence fence{nullptr};
    std::uint64_t bytes = 0;
    std::vector<Rgba8> pixels;
    GpuDisplayTransferAccounting accounting;
    std::optional<ImageWindow> dataWindow, displayWindow;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    std::uint32_t generation = 0;
    std::weak_ptr<const void> deviceBinding;
    std::uint64_t quarantinedToken = 0;
    bool cancelRequested = false;
    // True from the moment vkQueueSubmit succeeded until the fence is proven retired by THIS owner.
    // A wrong-thread poll never clears it; only the owner thread may.
    bool submitted = false;
    // Nonzero while this submission owns the global slot claim.
    std::uint64_t slotToken = 0;
    std::thread::id ownerThread{};
    std::uint64_t ownerLifetimeToken = 0;
    std::chrono::steady_clock::time_point submittedAt{};

    ~Impl() {
        std::vector<Rgba8>{}.swap(pixels);
        if (retentionState)
            retentionState->releaseHost();
    }
    void discardHost() noexcept {
        std::vector<Rgba8>{}.swap(pixels);
        if (retentionState)
            retentionState->releaseHost();
    }
    [[nodiscard]] bool onOwnerThread() const noexcept {
        return ownerThread == std::this_thread::get_id() && ownerLifetimeToken != 0 &&
               ownerLifetimeToken == vulkan_detail::deviceOwnerLifetimeToken() &&
               (!deviceState || deviceState->ownerLifetimeToken == ownerLifetimeToken);
    }

    // Frees this submission's local resources and releases its slot claim after the owner proved
    // retirement (or nothing was submitted). Owner-thread only. The live Impl owns every resource
    // until this point; releasing the Reserved claim just returns the slot to Free.
    void releaseRetired() noexcept {
        if (ownerLifetimeToken != 0 && !onOwnerThread())
            return;
        staging.release();
        fence = vk::raii::Fence{nullptr};
        buffer = vk::raii::CommandBuffer{nullptr};
        pool = vk::raii::CommandPool{nullptr};
        if (source && submitted)
            gpuDisplayImageImpl(*source)->submissionUnretired.store(false,
                                                                    std::memory_order_release);
        source.reset();
        if (slotToken != 0) {
            std::lock_guard lock(displayTransferSlotMutex());
            DisplayTransferSlot& slot = displayTransferSlot();
            if (isSlotOwnerLocked(slot) && slot.token == slotToken) {
                slot.slotState = DisplayTransferSlotState::Free;
                slot.ownerThread = std::thread::id{};
                slot.ownerLifetimeToken = 0;
                slot.retention.reset();
            }
            slotToken = 0;
        }
        deviceState.reset();
        submitted = false;
        if (retentionState)
            retentionState->releaseNative();
    }

    // Releases a Reserved claim taken before submission. Owner-thread only. Never frees another
    // owner's Quarantined resources.
    void releaseClaim() noexcept {
        if (slotToken == 0) {
            return;
        }
        std::lock_guard lock(displayTransferSlotMutex());
        DisplayTransferSlot& slot = displayTransferSlot();
        if (isSlotOwnerLocked(slot) && slot.token == slotToken) {
            slot.slotState = DisplayTransferSlotState::Free;
            slot.ownerThread = std::thread::id{};
            slot.ownerLifetimeToken = 0;
            slot.retention.reset();
        }
        slotToken = 0;
        if (retentionState)
            retentionState->releaseNative();
    }

    // Transfers this submission's (possibly in-flight) resources into the global slot as
    // Quarantined. Owner-thread only. This is the SOLE ownership transfer: the live Impl becomes
    // empty. If the Reserved claim is no longer owned (impossible while held, but defended), the
    // resources are retained in this Impl (`submitted` stays true) rather than destroyed.
    bool quarantine() noexcept {
        if (!onOwnerThread())
            return false;
        if (!submitted) {
            releaseRetired();
            return true;
        }
        displayTransferFuse().store(true);
        std::lock_guard lock(displayTransferSlotMutex());
        DisplayTransferSlot& slot = displayTransferSlot();
        if (!isSlotOwnerLocked(slot) || slot.token != slotToken) {
            return false;
        }
        slot.state = std::move(deviceState);
        slot.retention = retentionState;
        slot.source = std::move(source);
        slot.staging = std::move(staging);
        slot.pool = std::move(pool);
        slot.buffer = std::move(buffer);
        slot.fence = std::move(fence);
        slot.slotState = DisplayTransferSlotState::Quarantined;
        deviceState.reset();
        source.reset();
        staging = {};
        pool = vk::raii::CommandPool{nullptr};
        buffer = vk::raii::CommandBuffer{nullptr};
        fence = vk::raii::Fence{nullptr};
        quarantinedToken = slotToken;
        slotToken = 0;
        submitted = false;
        return true;
    }

    void setFailure(GpuDisplayTransferCode code, std::string_view message) noexcept {
        state = GpuDisplayTransferState::Failure;
        diagnostic.code = code;
        try {
            diagnostic.message.assign(message);
        } catch (...) {
            diagnostic.message.clear();
        }
    }

    void fail(GpuDisplayTransferCode code, std::string_view message) noexcept {
        setFailure(code, message);
        if (!submitted) {
            releaseClaim();
            discardHost();
        }
    }
};

GpuDisplayTransfer::GpuDisplayTransfer() : impl_(std::make_unique<Impl>()) {}

GpuDisplayTransfer::~GpuDisplayTransfer() {
    if (!impl_ || !impl_->submitted)
        return;
    if (!impl_->onOwnerThread()) {
        [[maybe_unused]] const auto* retained = impl_.release();
        return;
    }
    cancel();
    static_cast<void>(poll()); // one nonblocking retirement observation, never a wait loop
    if (impl_->submitted && !impl_->quarantine()) {
        [[maybe_unused]] const auto* retained = impl_.release();
    }
}

GpuDisplayTransferState GpuDisplayTransfer::state() const noexcept {
    return impl_ == nullptr ? GpuDisplayTransferState::Idle : impl_->state;
}

const GpuDisplayTransferDiagnostic& GpuDisplayTransfer::diagnostic() const noexcept {
    static const GpuDisplayTransferDiagnostic empty{};
    return impl_ == nullptr ? empty : impl_->diagnostic;
}

bool GpuDisplayTransfer::isOwnerThread() const noexcept {
    return impl_ != nullptr && impl_->onOwnerThread();
}

bool GpuDisplayTransfer::begin(GpuDevice& expectedDevice,
                               std::shared_ptr<const GpuDisplayImage> image,
                               const std::uint64_t byteBudget) noexcept {
    try {
        if (impl_ == nullptr || impl_->state != GpuDisplayTransferState::Idle) {
            return false;
        }
        impl_->diagnostic = {};
        if (image == nullptr || gpuDisplayImageImpl(*image) == nullptr) {
            impl_->setFailure(GpuDisplayTransferCode::DeviceUnavailable, "no resident image");
            return false;
        }
        const GpuDisplayImageImpl* source = gpuDisplayImageImpl(*image);
        if (source->image == VK_NULL_HANDLE) {
            impl_->setFailure(GpuDisplayTransferCode::DeviceUnavailable, "no resident image");
            return false;
        }
        if (!source->onOwnerThread()) {
            impl_->setFailure(GpuDisplayTransferCode::WrongThread,
                              "display transfer must run on the device owner thread");
            return false;
        }
        if (!image->isValid() || !image->isBoundTo(expectedDevice) || source->state == nullptr ||
            source->generation == 0 || source->generation != source->state->generation ||
            !source->dataWindow || !source->displayWindow) {
            impl_->setFailure(GpuDisplayTransferCode::DeviceUnavailable,
                              "the final display source is not bound to this device generation");
            return false;
        }
        if (source->deviceLost) {
            impl_->setFailure(GpuDisplayTransferCode::DeviceLost, "the device was lost");
            return false;
        }
        // A prior Impl may already have been destroyed after quarantining this very source.
        // Offer its exact owner one real nonblocking fence observation before the image marker
        // refuses admission. Never poll a Reserved slot owned by a still-live Impl.
        {
            std::lock_guard lock(displayTransferSlotMutex());
            auto& slot = displayTransferSlot();
            if (slot.slotState == DisplayTransferSlotState::Quarantined && isSlotOwnerLocked(slot))
                static_cast<void>(retireQuarantinedSlotLocked(slot));
        }
        if (source->submissionUnretired) {
            impl_->setFailure(GpuDisplayTransferCode::DeviceUnavailable,
                              "a prior display transfer submission is not retired");
            return false;
        }
        std::uint64_t bytes = 0;
        if (!checkedReadbackBytes(source->width, source->height, bytes) || bytes > byteBudget) {
            impl_->setFailure(GpuDisplayTransferCode::OverBudget,
                              "the display transfer exceeds the requested byte budget");
            return false;
        }

        if (image->allocationBytes() > UINT64_MAX - byteBudget) {
            impl_->setFailure(GpuDisplayTransferCode::OverBudget,
                              "source and transfer reservation overflow");
            return false;
        }
        impl_->retentionState = std::make_shared<GpuDisplayTransferRetention::State>();
        impl_->retentionView = std::shared_ptr<const GpuDisplayTransferRetention>(
            new GpuDisplayTransferRetention(impl_->retentionState));
        const std::thread::id self = std::this_thread::get_id();
        // Claim the global slot. A Reserved slot is owned by a live Impl and a Quarantined slot by
        // a possibly in-flight submission; neither may be reused by a foreign OR same owner thread
        // until proven retired. Only a same-owner quarantined slot whose fence is proven retired
        // may be reclaimed. A Reserved slot is never reclaimed (the owning Impl still holds
        // resources).
        {
            std::lock_guard lock(displayTransferSlotMutex());
            DisplayTransferSlot& slot = displayTransferSlot();
            if (slot.slotState == DisplayTransferSlotState::Reserved) {
                impl_->setFailure(GpuDisplayTransferCode::Busy,
                                  "the display transfer slot is reserved by a live submission");
                return false;
            }
            if (slot.slotState == DisplayTransferSlotState::Quarantined) {
                if (!isSlotOwnerLocked(slot)) {
                    impl_->setFailure(GpuDisplayTransferCode::Busy,
                                      "a foreign owner thread holds the display transfer slot");
                    return false;
                }
                if (!retireQuarantinedSlotLocked(slot)) {
                    impl_->setFailure(GpuDisplayTransferCode::Busy,
                                      "a prior display transfer submission is not retired");
                    return false;
                }
            }
            slot.slotState = DisplayTransferSlotState::Reserved;
            slot.ownerThread = self;
            slot.ownerLifetimeToken = source->state->ownerLifetimeToken;
            impl_->ownerThread = self;
            impl_->ownerLifetimeToken = slot.ownerLifetimeToken;
            ++slot.token;
            if (slot.token == 0) {
                ++slot.token;
            }
            impl_->slotToken = slot.token;
            slot.retention = impl_->retentionState;
            impl_->retentionState->reservedBytes.store(byteBudget + image->allocationBytes());
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
            impl_->fail(GpuDisplayTransferCode::TransferFailed,
                        "the display transfer staging buffer could not be created");
            return false;
        }
        staging.armed = true;

        // Actual-size budget: this submission concurrently owns the VMA staging allocation
        // (stagingInfo.size, allocator-rounded) and, once the fence retires, the host pixel vector
        // of `bytes`. Both must fit `byteBudget` together; the logical `bytes <= byteBudget` check
        // before the claim is only a cheap preflight. Overflow-safe.
        const std::uint64_t actualStagingBytes = static_cast<std::uint64_t>(stagingInfo.size);
        if (actualStagingBytes > byteBudget || bytes > byteBudget - actualStagingBytes) {
            impl_->fail(GpuDisplayTransferCode::OverBudget,
                        "the actual staging buffer plus host vector exceeds the byte budget");
            return false;
        }

        impl_->pixels.resize(static_cast<std::size_t>(bytes / sizeof(Rgba8)));
        const std::uint64_t hostBytes = impl_->pixels.capacity() * sizeof(Rgba8);
        impl_->accounting = {0,
                             bytes,
                             actualStagingBytes,
                             hostBytes,
                             actualStagingBytes + hostBytes,
                             image->allocationBytes()};
        if (hostBytes > byteBudget - actualStagingBytes) {
            impl_->fail(GpuDisplayTransferCode::OverBudget,
                        "actual staging plus host vector capacity exceeds the byte budget");
            return false;
        }
        impl_->dataWindow = source->dataWindow;
        impl_->displayWindow = source->displayWindow;
        impl_->pixelAspect = source->pixelAspect;
        impl_->generation = source->generation;
        impl_->deviceBinding = source->state;

        vk::raii::CommandPool pool{nullptr};
        {
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolInfo.queueFamilyIndex = state->computeQueueFamily;
            VkCommandPool raw = VK_NULL_HANDLE;
            if (dispatcher->vkCreateCommandPool(device, &poolInfo, nullptr, &raw) != VK_SUCCESS) {
                impl_->fail(GpuDisplayTransferCode::TransferFailed,
                            "the display transfer command pool could not be created");
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
                impl_->fail(GpuDisplayTransferCode::TransferFailed,
                            "the display transfer command buffer could not be allocated");
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
                impl_->fail(GpuDisplayTransferCode::TransferFailed,
                            "the display transfer fence could not be created");
                return false;
            }
            fence = vk::raii::Fence(state->device, raw);
        }

        const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*commandBuffer);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (dispatcher->vkBeginCommandBuffer(rawCommandBuffer, &beginInfo) != VK_SUCCESS) {
            impl_->fail(GpuDisplayTransferCode::TransferFailed,
                        "the display transfer command buffer could not begin");
            return false;
        }

        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = source->image;
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
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
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                         nullptr, 1, &toGeneral);

        // Make every copied destination byte visible to the host before fence retirement
        // and the subsequent allocation-relative invalidate.
        VkBufferMemoryBarrier toHost{};
        toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.buffer = staging.buffer;
        toHost.size = bytes;
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0,
                                         nullptr);

        if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
            impl_->fail(GpuDisplayTransferCode::TransferFailed,
                        "the display transfer command buffer could not end");
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
            impl_->fail(GpuDisplayTransferCode::DeviceLost,
                        "the device was lost during the display transfer submit");
            return false;
        }
        if (submitted != VK_SUCCESS) {
            impl_->fail(GpuDisplayTransferCode::TransferFailed,
                        "the display transfer submission failed");
            return false;
        }

        // The live Impl owns every native resource while Pending; the global slot stays RESERVED
        // until this submission retires (releaseRetired) or is quarantined (quarantine()). No
        // ownership metadata is copied into the slot here; the slot is not pollable/reclaimable
        // while reserved.
        source->submissionUnretired.store(true, std::memory_order_release);
        impl_->retentionState->reservedBytes.store(byteBudget + image->allocationBytes());
        impl_->retentionState->sourceBytes.store(image->allocationBytes());
        impl_->retentionState->stagingBytes.store(actualStagingBytes);
        impl_->retentionState->hostBytes.store(hostBytes);
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
        impl_->state = GpuDisplayTransferState::Pending;
        impl_->accounting.submissions = 1;
        return true;
    } catch (const std::bad_alloc&) {
        impl_->fail(GpuDisplayTransferCode::TransferFailed,
                    "the display transfer submission state could not be allocated");
        return false;
    } catch (...) {
        impl_->fail(GpuDisplayTransferCode::TransferFailed,
                    "the display transfer submission failed");
        return false;
    }
}

GpuDisplayTransferState GpuDisplayTransfer::poll() noexcept {
    if (impl_ && impl_->quarantinedToken != 0 && impl_->onOwnerThread()) {
        std::lock_guard lock(displayTransferSlotMutex());
        auto& slot = displayTransferSlot();
        if (slot.slotState == DisplayTransferSlotState::Quarantined &&
            slot.token == impl_->quarantinedToken && retireQuarantinedSlotLocked(slot))
            impl_->quarantinedToken = 0;
    }
    if (impl_ == nullptr || impl_->state != GpuDisplayTransferState::Pending) {
        return impl_ == nullptr ? GpuDisplayTransferState::Idle : impl_->state;
    }
    Impl& impl = *impl_;
    // Wrong-thread poll must NOT alter the native job state or retirement ownership. It reports a
    // typed WrongThread failure for the caller but keeps Pending and `submitted` unchanged so the
    // real owner can still complete/retire.
    if (!impl.onOwnerThread()) {
        impl.diagnostic = {GpuDisplayTransferCode::WrongThread,
                           "display transfer poll must run on the device owner thread"};
        return GpuDisplayTransferState::Pending;
    }
    if (impl.deviceState == nullptr) {
        impl.fail(GpuDisplayTransferCode::DeviceUnavailable,
                  "the display transfer lost its device");
        return impl.state;
    }
    const auto* dispatcher = impl.deviceState->device.getDispatcher();
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    const bool forceTimeout =
        display_transfer_detail::transferFault().load() ==
        static_cast<std::uint8_t>(display_transfer_detail::TransferFault::ForceFenceTimeout);
    const VkResult actualStatus =
        dispatcher->vkGetFenceStatus(static_cast<VkDevice>(*impl.deviceState->device), rawFence);
    const VkResult status =
        forceTimeout && actualStatus != VK_ERROR_DEVICE_LOST ? VK_NOT_READY : actualStatus;
    if (status == VK_NOT_READY) {
        const bool deadlineExceeded =
            forceTimeout ||
            std::chrono::steady_clock::now() >=
                impl.submittedAt + std::chrono::nanoseconds(kDisplayTransferDeadlineNanoseconds);
        if (deadlineExceeded) {
            const bool cancelled = impl.cancelRequested;
            impl.quarantine();
            impl.setFailure(
                cancelled ? GpuDisplayTransferCode::Cancelled
                          : GpuDisplayTransferCode::TransferFailed,
                cancelled
                    ? "the display transfer was cancelled; the submission is retained until "
                      "retired"
                    : "the display transfer did not retire within the deadline; the submission "
                      "is retained");
        }
        return impl.state;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        impl.releaseRetired();
        impl.setFailure(GpuDisplayTransferCode::DeviceLost,
                        "the device was lost during the display transfer wait");
        return impl.state;
    }
    if (status != VK_SUCCESS) {
        impl.quarantine();
        impl.setFailure(GpuDisplayTransferCode::TransferFailed,
                        "the display transfer completion is unknown; the submission was retained");
        return impl.state;
    }

    const bool cancelled = impl.cancelRequested;
    if (!cancelled) {
        if (vmaInvalidateAllocation(impl.deviceState->allocator, impl.staging.allocation, 0,
                                    impl.bytes) != VK_SUCCESS) {
            impl.releaseRetired();
            impl.setFailure(GpuDisplayTransferCode::TransferFailed,
                            "the display transfer buffer could not be invalidated");
            return impl.state;
        }
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(impl.deviceState->allocator, impl.staging.allocation, &info);
        if (info.pMappedData == nullptr) {
            impl.releaseRetired();
            impl.setFailure(GpuDisplayTransferCode::TransferFailed,
                            "the display transfer buffer was not host-visible");
            return impl.state;
        }
        try {
            std::memcpy(impl.pixels.data(), info.pMappedData, static_cast<std::size_t>(impl.bytes));
        } catch (const std::bad_alloc&) {
            impl.releaseRetired();
            impl.setFailure(GpuDisplayTransferCode::TransferFailed,
                            "the display transfer host buffer could not be allocated");
            return impl.state;
        }
    }
    impl.releaseRetired();
    if (cancelled) {
        impl.discardHost();
        impl.setFailure(GpuDisplayTransferCode::Cancelled,
                        "the display transfer was cancelled; no frame was published");
    } else {
        impl.state = GpuDisplayTransferState::Ready;
        impl.diagnostic = {};
    }
    return impl.state;
}

std::optional<GpuDisplayTransferResult> GpuDisplayTransfer::take() noexcept {
    if (!impl_ || impl_->state != GpuDisplayTransferState::Ready || !impl_->onOwnerThread() ||
        !impl_->dataWindow || !impl_->displayWindow)
        return std::nullopt;
    GpuDisplayTransferResult result(*impl_->dataWindow, *impl_->displayWindow, impl_->pixelAspect,
                                    impl_->generation, std::move(impl_->pixels), impl_->accounting,
                                    impl_->deviceBinding);
    impl_->retentionState->releaseHost();
    impl_->state = GpuDisplayTransferState::Idle;
    impl_->diagnostic = {};
    return result;
}

void GpuDisplayTransfer::cancel() noexcept {
    if (!impl_ || !impl_->onOwnerThread())
        return;
    if (impl_->state == GpuDisplayTransferState::Pending)
        impl_->cancelRequested = true;
    else if (impl_->state == GpuDisplayTransferState::Ready) {
        impl_->discardHost();
        impl_->setFailure(GpuDisplayTransferCode::Cancelled,
                          "the completed transfer was cancelled; no frame was published");
    }
}

bool GpuDisplayTransfer::reset() noexcept {
    if (!impl_ || hasUnretiredSubmission() ||
        (impl_->ownerLifetimeToken != 0 && !impl_->onOwnerThread()))
        return false;
    impl_->releaseRetired();
    impl_->discardHost();
    impl_->state = GpuDisplayTransferState::Idle;
    impl_->diagnostic = {};
    impl_->accounting = {};
    impl_->dataWindow.reset();
    impl_->displayWindow.reset();
    impl_->deviceBinding.reset();
    impl_->quarantinedToken = 0;
    impl_->ownerThread = {};
    impl_->ownerLifetimeToken = 0;
    return true;
}

std::shared_ptr<const GpuDisplayTransferRetention> GpuDisplayTransfer::retention() const noexcept {
    return impl_ ? impl_->retentionView : nullptr;
}

const GpuDisplayTransferAccounting& GpuDisplayTransfer::accounting() const noexcept {
    static const GpuDisplayTransferAccounting empty;
    return impl_ ? impl_->accounting : empty;
}

bool GpuDisplayTransfer::hasUnretiredSubmission() const noexcept {
    if (!impl_)
        return false;
    if (impl_->submitted)
        return true;
    if (impl_->quarantinedToken == 0)
        return false;
    std::lock_guard lock(displayTransferSlotMutex());
    const auto& slot = displayTransferSlot();
    return slot.slotState == DisplayTransferSlotState::Quarantined &&
           slot.token == impl_->quarantinedToken;
}

bool GpuDisplayTransferResult::isBoundTo(GpuDevice& device) const noexcept {
    const auto bound = deviceBinding_.lock();
    const auto state = GpuRendererAccess::state(device);
    return bound && state && bound.get() == state.get() && generation_ == state->generation;
}

namespace display_transfer_detail {
std::atomic<std::uint8_t>& transferFault() noexcept {
    static std::atomic<std::uint8_t> fault{static_cast<std::uint8_t>(TransferFault::None)};
    return fault;
}
} // namespace display_transfer_detail
} // namespace bloom::render
