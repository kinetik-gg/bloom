#ifndef BLOOM_RUNTIME_GPU_SCENE_COVERAGE_HPP
#define BLOOM_RUNTIME_GPU_SCENE_COVERAGE_HPP

// Private to src/runtime. The solid vector-coverage path: a fractional-device-grid solid layer
// whose CPU evaluation the evaluator performs through render::PathRaster. This reproduces that
// exact raster (same rectangle path, same matrix, same fill rule) and builds the resolved
// GpuSceneCoverageSolidCommand. It never materialises the RGBA image; only the R8 coverage is
// allocated, and only through the caller's bounded charger.

#include "gpu_scene_preparation_common.hpp"

#include "cpu_composition_resolution.hpp"
#include "layer_parent_transform.hpp"

#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace bloom::runtime::detail {

// Charges one prepared command's resident bytes; returns a failure when the request allowance would
// be exceeded. The orchestrator owns the running total.
using GpuSceneChargeBytes = std::function<std::optional<GpuSceneLeafFailure>(
    std::uint64_t width, std::uint64_t height, std::uint64_t bytesPerPixel)>;

// Builds the coverage command for a solid layer whose authored matrix is NOT on the integer device
// grid. `matrix` is the same authored LayerMatrix the evaluator keys the vector chain by;
// `layerWindow` its support bounds; `hScale`/`vScale` the proxy scales the raster uses; `opacity`
// the already-resolved layer opacity. On success the command (sans index/sourceOperation) is filled
// and nullopt is returned; on refusal the failure is returned and the command is unspecified.
[[nodiscard]] std::optional<GpuSceneLeafFailure> buildCoverageSolidLeaf(
    const CompiledSolid& solid, const CompiledCompositionPlan& plan,
    const ResolvedEvaluation& resolved, const LayerMatrix& matrix,
    const render::ImageWindow layerWindow, const render::ImageWindow fullDisplayWindow,
    const core::PixelAspectRatio fullPixelAspect, double hScale, double vScale, double opacity,
    const std::shared_ptr<GpuSceneCoverageCache>& coverageCache,
    const GpuSceneChargeBytes& chargeBytes, const CancellationToken& cancellation,
    GpuSceneCoverageSolidCommand& command);

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_COVERAGE_HPP
