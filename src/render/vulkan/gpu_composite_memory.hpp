#ifndef BLOOM_RENDER_VULKAN_GPU_COMPOSITE_MEMORY_HPP
#define BLOOM_RENDER_VULKAN_GPU_COMPOSITE_MEMORY_HPP

#include "gpu_composite_fault.hpp"
#include "gpu_device_private.hpp"

#include <atomic>

namespace bloom::render::composite_detail {

// Private instrumented seam: an unset hook always calls VMA. Tests can observe the actual
// allocation-relative range or return a failure before VMA; no memory type is forced.
using MappedMemoryHook = VkResult (*)(bool invalidate, VmaAllocator, VmaAllocation,
                                      VkDeviceSize offset, VkDeviceSize bytes) noexcept;
inline std::atomic<MappedMemoryHook> mappedMemoryHook{nullptr};

inline VkResult mappedMemoryVisibility(const bool invalidate, VmaAllocator allocator,
                                       VmaAllocation allocation,
                                       const VkDeviceSize bytes) noexcept {
    if (const auto hook = mappedMemoryHook.load(std::memory_order_relaxed)) {
        return hook(invalidate, allocator, allocation, 0, bytes);
    }
    return invalidate ? vmaInvalidateAllocation(allocator, allocation, 0, bytes)
                      : vmaFlushAllocation(allocator, allocation, 0, bytes);
}

// Fence completion alone does not make shader writes visible to host accesses. Record the
// availability/visibility dependency before submission, then invalidate only after retirement.
inline void statusToHost(vulkan_detail::DeviceAllocatorState& state, VkCommandBuffer command,
                         VkBuffer status, VkDeviceSize bytes) noexcept {
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = status;
    barrier.size = bytes;
    state.device.getDispatcher()->vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
        &barrier, 0, nullptr);
}

} // namespace bloom::render::composite_detail

#endif
