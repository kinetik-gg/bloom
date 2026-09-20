#ifndef BLOOM_RENDER_VULKAN_GPU_OUTPUT_COLOR_READBACK_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_OUTPUT_COLOR_READBACK_PRIVATE_HPP

// Private to src/render/vulkan. The bounded reservation and native resource set behind the
// production combined output-colour readback. This mirrors the accepted GpuProcessReadback slot
// contract: exactly one submission may be Reserved or Quarantined process-wide. A reservation is
// acquired BEFORE submit; a proven retirement returns it to Free; an unproven retirement moves the
// exact submission (device generation, sources, staging, command pool/buffer, fence) into the
// retained quarantine so only the genuine owner thread can later prove retirement or observe device
// loss. No per-object unbounded leak: a foreign-thread destruction retains the one occupied
// reservation rather than allocating a new one, and admission is refused while it is occupied.

#include "gpu_device_private.hpp"
#include "gpu_image_private.hpp"
#include "gpu_output_color_readback_fault.hpp"
#include "gpu_resident_display_private.hpp"

#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_resident_display.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace bloom::render::output_color_readback_detail {

using vulkan_detail::DeviceAllocatorState;

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

enum class ReservationState : std::uint8_t {
    // Reusable by any owner thread.
    Free,
    // A live Impl owns every native resource. Never reclaimed by anyone until that Impl retires or
    // quarantines.
    Reserved,
    // A possibly in-flight submission was transferred here; only the owner thread whose fence is
    // proven retired (or the device is lost) may free/reclaim it.
    Quarantined,
};

struct Reservation final {
    ReservationState state = ReservationState::Free;
    std::thread::id ownerThread{};
    std::uint64_t token = 0;

    std::shared_ptr<DeviceAllocatorState> deviceState;
    std::shared_ptr<const GpuImage> process;
    std::shared_ptr<const GpuImage> encodedProcess;
    std::shared_ptr<const GpuDisplayImage> encodedDisplay;
    StagingBuffer staging;
    vk::raii::CommandPool pool{nullptr};
    vk::raii::CommandBuffer buffer{nullptr};
    vk::raii::Fence fence{nullptr};
};

inline Reservation& reservation() {
    static auto* const value = new Reservation();
    return *value;
}

inline std::mutex& reservationMutex() {
    static auto* const value = new std::mutex();
    return *value;
}

// Latched when a submission could not be proven retired. Never cleared; the reservation itself
// serializes admission so a stale fuse only denies new work until the owner proves retirement.
inline std::atomic<bool>& outputColorReadbackFuse() {
    static std::atomic<bool> fuse{false};
    return fuse;
}

inline void freeQuarantinedLocked(Reservation& slot) noexcept {
    slot.staging.release();
    slot.fence = vk::raii::Fence{nullptr};
    slot.buffer = vk::raii::CommandBuffer{nullptr};
    slot.pool = vk::raii::CommandPool{nullptr};
    slot.encodedDisplay.reset();
    slot.encodedProcess.reset();
    slot.process.reset();
    slot.deviceState.reset();
    slot.state = ReservationState::Free;
    slot.ownerThread = std::thread::id{};
}

inline bool isReservationOwnerLocked(const Reservation& slot) noexcept {
    return slot.state != ReservationState::Free && slot.ownerThread == std::this_thread::get_id();
}

// Owner-only, non-blocking.
inline bool retireQuarantinedLocked(Reservation& slot) noexcept {
    if (slot.state != ReservationState::Quarantined || slot.deviceState == nullptr ||
        slot.fence == vk::raii::Fence{nullptr}) {
        return false;
    }
    // Test seam: ForceFenceTimeout keeps a quarantined submission un-retirable so admission refusal
    // and recovery can be proven deterministically.
    if (static_cast<ReadbackFault>(readbackFault().load()) == ReadbackFault::ForceFenceTimeout) {
        return false;
    }
    const VkFence rawFence = static_cast<VkFence>(*slot.fence);
    const VkResult status = slot.deviceState->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*slot.deviceState->device), rawFence);
    if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) {
        return false;
    }
    freeQuarantinedLocked(slot);
    return true;
}

// True while the quarantine holds a possibly in-flight submission. Test/observability seam.
[[nodiscard]] inline bool quarantineOccupied() noexcept {
    std::lock_guard lock(reservationMutex());
    return reservation().state == ReservationState::Quarantined;
}

// Owner-thread retirement of an occupied quarantine. Returns true when the fence is proven retired
// (or the device was lost) and the native resources are freed. Test/observability seam.
[[nodiscard]] inline bool retireQuarantineForOwner() noexcept {
    std::lock_guard lock(reservationMutex());
    Reservation& slot = reservation();
    if (slot.state != ReservationState::Quarantined) {
        return false;
    }
    if (slot.ownerThread != std::this_thread::get_id()) {
        return false;
    }
    return retireQuarantinedLocked(slot);
}

// Checked width * height * bytesPerPixel with explicit zero and overflow rejection.
[[nodiscard]] inline bool checkedImageBytes(const std::uint32_t width, const std::uint32_t height,
                                            const std::uint64_t bytesPerPixel,
                                            std::uint64_t& out) noexcept {
    if (width == 0 || height == 0 || bytesPerPixel == 0) {
        return false;
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > UINT64_MAX / bytesPerPixel) {
        return false;
    }
    out = pixels * bytesPerPixel;
    return true;
}

// Records one RGBA32F GENERAL-layout source barrier + copy into `offset`.
inline void recordRgba32fCopy(const VkCommandBuffer command, const GpuImageImpl& source,
                              const VkBuffer staging, const std::uint64_t offset) {
    const auto* dispatcher = source.state->device.getDispatcher();
    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = source.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dispatcher->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                     &toTransfer);
    VkBufferImageCopy copy{};
    copy.bufferOffset = offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {source.width, source.height, 1};
    dispatcher->vkCmdCopyImageToBuffer(command, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       staging, 1, &copy);
    VkImageMemoryBarrier toGeneral = toTransfer;
    toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dispatcher->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toGeneral);
}

// Records one RGBA8 SHADER_READ_ONLY_OPTIMAL display source barrier + copy into `offset`.
inline void recordRgba8Copy(const VkCommandBuffer command, const GpuDisplayImageImpl& source,
                            const VkBuffer staging, const std::uint64_t offset) {
    const auto* dispatcher = source.state->device.getDispatcher();
    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = source.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dispatcher->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                     &toTransfer);
    VkBufferImageCopy copy{};
    copy.bufferOffset = offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {source.width, source.height, 1};
    dispatcher->vkCmdCopyImageToBuffer(command, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       staging, 1, &copy);
    VkImageMemoryBarrier toRead = toTransfer;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dispatcher->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toRead);
}

} // namespace bloom::render::output_color_readback_detail

#endif // BLOOM_RENDER_VULKAN_GPU_OUTPUT_COLOR_READBACK_PRIVATE_HPP
