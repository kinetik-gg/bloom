#pragma once

namespace bloom::render {
namespace vulkan_detail {
struct DeviceAllocatorState;
}
namespace upload_detail {
void drainUploads(vulkan_detail::DeviceAllocatorState& state) noexcept;
void closeUploads(vulkan_detail::DeviceAllocatorState& state) noexcept;
[[nodiscard]] bool uploadPendingFor(const vulkan_detail::DeviceAllocatorState& state) noexcept;
} // namespace upload_detail
} // namespace bloom::render
