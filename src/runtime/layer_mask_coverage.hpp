#pragma once

// ADR 0028 layer-mask coverage: the one CPU reference of the mask math, shared by the CPU
// evaluator and by the GPU program generator (which bakes the same prepared geometry and runs the
// same per-pixel formula). Qt-free and device-free.

#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/render/ocio_gpu_program.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::runtime::detail {

// One mask's authored values sampled at the operation's time.
struct SampledLayerMask final {
    document::LayerMaskMode mode = document::LayerMaskMode::Add;
    bool inverted = false;
    document::PathValue path;
    double feather = 0.0;
    double expansion = 0.0;
    double opacity = 1.0;
};

// One mask ready to evaluate in the image's scaled global pixel space: the flattened outline as
// line segments (x0, y0, x1, y1), its bounds, and the scaled feather width and expansion.
struct PreparedMask final {
    document::LayerMaskMode mode = document::LayerMaskMode::Add;
    bool inverted = false;
    float width = 1.0F;     // max(feather, 1) in scaled pixels
    float expansion = 0.0F; // scaled pixels, positive grows the covered region
    float opacity = 1.0F;
    std::vector<std::array<float, 4>> segments;
    // Host-computed reciprocals shared by both routes, so neither divides per pixel: per segment
    // the crossing slope dx/dy (0 when horizontal) and 1/length^2 (0 when degenerate).
    std::vector<std::array<float, 2>> factors;
    float inverseWidth = 1.0F;
    std::array<float, 4> bounds{}; // minX, minY, maxX, maxY of the segments
};

struct PreparedLayerMasks final {
    std::vector<PreparedMask> masks; // None masks are dropped
    float start = 0.0F;              // accumulation start: 0 or 1
    std::string identity;            // content identity of every prepared value
    std::size_t segmentCount = 0;
};

// Flattens a path into line segments in scaled pixel space. An open path or one with fewer than
// two anchors has no outline (and covers nothing).
[[nodiscard]] std::vector<std::array<float, 4>>
flattenMaskPath(const document::PathValue& path, double horizontalScale, double verticalScale);

// nullopt when a sampled value is outside its schema domain.
[[nodiscard]] std::optional<PreparedLayerMasks>
prepareLayerMasks(std::span<const SampledLayerMask> masks, double horizontalScale,
                  double verticalScale);

// The accumulated coverage at a pixel centre (x, y) in scaled global pixel space. No allocation.
[[nodiscard]] float layerMaskCoverage(const PreparedLayerMasks& masks, float x, float y) noexcept;

// GLSL of the same math for a generated neighbourhood program: defines
// `vec4 bloom_layer_mask(ivec2 c)` reading `bloom_ocio_input` at the image-relative pixel c, whose
// global origin is (originX, originY). nullopt when the geometry exceeds the program limits.
inline constexpr std::size_t kMaximumGpuMaskSegments = 8192;
[[nodiscard]] std::optional<std::string>
layerMaskProgramText(const PreparedLayerMasks& masks, std::int64_t originX, std::int64_t originY);

// The production neighbourhood program for a layerMaskProgramText() body.
[[nodiscard]] render::OcioGpuProgramResult buildLayerMaskProgram(const std::string& shaderText);

} // namespace bloom::runtime::detail
