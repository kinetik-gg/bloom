#ifndef BLOOM_RUNTIME_GPU_SCENE_PREPARATION_COMMON_HPP
#define BLOOM_RUNTIME_GPU_SCENE_PREPARATION_COMMON_HPP

// Private to src/runtime. The shared vocabulary of the scene-preparation translation units: the
// resolved composition geometry the per-leaf helpers need, the window/aspect key encoders, and the
// fail-closed result type every helper returns. Keeping these in one small header is what lets the
// builder orchestrator, the solid coverage path and the media translation path be separate files
// without each re-deriving the same values.

#include "operation_key.hpp"

#include <bloom/core/blend_mode.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace bloom::runtime::detail {

// The composition-level geometry every leaf resolves against: the full display descriptor, the
// proxy scales, and the authored format extent. One value passed by const reference keeps the
// helpers from recomputing it.
struct GpuSceneCompositionGeometry final {
    render::Rgba32fImageDescriptor fullDescriptor;
    double horizontalScale = 1.0;
    double verticalScale = 1.0;
    double authoredWidth = 0.0;
    double authoredHeight = 0.0;
};

inline void addWindowToKey(OperationKey& key, const render::ImageWindow window) {
    key.add(window.originX());
    key.add(window.originY());
    key.add(window.extent().width());
    key.add(window.extent().height());
}

inline void addPixelAspectToKey(OperationKey& key, const core::PixelAspectRatio ratio) {
    key.add(ratio.numerator());
    key.add(ratio.denominator());
}

// A fail-closed result: a code + message + the CPU media counters accumulated so far. Helpers
// return `std::nullopt` on success and this on refusal; the orchestrator attaches the counters to
// the diagnostic it publishes.
struct GpuSceneLeafFailure final {
    PreparedGpuSceneDiagnosticCode code = PreparedGpuSceneDiagnosticCode::InternalInvariant;
    std::string message;
};

[[nodiscard]] inline std::optional<GpuSceneLeafFailure>
fail(const PreparedGpuSceneDiagnosticCode code, std::string message) {
    return GpuSceneLeafFailure{code, std::move(message)};
}

// The ACTUAL host-retained byte size of one prepared covered command: the immutable
// PathRasterCoverageGeometry row ranges + spans for the vector paths, or the integer-grid text
// leaf's host FreeType bitmap. This is the only host coverage allocation scene preparation retains
// (the GPU R8 mask and the RGBA32F output are executor-time allocations bounded by the executor's
// live-pin ledger), so it is the exact byte count both the builder's retained-byte charger and the
// nested child resident-byte total must use. Never width*height*1: a simple full-width vector has
// O(rows) geometry while a complex one can exceed its R8 mask.
[[nodiscard]] inline std::uint64_t
gpuSceneCoverageHostBytes(const GpuSceneCoverageSolidCommand& command) noexcept {
    if (command.geometry != nullptr) {
        return static_cast<std::uint64_t>(command.geometry->rows.size()) *
                   sizeof(render::PathRasterCoverageRange) +
               static_cast<std::uint64_t>(command.geometry->spans.size()) *
                   sizeof(render::PathRasterCoverageSpan);
    }
    if (command.coverage != nullptr) {
        return static_cast<std::uint64_t>(command.coverage->size());
    }
    return 0;
}

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_PREPARATION_COMMON_HPP
