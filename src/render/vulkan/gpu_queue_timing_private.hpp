#pragma once

#include "gpu_device_private.hpp"

#include <bloom/render/gpu_queue_timing.hpp>

#include <array>
#include <chrono>

namespace bloom::render::queue_timing_detail {
enum class PollFault : std::uint8_t { None, Pending, Unknown };
struct Receipt final {
    GpuQueueTimingSample result{GpuQueueTimingStatus::Unsupported};
};
struct State final {
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    VkQueryPool queries = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, 2> commands{};
    std::array<VkFence, 2> fences{};
    GpuQueueTimingSample result{GpuQueueTimingStatus::Idle};
    std::weak_ptr<Receipt> receipt;
    std::chrono::steady_clock::time_point started{};
    std::uint64_t serial = 0;
    std::uint64_t active = 0;
    bool submittedStart = false;
    bool submittedEnd = false;
    bool cancelled = false;
    bool lost = false;
    bool ownsSlot = false;
    PollFault pollFaultForTests = PollFault::None;

    ~State();
    [[nodiscard]] bool owner() const noexcept;
    [[nodiscard]] bool initialize();
    [[nodiscard]] bool submit(std::size_t endpoint) noexcept;
    void close(bool discard) noexcept;
    void poll() noexcept;
    [[nodiscard]] bool pending() const noexcept { return submittedStart || submittedEnd; }
};
void drainRetired(vulkan_detail::DeviceAllocatorState& control) noexcept;
} // namespace bloom::render::queue_timing_detail
