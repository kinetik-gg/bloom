#pragma once

namespace bloom::render {
namespace vulkan_detail {
struct DeviceAllocatorState;
}
namespace display_transfer_detail {
void drainTransfers(vulkan_detail::DeviceAllocatorState& state) noexcept;
void closeTransfers(vulkan_detail::DeviceAllocatorState& state) noexcept;
[[nodiscard]] bool transferPendingFor(const vulkan_detail::DeviceAllocatorState& state) noexcept;
} // namespace display_transfer_detail
} // namespace bloom::render
