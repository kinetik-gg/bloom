#include "gpu_scene_coverage.hpp"

#include "cpu_composition_evaluator_support.hpp"
#include "gpu_scene_preparation_private.hpp"

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/path_raster.hpp>
#include <bloom/render/text_raster.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace bloom::runtime::detail {
namespace {

// Encodes the resolved text face exactly as the evaluator's semantic cache does: an embedded face
// is its closed enum, an external face its content digest plus face index. Never a path or pointer.
void addTextFontToKey(OperationKey& key, const render::TextFont& font) {
    std::visit(
        [&key](const auto& face) {
            using Face = std::decay_t<decltype(face)>;
            if constexpr (std::is_same_v<Face, render::EmbeddedFace>) {
                key.add(face);
            } else {
                const auto digest = face.contentDigest.toLowercaseHex();
                key.add(std::string(digest.data(), digest.size()));
                key.add(face.faceIndex);
            }
        },
        font);
}

// The evaluator's own text layout options for the vector arm: outline units are AUTHOR space, so
// spacing and the box stay unscaled and PathRaster::transformed applies the proxy scales.
[[nodiscard]] render::TextLayoutOptions vectorTextLayout(const CompiledText& text,
                                                         const std::int64_t alignment,
                                                         const double lineHeight,
                                                         const double letterSpacing) {
    render::TextLayoutOptions layout;
    layout.alignment = static_cast<render::TextAlignment>(alignment);
    layout.lineHeight = lineHeight;
    layout.letterSpacing = letterSpacing;
    layout.multiline = true;
    layout.boxWidth = text.layout.box.x;
    layout.boxHeight = text.layout.box.y;
    layout.wrap = text.layout.wrap;
    layout.verticalAlignment =
        static_cast<render::TextLayoutOptions::VerticalAlignment>(text.layout.verticalAlignment);
    layout.anchorMode = static_cast<render::TextLayoutOptions::AnchorMode>(text.layout.anchorMode);
    layout.overflow = static_cast<render::TextLayoutOptions::Overflow>(text.layout.overflow);
    return layout;
}

// Mirrors the evaluator's shapePath (src/runtime/vector_geometry.ipp); duplicated under a distinct
// name because that definition has external linkage in the evaluator translation unit.
} // namespace

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
    std::shared_ptr<const render::PathRasterCoverageGeometry> geometry;
    if (coverageCache != nullptr) {
        geometry = coverageCache->find(geometryKeyDigest);
    }
    if (geometry == nullptr) {
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
        auto built = raster.value()->coverageGeometry(layerWindow.originX(), layerWindow.originY(),
                                                      windowWidth, windowHeight,
                                                      render::PathFillRule::NonZero, false, cancel);
        if (!built) {
            if (cancellation.isCancellationRequested()) {
                return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                            "Coverage rasterization was cancelled");
            }
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Coverage geometry is invalid");
        }
        geometry =
            std::make_shared<const render::PathRasterCoverageGeometry>(std::move(*built.value()));
        if (coverageCache != nullptr) {
            coverageCache->store(geometryKeyDigest, geometry);
        }
    }

    if (const auto error = chargeBytes(windowWidth, windowHeight, sizeof(render::Rgba32f))) {
        return error;
    }
    command.pixel = *pixel.value();
    command.opacity = static_cast<float>(opacity);
    command.geometry = geometry;
    command.coverage = nullptr;
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

std::optional<GpuSceneLeafFailure>
resolveTextLeafWindow(const CompiledText& text, const CompiledCompositionPlan& plan,
                      const ResolvedEvaluation& resolved, const std::uint64_t coverageByteLimit,
                      const double hScale, const double vScale,
                      const CancellationToken& cancellation,
                      std::optional<render::ImageWindow>& window) {
    const auto size = resolveParameter(text.size, plan, resolved);
    const auto content =
        resolveParameter(text.contentParameterId, text.content, text.drivenContent, resolved);
    const auto lineHeight = resolveParameter(text.layout.lineHeight, plan, resolved);
    const auto letterSpacing = resolveParameter(text.layout.letterSpacing, plan, resolved);
    const auto alignment = resolveParameter(text.layout.alignmentId, text.layout.alignment,
                                            text.layout.drivenAlignment, resolved);
    if (!size || !content || !lineHeight || !letterSpacing || !alignment) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Text parameters are not evaluable");
    }
    // The evaluator's leaf arm rasterizes at the proxy-scaled em size, so its published data window
    // is the scaled coverage extent, not the author-space one the vector arm uses.
    const auto parameters =
        render::TextRasterParameters::create(size->value * hScale, size->value * vScale);
    if (!parameters) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Text size is not rasterizable");
    }
    render::TextLayoutOptions layout;
    layout.alignment = static_cast<render::TextAlignment>(alignment->value);
    layout.lineHeight = lineHeight->value;
    layout.letterSpacing = letterSpacing->value * hScale;
    layout.multiline = true;
    layout.boxWidth = text.layout.box.x * hScale;
    layout.boxHeight = text.layout.box.y * vScale;
    layout.wrap = text.layout.wrap;
    layout.verticalAlignment =
        static_cast<render::TextLayoutOptions::VerticalAlignment>(text.layout.verticalAlignment);
    layout.anchorMode = static_cast<render::TextLayoutOptions::AnchorMode>(text.layout.anchorMode);
    layout.overflow = static_cast<render::TextLayoutOptions::Overflow>(text.layout.overflow);

    const auto byteLimit =
        coverageByteLimit > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(coverageByteLimit);
    auto coverage = render::TextCoverageBitmap::rasterizeText(
        text.font, content->value, *parameters.value(), byteLimit, layout);
    if (!coverage) {
        if (cancellation.isCancellationRequested()) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                        "Text rasterization was cancelled");
        }
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Text content could not be rasterized");
    }
    const bool hasBox = layout.boxWidth > 0.0 && layout.boxHeight > 0.0;
    if (!coverage.value()->hasCoverage() && !hasBox) {
        window = std::nullopt;
        return std::nullopt;
    }
    const auto extent = [](const double value) -> std::optional<std::uint64_t> {
        if (!std::isfinite(value) || value <= 0.0 ||
            value > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(std::ceil(value));
    };
    auto originX = coverage.value()->originX();
    auto originY = coverage.value()->originY();
    auto width = static_cast<std::uint64_t>(coverage.value()->width());
    auto height = static_cast<std::uint64_t>(coverage.value()->height());
    if (hasBox) {
        const auto boxWidth = extent(layout.boxWidth);
        const auto boxHeight = extent(layout.boxHeight);
        if (!boxWidth || !boxHeight) {
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Text box bounds are invalid");
        }
        const auto boxRight = static_cast<std::int64_t>(*boxWidth);
        const auto boxBottom = static_cast<std::int64_t>(*boxHeight);
        if (layout.overflow == render::TextLayoutOptions::Overflow::Grow) {
            const auto contentRight =
                coverage.value()->originX() + static_cast<std::int64_t>(coverage.value()->width());
            const auto contentBottom =
                coverage.value()->originY() + static_cast<std::int64_t>(coverage.value()->height());
            originX = std::min<std::int64_t>(0, coverage.value()->originX());
            originY = std::min<std::int64_t>(0, coverage.value()->originY());
            const auto right = std::max(boxRight, contentRight);
            const auto bottom = std::max(boxBottom, contentBottom);
            width = static_cast<std::uint64_t>(right - originX);
            height = static_cast<std::uint64_t>(bottom - originY);
        } else {
            originX = 0;
            originY = 0;
            width = *boxWidth;
            height = *boxHeight;
        }
    }
    const auto created = render::ImageWindow::create(originX, originY, width, height);
    if (!created) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Text bounds are invalid");
    }
    window = *created.value();
    return std::nullopt;
}

std::optional<GpuSceneLeafFailure> buildTextCoverageLeaf(
    const CompiledText& text, const CompiledCompositionPlan& plan,
    const ResolvedEvaluation& resolved, const LayerMatrix& matrix,
    const render::ImageWindow layerWindow, const render::ImageWindow fullDisplayWindow,
    const core::PixelAspectRatio fullPixelAspect, const double hScale, const double vScale,
    const double opacity, const std::shared_ptr<GpuSceneCoverageCache>& coverageCache,
    const GpuSceneChargeBytes& chargeBytes, const CancellationToken& cancellation,
    GpuSceneCoverageSolidCommand& command) {
    const auto size = resolveParameter(text.size, plan, resolved);
    const auto color = resolveParameter(text.color, plan, resolved);
    const auto content =
        resolveParameter(text.contentParameterId, text.content, text.drivenContent, resolved);
    const auto lineHeight = resolveParameter(text.layout.lineHeight, plan, resolved);
    const auto spacing = resolveParameter(text.layout.letterSpacing, plan, resolved);
    const auto alignment = resolveParameter(text.layout.alignmentId, text.layout.alignment,
                                            text.layout.drivenAlignment, resolved);
    if (!size || !color || !content || !lineHeight || !spacing || !alignment) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Text parameters are not evaluable");
    }
    const auto pixel = render::solidPixelFromStraightLinearRec709Scene(color->value);
    if (!pixel) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Text colour is not evaluable");
    }
    const auto parameters = render::TextRasterParameters::create(size->value, size->value);
    if (!parameters) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Text size is not rasterizable");
    }
    const auto layout = vectorTextLayout(text, alignment->value, lineHeight->value, spacing->value);
    const auto cancel = [&cancellation]() { return cancellation.isCancellationRequested(); };
    auto outlines =
        render::textOutlines(text.font, content->value, *parameters.value(), layout, cancel);
    if (!outlines) {
        if (cancellation.isCancellationRequested()) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Text outlines were cancelled");
        }
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Text outlines are unavailable");
    }
    std::optional<render::PathBounds> clip;
    if (layout.boxWidth > 0.0 && layout.overflow == render::TextLayoutOptions::Overflow::Clip) {
        clip = render::PathBounds{0, 0, layout.boxWidth, layout.boxHeight};
    }

    // The coverage raster identity: everything that changes a single covered byte. Node identity,
    // plan indexes and the revision are deliberately absent so a reordered or renamed plan reuses
    // the same coverage; the resolved content, size, layout, font, matrix and geometry are all
    // here, so an animated or driven text invalidates exactly when its pixels move.
    OperationKey geometryKey;
    geometryKey.add(std::string{"gpu-text-coverage-v1"});
    addTextFontToKey(geometryKey, text.font);
    geometryKey.add(content->value);
    geometryKey.add(size->value);
    geometryKey.add(static_cast<std::int64_t>(layout.alignment));
    geometryKey.add(layout.lineHeight);
    geometryKey.add(layout.letterSpacing);
    geometryKey.add(layout.boxWidth);
    geometryKey.add(layout.boxHeight);
    geometryKey.add(layout.wrap);
    geometryKey.add(text.layout.verticalAlignment);
    geometryKey.add(text.layout.anchorMode);
    geometryKey.add(text.layout.overflow);
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

    const auto windowWidth = layerWindow.extent().width();
    const auto windowHeight = layerWindow.extent().height();
    std::shared_ptr<const render::PathRasterCoverageGeometry> geometry;
    if (coverageCache != nullptr) {
        geometry = coverageCache->find(geometryKeyDigest);
    }
    if (geometry == nullptr) {
        const auto matrixPath =
            render::PathMatrix{matrix.a, matrix.b, matrix.c, matrix.d, matrix.x, matrix.y};
        auto raster =
            render::PathRaster::transformed(std::span<const render::Path>(*outlines.value()), {},
                                            matrixPath, hScale, vScale, cancel, clip);
        if (!raster) {
            if (cancellation.isCancellationRequested()) {
                return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                            "Text coverage rasterization was cancelled");
            }
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Text coverage geometry is invalid");
        }
        auto built = raster.value()->coverageGeometry(layerWindow.originX(), layerWindow.originY(),
                                                      windowWidth, windowHeight,
                                                      render::PathFillRule::NonZero, false, cancel);
        if (!built) {
            if (cancellation.isCancellationRequested()) {
                return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                            "Text coverage rasterization was cancelled");
            }
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Text coverage geometry is invalid");
        }
        geometry =
            std::make_shared<const render::PathRasterCoverageGeometry>(std::move(*built.value()));
        if (coverageCache != nullptr) {
            coverageCache->store(geometryKeyDigest, geometry);
        }
    }

    if (const auto error = chargeBytes(windowWidth, windowHeight, sizeof(render::Rgba32f))) {
        return error;
    }
    command.pixel = *pixel.value();
    command.opacity = static_cast<float>(opacity);
    command.geometry = geometry;
    command.coverage = nullptr;
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
    pixelKey.add(std::string{kGpuCoveredSolidSpirvSha256});
    command.semanticKey = pixelKey.digest();
    return std::nullopt;
}

std::optional<GpuSceneLeafFailure> buildTextLeafCoverageLeaf(
    const CompiledText& text, const CompiledCompositionPlan& plan,
    const ResolvedEvaluation& resolved, const render::ImageWindow layerWindow,
    const render::ImageWindow fullDisplayWindow, const core::PixelAspectRatio fullPixelAspect,
    const double hScale, const double vScale, const double opacity,
    const std::uint64_t coverageByteLimit,
    const std::shared_ptr<GpuSceneCoverageCache>& coverageCache,
    const GpuSceneChargeBytes& chargeBytes, const CancellationToken& cancellation,
    GpuSceneCoverageSolidCommand& command) {
    static_cast<void>(opacity);
    static_cast<void>(layerWindow);
    if (text.layout.box.x > 0.0 && text.layout.box.y > 0.0) {
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedTransform,
                    "An integer-grid text box is not prepared");
    }
    const auto size = resolveParameter(text.size, plan, resolved);
    const auto color = resolveParameter(text.color, plan, resolved);
    const auto content =
        resolveParameter(text.contentParameterId, text.content, text.drivenContent, resolved);
    const auto lineHeight = resolveParameter(text.layout.lineHeight, plan, resolved);
    const auto spacing = resolveParameter(text.layout.letterSpacing, plan, resolved);
    const auto alignment = resolveParameter(text.layout.alignmentId, text.layout.alignment,
                                            text.layout.drivenAlignment, resolved);
    if (!size || !color || !content || !lineHeight || !spacing || !alignment) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Text parameters are not evaluable");
    }
    const auto pixel = render::solidPixelFromStraightLinearRec709Scene(color->value);
    if (!pixel) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Text colour is not evaluable");
    }
    const auto parameters =
        render::TextRasterParameters::create(size->value * hScale, size->value * vScale);
    if (!parameters) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Text size is not rasterizable");
    }
    render::TextLayoutOptions layout;
    layout.alignment = static_cast<render::TextAlignment>(alignment->value);
    layout.lineHeight = lineHeight->value;
    layout.letterSpacing = spacing->value * hScale;
    layout.multiline = true;
    layout.wrap = text.layout.wrap;
    layout.verticalAlignment =
        static_cast<render::TextLayoutOptions::VerticalAlignment>(text.layout.verticalAlignment);
    layout.anchorMode = static_cast<render::TextLayoutOptions::AnchorMode>(text.layout.anchorMode);
    layout.overflow = static_cast<render::TextLayoutOptions::Overflow>(text.layout.overflow);
    const auto byteLimit =
        coverageByteLimit > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(coverageByteLimit);
    auto bitmap = render::TextCoverageBitmap::rasterizeText(text.font, content->value,
                                                            *parameters.value(), byteLimit, layout);
    if (!bitmap) {
        if (cancellation.isCancellationRequested()) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                        "Text rasterization was cancelled");
        }
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Text content could not be rasterized");
    }
    if (!bitmap.value()->hasCoverage()) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "The integer-grid text leaf has no coverage");
    }
    const auto window =
        render::ImageWindow::create(bitmap.value()->originX(), bitmap.value()->originY(),
                                    static_cast<std::uint64_t>(bitmap.value()->width()),
                                    static_cast<std::uint64_t>(bitmap.value()->height()));
    if (!window) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Text bounds are invalid");
    }
    const auto coverageBytes = bitmap.value()->coverage();
    OperationKey geometryKey;
    geometryKey.add(std::string{"gpu-text-leaf-coverage-v1"});
    addTextFontToKey(geometryKey, text.font);
    geometryKey.add(content->value);
    geometryKey.add(size->value);
    geometryKey.add(static_cast<std::int64_t>(layout.alignment));
    geometryKey.add(layout.lineHeight);
    geometryKey.add(layout.letterSpacing);
    geometryKey.add(hScale);
    geometryKey.add(vScale);
    addWindowToKey(geometryKey, *window.value());
    addPixelAspectToKey(geometryKey, fullPixelAspect);
    const auto geometryKeyDigest = geometryKey.digest();

    // The integer-grid text leaf is the CPU text source's own FreeType glyph rasterization:
    // host font preparation, not a PathRaster vector-coverage mask. It stays a host 8-bit bitmap
    // translated by an exact integer and does not consume the GPU vector-coverage producer.
    static_cast<void>(coverageCache);
    auto coverage = std::make_shared<const std::vector<std::uint8_t>>(coverageBytes.begin(),
                                                                      coverageBytes.end());
    if (const auto error =
            chargeBytes(window.value()->extent().width(), window.value()->extent().height(),
                        sizeof(render::Rgba32f))) {
        return error;
    }
    command.pixel = *pixel.value();
    command.opacity = 1.0F;
    command.geometry = nullptr;
    command.coverage = coverage;
    command.outputWindow = *window.value();
    command.displayWindow = fullDisplayWindow;
    command.pixelAspect = fullPixelAspect;
    command.geometryKey = geometryKeyDigest;
    OperationKey pixelKey;
    pixelKey.add(geometryKeyDigest);
    pixelKey.add(std::bit_cast<std::uint32_t>(pixel.value()->red()));
    pixelKey.add(std::bit_cast<std::uint32_t>(pixel.value()->green()));
    pixelKey.add(std::bit_cast<std::uint32_t>(pixel.value()->blue()));
    pixelKey.add(std::bit_cast<std::uint32_t>(pixel.value()->alpha()));
    pixelKey.add(std::bit_cast<std::uint32_t>(1.0F));
    pixelKey.add(std::string{kGpuCoveredSolidSpirvSha256});
    command.semanticKey = pixelKey.digest();
    return std::nullopt;
}

} // namespace bloom::runtime::detail
