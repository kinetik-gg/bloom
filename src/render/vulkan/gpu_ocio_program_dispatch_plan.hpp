#ifndef BLOOM_RENDER_VULKAN_GPU_OCIO_PROGRAM_DISPATCH_PLAN_HPP
#define BLOOM_RENDER_VULKAN_GPU_OCIO_PROGRAM_DISPATCH_PLAN_HPP

// Pure, Vulkan-free compute dispatch planning for the OCIO wrapper. The wrapper's one-dimensional
// local size (64) means a 1D dispatch of `ceil(pixelCount / 64)` groups can exceed the conformant
// `maxComputeWorkGroupCount[0]` floor (65535) for images above ~4.19M pixels even when the image
// and its memory fit. The planner lays the same work out as a capacity-bounded 2D grid; the
// wrapper flattens the 2D global invocation id back into one linear pixel index and the shader's
// own `if (index >= pixelCount) return;` guard discards the tail. No artificial pixel ceiling is
// introduced: the only refusals are real device-capacity or uint32-index-representability limits.
//
// This header has no Vulkan/OCIO/Qt dependency so a pure unit test can inject arbitrary device
// limits.

#include <cstdint>
#include <limits>

namespace bloom::render::ocio_program_detail {

enum class OcioDispatchPlanError : std::uint8_t {
    None,
    // The requested pixel count or workgroup size cannot be planned (empty/zero input).
    InvalidGeometry,
    // No (groupsX, groupsY) within the supplied limits can cover the work, or the flattened
    // invocation index would not be representable in the shader's uint32 index.
    DeviceCapacity,
};

struct OcioDispatchPlan final {
    std::uint32_t groupsX = 0;
    std::uint32_t groupsY = 0;
    OcioDispatchPlanError error = OcioDispatchPlanError::InvalidGeometry;

    [[nodiscard]] bool valid() const noexcept { return error == OcioDispatchPlanError::None; }
};

// Effective maxComputeWorkGroupCount axis: a nonzero caller request may only LOWER the physical
// device limit and can never authorize a vkCmdDispatch above it; zero selects the physical limit.
[[nodiscard]] inline std::uint32_t effectiveWorkGroupCount(const std::uint32_t requested,
                                                           const std::uint32_t physical) noexcept {
    if (requested == 0 || requested >= physical) {
        return physical;
    }
    return requested;
}

// Chooses (groupsX, groupsY) with:
//   groupsX <= maxGroupsX, groupsY <= maxGroupsY,
//   groupsX * groupsY * workgroupSizeX >= pixelCount,
//   groupsX * workgroupSizeX <= UINT32_MAX (the shader's uint32 stride multiply), and
//   groupsX * workgroupSizeX * groupsY <= 2^32 (the flattened index stays in uint32).
// For a geometry that already fits, it returns the exact 1D plan (groupsY == 1), so existing
// dispatches are byte-for-byte unchanged. Every arithmetic step is checked before use; an
// unrepresentable request is refused DeviceCapacity rather than wrapping.
[[nodiscard]] inline OcioDispatchPlan planOcioDispatch(const std::uint64_t pixelCount,
                                                       const std::uint32_t workgroupSizeX,
                                                       const std::uint32_t maxGroupsX,
                                                       const std::uint32_t maxGroupsY) noexcept {
    constexpr std::uint64_t kUint32Max = std::numeric_limits<std::uint32_t>::max();
    constexpr std::uint64_t kUint32Count = kUint32Max + 1ULL; // 2^32
    if (pixelCount == 0 || workgroupSizeX == 0 || maxGroupsX == 0 || maxGroupsY == 0) {
        return {};
    }
    // The wrapper indexes the resident image with a uint32 global index.
    if (pixelCount > kUint32Max) {
        return {0, 0, OcioDispatchPlanError::DeviceCapacity};
    }
    const std::uint64_t groups =
        (pixelCount + static_cast<std::uint64_t>(workgroupSizeX) - 1ULL) / workgroupSizeX;
    if (groups == 0) {
        return {};
    }
    // gl_NumWorkGroups.x * gl_WorkGroupSize.x is a uint32 multiply in GLSL; cap groupsX so the
    // stride cannot wrap, then never let the dispatch exceed maxGroupsX.
    std::uint64_t maxX = maxGroupsX;
    const std::uint64_t strideCap = kUint32Max / workgroupSizeX;
    if (maxX > strideCap) {
        maxX = strideCap;
    }
    if (maxX == 0) {
        return {0, 0, OcioDispatchPlanError::DeviceCapacity};
    }
    const std::uint64_t groupsX = groups < maxX ? groups : maxX;
    const std::uint64_t groupsY = (groups + groupsX - 1ULL) / groupsX;
    if (groupsY > maxGroupsY) {
        return {0, 0, OcioDispatchPlanError::DeviceCapacity};
    }
    // `groupsX * workgroupSizeX <= UINT32_MAX` by construction, so this product cannot overflow
    // uint64 for the uint32 `groupsY`.
    const std::uint64_t totalInvocations = groupsX * workgroupSizeX * groupsY;
    if (totalInvocations > kUint32Count) {
        return {0, 0, OcioDispatchPlanError::DeviceCapacity};
    }
    return {static_cast<std::uint32_t>(groupsX), static_cast<std::uint32_t>(groupsY),
            OcioDispatchPlanError::None};
}

} // namespace bloom::render::ocio_program_detail

#endif // BLOOM_RENDER_VULKAN_GPU_OCIO_PROGRAM_DISPATCH_PLAN_HPP
