#pragma once

#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <bloom/render/image_types.hpp>

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
    render::ReferenceDisplayBufferDescriptor displayDescriptor;
    double horizontalScale = 1.0;
    double verticalScale = 1.0;
    std::size_t imageBytes = 0;
    std::size_t displayBytes = 0;
    std::vector<std::size_t> remainingConsumers;
    std::vector<ResolvedCurveSample<double>> scalarCurveValues;
    std::vector<ResolvedCurveSample<document::Vec2d>> vec2CurveValues;
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
