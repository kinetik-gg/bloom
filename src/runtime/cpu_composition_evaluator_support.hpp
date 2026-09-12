#pragma once

#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <bloom/render/image_types.hpp>
#include <bloom/render/text_raster.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime::detail {

template <typename... Functions> struct Overloaded final : Functions... {
    using Functions::operator()...;
};
template <typename... Functions> Overloaded(Functions...) -> Overloaded<Functions...>;

template <typename Value> struct ResolvedCurveSample final {
    Value value;
    document::KeyframeId segmentStart;
};

struct ResolvedEvaluation final {
    render::Rgba32fImageDescriptor imageDescriptor;
    double horizontalScale = 1.0;
    double verticalScale = 1.0;
    std::size_t imageBytes = 0;
    std::vector<std::size_t> remainingConsumers;
    std::vector<ResolvedCurveSample<double>> scalarCurveValues;
    std::vector<ResolvedCurveSample<document::Vec2d>> vec2CurveValues;
    std::vector<ResolvedCurveSample<core::Color4d>> color4CurveValues;
};

struct PreflightOutcome final {
    std::optional<ResolvedEvaluation> resolved;
    std::optional<EvaluationDiagnostic> diagnostic;
    bool cancelled = false;

    [[nodiscard]] static PreflightOutcome success(ResolvedEvaluation value) {
        return PreflightOutcome{
            .resolved = std::move(value), .diagnostic = std::nullopt, .cancelled = false};
    }
    [[nodiscard]] static PreflightOutcome failure(EvaluationDiagnostic value) {
        return PreflightOutcome{
            .resolved = std::nullopt, .diagnostic = std::move(value), .cancelled = false};
    }
    [[nodiscard]] static PreflightOutcome cancellation() {
        return PreflightOutcome{
            .resolved = std::nullopt, .diagnostic = std::nullopt, .cancelled = true};
    }
};

[[nodiscard]] EvaluationDiagnostic diagnostic(EvaluationDiagnosticCode code, std::string summary,
                                              std::string detail = {},
                                              EvaluationSubject subject = {});
[[nodiscard]] EvaluationSubject subjectFor(OperationIndex index,
                                           const CompiledOperation& operation);
void reportProgress(const EvaluationProgressCallback& callback,
                    const EvaluationProgress& progress) noexcept;

template <typename Function>
void forEachInput(const CompiledOperation& operation, Function&& function) {
    std::visit(Overloaded{
                   [](const CompiledSolid&) {},
                   [](const CompiledText&) {},
                   [&function](const CompiledLayerOutput& layer) { function(layer.input); },
                   [&function](const CompiledLayerStack& stack) {
                       for (const auto& entry : stack.entries) {
                           function(entry.input);
                       }
                   },
                   [&function](const CompiledCompositionOutput& output) { function(output.input); },
               },
               operation);
}

// One output row's worth of glyph coverage, already clipped to the frame: `coverage` is the part of
// the bitmap row that lands inside `window` (empty when none of it does), and `outputOffset` is the
// column in the row span where it starts. Separating the clipping arithmetic from the compositing
// keeps the evaluator's text arm readable and makes the off-frame cases -- text scrolled left of
// the frame, above it, wider than it -- one testable rule instead of four inline branches.
struct ClippedCoverageRow final {
    std::span<const std::uint8_t> coverage;
    std::size_t outputOffset = 0;
};

[[nodiscard]] inline ClippedCoverageRow clipCoverageRow(const render::TextCoverageBitmap& bitmap,
                                                        const render::ImageWindow window,
                                                        const std::int64_t outputY) noexcept {
    if (!bitmap.hasCoverage()) {
        return {};
    }
    // The text origin is the window's own origin, so a bitmap coordinate is a window coordinate
    // plus the bitmap's origin offset.
    const auto bitmapRow = outputY - window.originY() - bitmap.originY();
    if (bitmapRow < 0 || bitmapRow >= static_cast<std::int64_t>(bitmap.height())) {
        return {};
    }
    const auto row = bitmap.row(static_cast<std::uint32_t>(bitmapRow));
    if (row.empty()) {
        return {};
    }
    const auto width = static_cast<std::int64_t>(window.extent().width());
    const auto firstColumn = std::max<std::int64_t>(bitmap.originX(), 0);
    const auto lastColumnExclusive =
        std::min<std::int64_t>(bitmap.originX() + static_cast<std::int64_t>(bitmap.width()), width);
    if (lastColumnExclusive <= firstColumn) {
        return {};
    }
    const auto skipped = static_cast<std::size_t>(firstColumn - bitmap.originX());
    const auto count = static_cast<std::size_t>(lastColumnExclusive - firstColumn);
    return {row.subspan(skipped, count), static_cast<std::size_t>(firstColumn)};
}

[[nodiscard]] EvaluationDiagnostic imageDiagnostic(const render::ImageError& error,
                                                   EvaluationSubject subject, std::string summary);
[[nodiscard]] inline bool checkedAdd(const std::size_t lhs, const std::size_t rhs,
                                     std::size_t& result) noexcept {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

[[nodiscard]] inline std::optional<std::uint64_t>
checkedProduct(const std::span<const std::uint64_t> factors) noexcept {
    std::uint64_t result = 1;
    for (const auto factor : factors) {
        if (factor != 0 && result > std::numeric_limits<std::uint64_t>::max() / factor) {
            return std::nullopt;
        }
        result *= factor;
    }
    return result;
}
[[nodiscard]] std::optional<core::PixelAspectRatio>
proxyPixelAspect(const document::CompositionFormat& format,
                 render::ImageExtent proxyExtent) noexcept;
[[nodiscard]] bool isImageProducing(const CompiledOperation& operation) noexcept;
[[nodiscard]] bool hasExpectedInputKinds(const CompiledCompositionPlan& plan, std::size_t index,
                                         EvaluationDiagnostic& failure);
[[nodiscard]] PreflightOutcome preflight(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                                         const EvaluationRequest& request,
                                         const CancellationToken& cancellation,
                                         const EvaluationProgressCallback& progress);
[[nodiscard]] EvaluationResult unexpectedAllocationFailure();

} // namespace bloom::runtime::detail
