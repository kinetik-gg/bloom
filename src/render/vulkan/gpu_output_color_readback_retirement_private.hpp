#pragma once

namespace bloom::render {
namespace vulkan_detail {
struct DeviceAllocatorState;
}
namespace output_color_readback_detail {
void drainReadbacks(vulkan_detail::DeviceAllocatorState& state) noexcept;
void closeReadbacks(vulkan_detail::DeviceAllocatorState& state) noexcept;
[[nodiscard]] bool readbackPendingFor(const vulkan_detail::DeviceAllocatorState& state) noexcept;
} // namespace output_color_readback_detail
} // namespace bloom::render
