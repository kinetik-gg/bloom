#pragma once

// Adopts the opaque integer instance bits carried by GpuBorrowedInstanceView into the typed
// VkInstance that QVulkanInstance requires. The public contract deliberately carries native handles
// as integer bits only; reconstructing the typed handle is the single necessary int-to-pointer
// boundary and has no safer equivalent. This mirrors the production
// bloom::render::handleFromBits boundary and is repeated here only because these UI fixtures never
// include the render Vulkan private header.

#include <vulkan/vulkan_core.h>

#include <cstdint>

namespace bloom::ui::test {

[[nodiscard]] inline VkInstance borrowedInstance(const std::uint64_t bits) noexcept {
    // NOLINTNEXTLINE(performance-no-int-to-ptr): documented native-handle reconstruction boundary.
    return reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(bits));
}

} // namespace bloom::ui::test
