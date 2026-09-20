#ifndef BLOOM_RENDER_VULKAN_GPU_RESIDENT_DISPLAY_READBACK_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_RESIDENT_DISPLAY_READBACK_PRIVATE_HPP

// Private shared readback plumbing for the resident RGBA8 display image, used by both the debug
// full-frame readback (gpu_resident_display_resources.cpp) and the test-only sparse readback
// (gpu_resident_display_readback.cpp).
//
// Both paths stage into a host-visible VMA buffer and share ONE bounded process quarantine and
// fuse, so an unproven submission is retained (never freed under a live fence) and a later readback
// fails closed rather than touching a live native object. Keeping the helper here is what lets the
// sparse path exist without a second quarantine or a duplicated staging lifetime.

#include "gpu_resident_display_private.hpp"

#include <atomic>
#include <cstdint>
#include <utility>

namespace bloom::render::readback_detail {

// One host-visible staging buffer, released by its owning scope unless it has been handed to the
// quarantine.
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
    vulkan_detail::DeviceAllocatorState* state = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    bool armed = false;
};

// Bounded quarantine for a display readback whose completion is unproved. Retains the whole set and
// the device generation; allocated once and never destroyed. No allocation on the failure path.
struct DisplayReadbackQuarantine final {
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> state;
    StagingBuffer staging;
    vk::raii::CommandPool pool{nullptr};
    vk::raii::CommandBuffer buffer{nullptr};
    vk::raii::Fence fence{nullptr};
    bool occupied = false;
};

[[nodiscard]] DisplayReadbackQuarantine& displayQuarantine();
[[nodiscard]] std::atomic<bool>& displayFuse();
[[nodiscard]] bool tryRetireDisplayQuarantine(DisplayReadbackQuarantine& slot) noexcept;

} // namespace bloom::render::readback_detail

#endif
