#include "gpu_scene_coverage.hpp"

#include "cpu_composition_evaluator_support.hpp"
#include "gpu_scene_preparation_private.hpp"

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/path_raster.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <utility>

namespace bloom::runtime::detail {

std::optional<GpuSceneLeafFailure> buildCoverageSolidLeaf(
    const CompiledSolid& solid, const CompiledCompositionPlan& plan,
    const ResolvedEvaluation& resolved, const LayerMatrix& matrix,
    const render::ImageWindow layerWindow, const render::ImageWindow fullDisplayWindow,
    const core::PixelAspectRatio fullPixelAspect, const double hScale, const double vScale,
    const double opacity, const std::shared_ptr<GpuSceneCoverageCache>& coverageCache,
    const GpuSceneChargeBytes& chargeBytes, const CancellationToken& cancellation,
    GpuSceneCoverageSolidCommand& command) {
    const auto width = resolveParameter(solid.width, plan, resolved);
    const auto height = resolveParameter(solid.height, plan, resolved);
    const auto color = resolveParameter(solid.color, plan, resolved);
    if (!width || !height || !color) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Solid parameters are not evaluable");
    }
    const auto pixel = render::solidPixelFromStraightLinearRec709Scene(color->value);
    if (!pixel) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Solid colour is not evaluable");
    }

    OperationKey geometryKey;
    geometryKey.add(std::string{"gpu-coverage-solid-v1"});
    geometryKey.add(matrix.a);
    geometryKey.add(matrix.b);
    geometryKey.add(matrix.c);
    geometryKey.add(matrix.d);
    geometryKey.add(matrix.x);
    geometryKey.add(matrix.y);
    geometryKey.add(hScale);
    geometryKey.add(vScale);
    geometryKey.add(width->value);
    geometryKey.add(height->value);
    addWindowToKey(geometryKey, layerWindow);
    addPixelAspectToKey(geometryKey, fullPixelAspect);
    const auto geometryKeyDigest = geometryKey.digest();

    const auto windowWidth = layerWindow.extent().width();
    const auto windowHeight = layerWindow.extent().height();
    std::shared_ptr<const std::vector<std::uint8_t>> coverage;
    if (coverageCache != nullptr) {
        coverage = coverageCache->find(geometryKeyDigest);
    }
    if (coverage == nullptr) {
        const std::array<render::Path, 1> paths{render::rectanglePath(width->value, height->value)};
        const auto cancel = [&cancellation]() { return cancellation.isCancellationRequested(); };
        const auto matrixPath =
            render::PathMatrix{matrix.a, matrix.b, matrix.c, matrix.d, matrix.x, matrix.y};
        auto raster = render::PathRaster::transformed(paths, {}, matrixPath, hScale, vScale, cancel,
                                                      std::nullopt);
        if (!raster) {
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Coverage geometry is invalid");
        }
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(
            static_cast<std::size_t>(windowWidth) * windowHeight, 0);
        for (std::int64_t y = layerWindow.originY(); y < layerWindow.maxYExclusive(); ++y) {
            const auto offset = static_cast<std::size_t>(y - layerWindow.originY()) * windowWidth;
            if (!raster.value()->coverageRow(
                    layerWindow.originX(), y,
                    std::span<std::uint8_t>(bytes->data() + offset, windowWidth),
                    render::PathFillRule::NonZero, false, cancel)) {
                return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                            "Coverage rasterization was cancelled");
            }
        }
        coverage = std::move(bytes);
        if (coverageCache != nullptr) {
            coverageCache->store(geometryKeyDigest, coverage);
        }
    }

    if (const auto error = chargeBytes(windowWidth, windowHeight, sizeof(render::Rgba32f))) {
        return error;
    }
    command.pixel = *pixel.value();
    command.opacity = static_cast<float>(opacity);
    command.coverage = coverage;
    command.outputWindow = layerWindow;
    command.displayWindow = fullDisplayWindow;
    command.pixelAspect = fullPixelAspect;
    command.geometryKey = geometryKeyDigest;

    OperationKey pixelKey;
    pixelKey.add(geometryKeyDigest);
    pixelKey.add(std::bit_cast<std::uint32_t>(pixel.value()->red()));
    pixelKey.add(std::bit_cast<std::uint32_t>(pixel.value()->green()));
    pixelKey.add(std::bit_cast<std::uint32_t>(pixel.value()->blue()));
    pixelKey.add(std::bit_cast<std::uint32_t>(pixel.value()->alpha()));
    pixelKey.add(std::bit_cast<std::uint32_t>(static_cast<float>(opacity)));
    // The actual CoveredSolidV1 SPIR-V digest the native render operation embeds, so a kernel
    // change can never serve a stale resident image.
    pixelKey.add(std::string{kGpuCoveredSolidSpirvSha256});
    command.semanticKey = pixelKey.digest();
    return std::nullopt;
}

} // namespace bloom::runtime::detail
