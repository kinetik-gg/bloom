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

// The CPU text leaf's own process data window: the exact window the evaluator's CompiledText arm
// publishes for this request, computed from the same TextCoverageBitmap raster and box arithmetic
// with the proxy-scaled raster parameters. A Layer Output fed by this text resolves its content
// bounds centre from it, so preparation must reproduce it before it can build the layer matrix. A
// text whose coverage is empty and which has no box produces no image: `window` is then disengaged
// and the caller publishes nothing, exactly like the evaluator. `coverageByteLimit` bounds the
// transient raster the same way the evaluator's pixel budget does.
[[nodiscard]] std::optional<GpuSceneLeafFailure>
resolveTextLeafWindow(const CompiledText& text, const CompiledCompositionPlan& plan,
                      const ResolvedEvaluation& resolved, std::uint64_t coverageByteLimit,
                      double hScale, double vScale, const CancellationToken& cancellation,
                      std::optional<render::ImageWindow>& window);

// Builds the vector-coverage command for a text layer whose authored matrix is NOT on the integer
// device grid. Text is a single premultiplied colour through an 8-bit linear-area glyph coverage,
// so it maps exactly onto CoveredSolidV1: the host rasterizes the real render::textOutlines through
// the same PathRaster the evaluator's vector arm uses and the native kernel selects the stored
// palette entry, never uploading a CPU-rendered RGBA frame. `matrix`/`layerWindow`/`hScale`/
// `vScale`/`opacity` carry the same meaning as for a solid.
[[nodiscard]] std::optional<GpuSceneLeafFailure>
buildTextCoverageLeaf(const CompiledText& text, const CompiledCompositionPlan& plan,
                      const ResolvedEvaluation& resolved, const LayerMatrix& matrix,
                      const render::ImageWindow layerWindow,
                      const render::ImageWindow fullDisplayWindow,
                      const core::PixelAspectRatio fullPixelAspect, double hScale, double vScale,
                      double opacity, const std::shared_ptr<GpuSceneCoverageCache>& coverageCache,
                      const GpuSceneChargeBytes& chargeBytes, const CancellationToken& cancellation,
                      GpuSceneCoverageSolidCommand& command);

// The prepared native coverage for one shape layer. A shape with only a fill is a single
// GpuSceneCoverageSolidCommand; a fill plus a stroke is two coverage commands the caller composes
// with the existing SourceOverV1 merge in CPU order. `postOpacity` is present exactly when the
// layer opacity must be applied AFTER the fill-over-stroke composition (both present and opacity !=
// 1): the caller appends an identity TranslationOpacityBilinearV1 command (translation 0, layer
// opacity) which is the exact CPU post-multiply. `opacity` is the resolved layer opacity.
struct GpuSceneShapeCoverage final {
    std::optional<GpuSceneCoverageSolidCommand> fill;
    std::optional<GpuSceneCoverageSolidCommand> stroke;
    bool needsPostOpacity = false;
    float opacity = 1.0F;

    [[nodiscard]] bool hasFill() const noexcept { return fill.has_value(); }
    [[nodiscard]] bool hasStroke() const noexcept { return stroke.has_value(); }
};

// The CPU shape leaf's own process data window (PathRaster::create bounds, proxy-scaled floor/ceil
// window), or disengaged when the shape produces no image (no fill and no stroke, or empty bounds).
// `local` receives the leaf's authored content bounds (the raw PathRaster bounds, exactly what the
// evaluator stores in bounds.local/output), which differ from the window's window/scales.
[[nodiscard]] std::optional<GpuSceneLeafFailure>
resolveShapeLeafWindow(const CompiledShape& shape, const CompiledCompositionPlan& plan,
                       const ResolvedEvaluation& resolved, double hScale, double vScale,
                       const CancellationToken& cancellation,
                       std::optional<render::ImageWindow>& window, ContentBounds& local);

// Builds the native vector-coverage commands for a shape layer whose authored matrix is NOT on the
// integer device grid. Fill and stroke coverage are the real render::PathRaster rasters of the same
// shapePath the evaluator's vector arm uses; colours are the resolved premultiplied pixels. No CPU
// RGBA frame is materialised.
[[nodiscard]] std::optional<GpuSceneLeafFailure>
buildShapeCoverage(const CompiledShape& shape, const CompiledCompositionPlan& plan,
                   const ResolvedEvaluation& resolved, const LayerMatrix& matrix,
                   const render::ImageWindow layerWindow,
                   const render::ImageWindow fullDisplayWindow,
                   const core::PixelAspectRatio fullPixelAspect, double hScale, double vScale,
                   double opacity, const std::shared_ptr<GpuSceneCoverageCache>& coverageCache,
                   const GpuSceneChargeBytes& chargeBytes, const CancellationToken& cancellation,
                   GpuSceneShapeCoverage& coverage);

// Builds the leaf-image coverage command for a text layer whose authored matrix IS on the integer
// device grid. The CPU evaluator's integer arm rasterizes the text at the proxy-scaled em size into
// its own window (render::TextCoverageBitmap) and then translates it by an exact integer; this
// reproduces that raster and places it directly at the layer window. A text box is not reproduced
// on this arm and fails closed.
[[nodiscard]] std::optional<GpuSceneLeafFailure> buildTextLeafCoverageLeaf(
    const CompiledText& text, const CompiledCompositionPlan& plan,
    const ResolvedEvaluation& resolved, const render::ImageWindow layerWindow,
    const render::ImageWindow fullDisplayWindow, const core::PixelAspectRatio fullPixelAspect,
    double hScale, double vScale, double opacity, std::uint64_t coverageByteLimit,
    const std::shared_ptr<GpuSceneCoverageCache>& coverageCache,
    const GpuSceneChargeBytes& chargeBytes, const CancellationToken& cancellation,
    GpuSceneCoverageSolidCommand& command);

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_COVERAGE_HPP
