#pragma once

// Pure, Vulkan-free dispatch-geometry planning for PointResampleV1. Shared by the native
// implementation (real device limits) and the CPU-only kernel/dispatch tests (injected limits), so
// the small-maxX / 65535 boundary behavior is proven without a GPU.

#include <cstdint>

namespace bloom::render {

inline constexpr std::uint32_t kPointResampleLocalSizeX = 256;

struct PointResampleDispatch final {
    std::uint32_t groupsX = 0;
    std::uint32_t groupsY = 0;

    friend bool operator==(const PointResampleDispatch&,
                           const PointResampleDispatch&) noexcept = default;
};

// Plans a 2D dispatch that flattens the output pixel space into 256-wide groups and clamps the X
// dimension to the physical `maxGroupsX`. The Y dimension carries the remainder so a frame whose
// X workgroup count exceeds `maxGroupsX` is still dispatched (a 2D tail) instead of being rejected.
// Returns false only when the pixel count exceeds the physical `maxGroupsX * maxGroupsY` grid
// capacity or an input is zero; the caller treats that as an out-of-capacity refusal.
[[nodiscard]] inline bool planPointResampleDispatch(const std::uint64_t totalPixels,
                                                    const std::uint64_t maxGroupsX,
                                                    const std::uint64_t maxGroupsY,
                                                    PointResampleDispatch& out) noexcept {
    if (totalPixels == 0 || maxGroupsX == 0 || maxGroupsY == 0) {
        return false;
    }
    const std::uint64_t groups =
        (totalPixels + kPointResampleLocalSizeX - 1U) / kPointResampleLocalSizeX;
    const std::uint64_t x = groups < maxGroupsX ? groups : maxGroupsX;
    const std::uint64_t y = (groups + x - 1U) / x;
    if (y > maxGroupsY || x > 0xFFFFFFFFULL || y > 0xFFFFFFFFULL) {
        return false;
    }
    out.groupsX = static_cast<std::uint32_t>(x);
    out.groupsY = static_cast<std::uint32_t>(y);
    return true;
}

} // namespace bloom::render
