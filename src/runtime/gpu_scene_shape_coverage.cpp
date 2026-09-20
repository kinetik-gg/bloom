#include "gpu_scene_coverage.hpp"

#include "gpu_scene_preparation_private.hpp"

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/path_raster.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace bloom::runtime::detail {
namespace {

[[nodiscard]] render::Path gpuShapePath(const CompiledShape& shape, const document::Vec2d size) {
    render::Path path;
    const auto w = size.x, h = size.y;
    switch (shape.kind) {
    case document::ShapeKind::Rectangle:
        path = render::rectanglePath(w, h, shape.cornerRadius);
        break;
    case document::ShapeKind::Ellipse:
        path = render::ellipsePath(w, h);
        break;
    case document::ShapeKind::Triangle:
        path = render::polygonPath(w, h, 3);
        break;
    case document::ShapeKind::Polygon:
        path =
            render::polygonPath(w, h, static_cast<std::uint32_t>(shape.points), shape.cornerRadius);
        break;
    case document::ShapeKind::Star:
        path = render::starPath(w, h, static_cast<std::uint32_t>(shape.points), shape.innerRatio);
        break;
    case document::ShapeKind::Line:
        path = render::linePath({shape.lineStart.x, shape.lineStart.y},
                                {shape.lineEnd.x, shape.lineEnd.y});
        break;
    case document::ShapeKind::Path:
        path.closed = shape.path.closed;
        for (const auto& anchor : shape.path.anchors) {
            render::PathAnchor a{{anchor.point.x, anchor.point.y}, {}, {}};
            if (anchor.inHandle) {
                a.inHandle = render::PathPoint{anchor.inHandle->x, anchor.inHandle->y};
            }
            if (anchor.outHandle) {
                a.outHandle = render::PathPoint{anchor.outHandle->x, anchor.outHandle->y};
            }
            path.anchors.push_back(a);
        }
        break;
    }
    return path;
}

void addShapeGeometryToKey(OperationKey& key, const CompiledShape& shape,
                           const document::Vec2d size) {
    key.add(shape.kind);
    key.add(size.x);
    key.add(size.y);
    key.add(shape.cornerRadius);
    key.add(shape.points);
    key.add(shape.innerRatio);
    key.add(shape.lineStart);
    key.add(shape.lineEnd);
    key.add(shape.path);
    key.add(shape.fillRule);
}

} // namespace

std::optional<GpuSceneLeafFailure>
resolveShapeLeafWindow(const CompiledShape& shape, const CompiledCompositionPlan& plan,
                       const ResolvedEvaluation& resolved, const double hScale, const double vScale,
                       const CancellationToken& cancellation,
                       std::optional<render::ImageWindow>& window, ContentBounds& local) {
    window = std::nullopt;
    local = {};
    const auto size = resolveParameter(shape.size, plan, resolved);
    const auto fill = resolveParameter(shape.fillColor, plan, resolved);
    const auto strokeColor = resolveParameter(shape.strokeColor, plan, resolved);
    const auto width = resolveParameter(shape.strokeWidth, plan, resolved);
    if (!size || !fill || !strokeColor || !width || !std::isfinite(size->value.x) ||
        !std::isfinite(size->value.y) || size->value.x < 0.0 || size->value.y < 0.0 ||
        !std::isfinite(width->value) || width->value < 0.0) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Shape parameters are invalid");
    }
    const bool fillEnabled = shape.fillEnabled && shape.kind != document::ShapeKind::Line;
    const bool strokeEnabled = shape.strokeEnabled && width->value > 0.0;
    if (!fillEnabled && !strokeEnabled) {
        window = std::nullopt;
        return std::nullopt;
    }
    const auto cancel = [&cancellation]() { return cancellation.isCancellationRequested(); };
    const auto raster =
        render::PathRaster::create(gpuShapePath(shape, size->value),
                                   {strokeEnabled ? width->value : 0.0,
                                    static_cast<render::PathStrokeAlign>(shape.strokeAlign),
                                    static_cast<render::PathStrokeJoin>(shape.strokeJoin),
                                    static_cast<render::PathStrokeCap>(shape.strokeCap)},
                                   hScale, vScale, cancel);
    if (!raster) {
        if (cancellation.isCancellationRequested()) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                        "Shape geometry rasterization was cancelled");
        }
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Shape geometry could not be rasterized");
    }
    const auto extent = raster.value()->bounds(fillEnabled, strokeEnabled);
    if (!(extent.right > extent.left) || !(extent.bottom > extent.top)) {
        window = std::nullopt;
        return std::nullopt;
    }
    local = {extent.left, extent.top, extent.right, extent.bottom};
    const auto left = std::floor(extent.left * hScale);
    const auto top = std::floor(extent.top * vScale);
    const auto right = std::ceil(extent.right * hScale);
    const auto bottom = std::ceil(extent.bottom * vScale);
    constexpr double maxCoordinate = 9007199254740991.0;
    const auto finiteCoordinate = [](const double value) {
        return std::isfinite(value) && std::abs(value) <= maxCoordinate;
    };
    if (!finiteCoordinate(left) || !finiteCoordinate(top) || !finiteCoordinate(right) ||
        !finiteCoordinate(bottom) ||
        right - left > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        bottom - top > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Shape bounds exceed the supported coordinate range");
    }
    const auto created = render::ImageWindow::create(
        static_cast<std::int64_t>(left), static_cast<std::int64_t>(top),
        static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top));
    if (!created) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Shape bounds are invalid");
    }
    window = *created.value();
    return std::nullopt;
}

std::optional<GpuSceneLeafFailure> buildShapeCoverage(
    const CompiledShape& shape, const CompiledCompositionPlan& plan,
    const ResolvedEvaluation& resolved, const LayerMatrix& matrix,
    const render::ImageWindow layerWindow, const render::ImageWindow fullDisplayWindow,
    const core::PixelAspectRatio fullPixelAspect, const double hScale, const double vScale,
    const double opacity, const std::shared_ptr<GpuSceneCoverageCache>& coverageCache,
    const GpuSceneChargeBytes& chargeBytes, const CancellationToken& cancellation,
    GpuSceneShapeCoverage& out) {
    const auto size = resolveParameter(shape.size, plan, resolved);
    const auto fillColor = resolveParameter(shape.fillColor, plan, resolved);
    const auto strokeColor = resolveParameter(shape.strokeColor, plan, resolved);
    const auto strokeWidth = resolveParameter(shape.strokeWidth, plan, resolved);
    if (!size || !fillColor || !strokeColor || !strokeWidth) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Shape parameters are not evaluable");
    }
    const bool fillEnabled = shape.fillEnabled && shape.kind != document::ShapeKind::Line;
    const bool strokeEnabled = shape.strokeEnabled && strokeWidth->value > 0.0;
    out.opacity = static_cast<float>(opacity);
    out.needsPostOpacity = false;
    if (!fillEnabled && !strokeEnabled) {
        return std::nullopt;
    }
    const auto cancel = [&cancellation]() { return cancellation.isCancellationRequested(); };
    const std::array<render::Path, 1> paths{gpuShapePath(shape, size->value)};
    const render::PathStroke stroke{strokeEnabled ? strokeWidth->value : 0.0,
                                    static_cast<render::PathStrokeAlign>(shape.strokeAlign),
                                    static_cast<render::PathStrokeJoin>(shape.strokeJoin),
                                    static_cast<render::PathStrokeCap>(shape.strokeCap)};
    const auto matrixPath =
        render::PathMatrix{matrix.a, matrix.b, matrix.c, matrix.d, matrix.x, matrix.y};
    auto raster = render::PathRaster::transformed(paths, stroke, matrixPath, hScale, vScale, cancel,
                                                  std::nullopt);
    if (!raster) {
        if (cancellation.isCancellationRequested()) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                        "Shape coverage rasterization was cancelled");
        }
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Shape coverage geometry is invalid");
    }
    const auto windowWidth = layerWindow.extent().width();
    const auto windowHeight = layerWindow.extent().height();
    const auto rule = static_cast<render::PathFillRule>(shape.fillRule);
    const auto rasterize = [&](const bool strokePass, const std::string& geometryKeyDigest,
                               std::shared_ptr<const render::PathRasterCoverageGeometry>& geometry)
        -> std::optional<GpuSceneLeafFailure> {
        if (coverageCache != nullptr) {
            geometry = coverageCache->find(geometryKeyDigest);
        }
        if (geometry != nullptr) {
            return std::nullopt;
        }
        auto built =
            raster.value()->coverageGeometry(layerWindow.originX(), layerWindow.originY(),
                                             windowWidth, windowHeight, rule, strokePass, cancel);
        if (!built) {
            if (cancellation.isCancellationRequested()) {
                return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                            "Shape coverage rasterization was cancelled");
            }
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Shape coverage geometry is invalid");
        }
        geometry =
            std::make_shared<const render::PathRasterCoverageGeometry>(std::move(*built.value()));
        if (coverageCache != nullptr) {
            coverageCache->store(geometryKeyDigest, geometry);
        }
        return std::nullopt;
    };

    if (fillEnabled) {
        const auto pixel = render::solidPixelFromStraightLinearRec709Scene(fillColor->value);
        if (!pixel) {
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Shape fill colour is not evaluable");
        }
        OperationKey geometryKey;
        geometryKey.add(std::string{"gpu-shape-fill-coverage-v1"});
        addShapeGeometryToKey(geometryKey, shape, size->value);
        geometryKey.add(matrix.a);
        geometryKey.add(matrix.b);
        geometryKey.add(matrix.c);
        geometryKey.add(matrix.d);
        geometryKey.add(matrix.x);
        geometryKey.add(matrix.y);
        geometryKey.add(hScale);
        geometryKey.add(vScale);
        addWindowToKey(geometryKey, layerWindow);
        addPixelAspectToKey(geometryKey, fullPixelAspect);
        const auto geometryKeyDigest = geometryKey.digest();
        std::shared_ptr<const render::PathRasterCoverageGeometry> geometry;
        if (const auto error = rasterize(false, geometryKeyDigest, geometry)) {
            return error;
        }
        if (const auto error = chargeBytes(windowWidth, windowHeight, sizeof(render::Rgba32f))) {
            return error;
        }
        // Only a lone fill folds the layer opacity into the palette; with a stroke present the
        // opacity must be applied after the stroke-over-fill composition.
        const float fillOpacity = strokeEnabled ? 1.0F : static_cast<float>(opacity);
        OperationKey pixelKey;
        pixelKey.add(geometryKeyDigest);
        pixelKey.add(std::bit_cast<std::uint32_t>(pixel.value()->red()));
        pixelKey.add(std::bit_cast<std::uint32_t>(pixel.value()->green()));
        pixelKey.add(std::bit_cast<std::uint32_t>(pixel.value()->blue()));
        pixelKey.add(std::bit_cast<std::uint32_t>(pixel.value()->alpha()));
        pixelKey.add(std::bit_cast<std::uint32_t>(fillOpacity));
        pixelKey.add(std::string{kGpuCoveredSolidSpirvSha256});
        out.fill = GpuSceneCoverageSolidCommand{.index = kInvalidGpuSceneCommand,
                                                .sourceOperation = OperationIndex::fromRaw(0),
                                                .pixel = *pixel.value(),
                                                .opacity = fillOpacity,
                                                .geometry = geometry,
                                                .coverage = nullptr,
                                                .outputWindow = layerWindow,
                                                .displayWindow = fullDisplayWindow,
                                                .pixelAspect = fullPixelAspect,
                                                .geometryKey = geometryKeyDigest,
                                                .semanticKey = pixelKey.digest()};
    }
    if (strokeEnabled) {
        const auto pixel = render::solidPixelFromStraightLinearRec709Scene(strokeColor->value);
        if (!pixel) {
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Shape stroke colour is not evaluable");
        }
        OperationKey geometryKey;
        geometryKey.add(std::string{"gpu-shape-stroke-coverage-v1"});
        addShapeGeometryToKey(geometryKey, shape, size->value);
        geometryKey.add(shape.strokeAlign);
        geometryKey.add(shape.strokeJoin);
        geometryKey.add(shape.strokeCap);
        geometryKey.add(strokeWidth->value);
        geometryKey.add(matrix.a);
        geometryKey.add(matrix.b);
        geometryKey.add(matrix.c);
        geometryKey.add(matrix.d);
        geometryKey.add(matrix.x);
        geometryKey.add(matrix.y);
        geometryKey.add(hScale);
        geometryKey.add(vScale);
        addWindowToKey(geometryKey, layerWindow);
        addPixelAspectToKey(geometryKey, fullPixelAspect);
        const auto geometryKeyDigest = geometryKey.digest();
        std::shared_ptr<const render::PathRasterCoverageGeometry> geometry;
        if (const auto error = rasterize(true, geometryKeyDigest, geometry)) {
            return error;
        }
        if (const auto error = chargeBytes(windowWidth, windowHeight, sizeof(render::Rgba32f))) {
            return error;
        }
        const float strokeOpacity = fillEnabled ? 1.0F : static_cast<float>(opacity);
        OperationKey pixelKey;
        pixelKey.add(geometryKeyDigest);
        pixelKey.add(std::bit_cast<std::uint32_t>(pixel.value()->red()));
        pixelKey.add(std::bit_cast<std::uint32_t>(pixel.value()->green()));
        pixelKey.add(std::bit_cast<std::uint32_t>(pixel.value()->blue()));
        pixelKey.add(std::bit_cast<std::uint32_t>(pixel.value()->alpha()));
        pixelKey.add(std::bit_cast<std::uint32_t>(strokeOpacity));
        pixelKey.add(std::string{kGpuCoveredSolidSpirvSha256});
        out.stroke = GpuSceneCoverageSolidCommand{.index = kInvalidGpuSceneCommand,
                                                  .sourceOperation = OperationIndex::fromRaw(0),
                                                  .pixel = *pixel.value(),
                                                  .opacity = strokeOpacity,
                                                  .geometry = geometry,
                                                  .coverage = nullptr,
                                                  .outputWindow = layerWindow,
                                                  .displayWindow = fullDisplayWindow,
                                                  .pixelAspect = fullPixelAspect,
                                                  .geometryKey = geometryKeyDigest,
                                                  .semanticKey = pixelKey.digest()};
    }
    out.needsPostOpacity = fillEnabled && strokeEnabled && static_cast<float>(opacity) != 1.0F;
    return std::nullopt;
}

} // namespace bloom::runtime::detail
