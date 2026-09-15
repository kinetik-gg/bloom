#include "cpu_composition_evaluator_support.hpp"
#include "image_source.hpp"
#include "operation_key.hpp"

#include <bloom/core/rational_time.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/text_raster.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bloom::runtime {
namespace detail {

// The text size schema bound and the rasterizer's own bound must be the same number: a document the
// schema accepts has to be a document the reference rasterizer can draw. src/runtime is the only
// module that sees both headers, so this is where the two are held equal -- at compile time, not in
// a test that could be deleted. See bloom/document/parameter.hpp and bloom/render/text_raster.hpp.
static_assert(document::kMaximumTextSizePixels == render::kMaximumTextPixelSize,
              "the text size schema bound and the text rasterizer bound must agree");

[[nodiscard]] EvaluationDiagnostic diagnostic(const EvaluationDiagnosticCode code,
                                              std::string summary, std::string detail,
                                              EvaluationSubject subject) {
    return {.code = code,
            .severity = DiagnosticSeverity::Error,
            .subject = std::move(subject),
            .summary = std::move(summary),
            .detail = std::move(detail)};
}

[[nodiscard]] EvaluationSubject subjectFor(const OperationIndex index,
                                           const CompiledOperation& operation) {
    EvaluationSubject subject{.operation = index,
                              .nodeId = std::nullopt,
                              .layerId = std::nullopt,
                              .parameterId = std::nullopt,
                              .animationCurveId = std::nullopt,
                              .keyframeId = std::nullopt,
                              .field = {}};
    std::visit(
        Overloaded{
            [&subject](const CompiledSolid& solid) { subject.nodeId = solid.sourceNodeId; },
            [&subject](const CompiledImageSource& image) { subject.nodeId = image.sourceNodeId; },
            [&subject](const CompiledText& text) { subject.nodeId = text.sourceNodeId; },
            [&subject](const CompiledLayerOutput& layer) {
                subject.nodeId = layer.sourceNodeId;
                subject.layerId = layer.layerId;
            },
            [&subject](const CompiledMerge& stack) { subject.nodeId = stack.sourceNodeId; },
            [&subject](const CompiledCompositionOutput& output) {
                subject.nodeId = output.sourceNodeId;
            },
        },
        operation);
    return subject;
}

// What one banded row returns: nothing, or the structured failure that ends the pass. Named because
// every row lambda below spells it as its own return type.
using RowFailure = std::optional<EvaluationDiagnostic>;

// The two Operation-stage progress events a BANDED row pass reports, both from the thread that owns
// the frame. Rows are no longer reported one at a time: a band runs on another thread, and the
// progress callback belongs to the task that owns the frame rather than to the pool. `completed ==
// 0` is the event an observer uses to rendezvous with the start of an operation's pixel work, and
// `completed == total` the one that says the pass finished.
void reportRowPassStarted(const EvaluationProgressCallback& callback,
                          const OperationIndex operation, const std::uint64_t total) noexcept;
void reportRowPassFinished(const EvaluationProgressCallback& callback,
                           const OperationIndex operation, const std::uint64_t total) noexcept;
[[nodiscard]] RowFailure rowPassFailure(const RowBandPassOutcome<RowFailure>& outcome,
                                        const EvaluationSubject& subject);

void reportProgress(const EvaluationProgressCallback& callback,
                    const EvaluationProgress& progress) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(progress);
    } catch (...) {
        // Monitoring is best effort. A presentation-side allocation failure must not change pixels.
        return;
    }
}

// One banded row pass's failure in the evaluator's own vocabulary. `incomplete` means a band
// escaped by throwing -- unreachable for a noexcept row kernel, and reported as the allocation
// failure it can only have been rather than published as a half-written image.
RowFailure rowPassFailure(const RowBandPassOutcome<RowFailure>& outcome,
                          const EvaluationSubject& subject) {
    if (outcome.failure.has_value()) {
        return outcome.failure;
    }
    return diagnostic(
        EvaluationDiagnosticCode::AllocationFailure, "An evaluation row band could not complete",
        "A parallel row band ended in an exception; the frame was not published.", subject);
}

void reportRowPassStarted(const EvaluationProgressCallback& callback,
                          const OperationIndex operation, const std::uint64_t total) noexcept {
    reportProgress(callback, {.stage = EvaluationProgressStage::Operation,
                              .operation = operation,
                              .completed = 0,
                              .total = total});
}

void reportRowPassFinished(const EvaluationProgressCallback& callback,
                           const OperationIndex operation, const std::uint64_t total) noexcept {
    reportProgress(callback, {.stage = EvaluationProgressStage::Operation,
                              .operation = operation,
                              .completed = total,
                              .total = total});
}

[[nodiscard]] std::optional<core::PixelAspectRatio>
proxyPixelAspect(const document::CompositionFormat& format,
                 const render::ImageExtent proxyExtent) noexcept {
    std::array<std::uint64_t, 3> numerator{format.pixelAspect().numerator(), format.width(),
                                           proxyExtent.height()};
    std::array<std::uint64_t, 3> denominator{format.pixelAspect().denominator(), format.height(),
                                             proxyExtent.width()};
    for (auto& numeratorFactor : numerator) {
        for (auto& denominatorFactor : denominator) {
            const auto divisor = std::gcd(numeratorFactor, denominatorFactor);
            numeratorFactor /= divisor;
            denominatorFactor /= divisor;
        }
    }

    const auto reducedNumerator = checkedProduct(numerator);
    const auto reducedDenominator = checkedProduct(denominator);
    if (!reducedNumerator.has_value() || !reducedDenominator.has_value()) {
        return std::nullopt;
    }
    return core::PixelAspectRatio::create(*reducedNumerator, *reducedDenominator);
}

[[nodiscard]] EvaluationDiagnostic imageDiagnostic(const render::ImageError& error,
                                                   EvaluationSubject subject, std::string summary) {
    EvaluationDiagnosticCode code = EvaluationDiagnosticCode::InternalInvariant;
    switch (error.code) {
    case render::ImageErrorCode::ArithmeticOverflow:
        code = EvaluationDiagnosticCode::ArithmeticOverflow;
        break;
    case render::ImageErrorCode::PixelStorageBudgetExceeded:
        code = EvaluationDiagnosticCode::PixelStorageBudgetExceeded;
        break;
    case render::ImageErrorCode::AllocationFailure:
        code = EvaluationDiagnosticCode::AllocationFailure;
        break;
    case render::ImageErrorCode::InvalidPixel:
    case render::ImageErrorCode::NonFiniteResult:
        code = EvaluationDiagnosticCode::InvalidPixel;
        break;
    case render::ImageErrorCode::InvalidParameter:
        code = EvaluationDiagnosticCode::InvalidParameter;
        break;
    case render::ImageErrorCode::UnsupportedFloatingPointEnvironment:
        code = EvaluationDiagnosticCode::UnsupportedFloatingPointEnvironment;
        break;
    case render::ImageErrorCode::IncompatibleImageDescriptor:
        code = EvaluationDiagnosticCode::IncompatibleImageDescriptor;
        break;
    case render::ImageErrorCode::InvalidExtent:
    case render::ImageErrorCode::InvalidWindow:
    case render::ImageErrorCode::InvalidStorageSize:
    case render::ImageErrorCode::CoordinateOutOfBounds:
    case render::ImageErrorCode::InvalidState:
        code = EvaluationDiagnosticCode::InternalInvariant;
        break;
    }

    std::string detail;
    if (error.requestedPixelStorageBytes.has_value()) {
        detail += "requestedBytes=" + std::to_string(*error.requestedPixelStorageBytes);
    }
    if (error.pixelStorageByteLimit.has_value()) {
        if (!detail.empty()) {
            detail += ' ';
        }
        detail += "byteLimit=" + std::to_string(*error.pixelStorageByteLimit);
    }
    return diagnostic(code, std::move(summary), std::move(detail), std::move(subject));
}

template <typename Value>
[[nodiscard]] EvaluationDiagnostic animationDiagnostic(const AnimationSamplingError error,
                                                       const document::AnimationCurveId curveId,
                                                       const AnimationSampleResult<Value>& sample) {
    auto code = EvaluationDiagnosticCode::InvalidPlan;
    std::string summary = "Animation curve cannot be sampled";
    if (error == AnimationSamplingError::UnsupportedFloatingPointEnvironment) {
        code = EvaluationDiagnosticCode::UnsupportedFloatingPointEnvironment;
        summary = "Animation sampling environment is unsupported";
    } else if (error == AnimationSamplingError::NonFiniteResult) {
        code = EvaluationDiagnosticCode::InvalidParameter;
        summary = "Animation interpolation produced a non-finite value";
    }
    EvaluationSubject subject;
    subject.animationCurveId = curveId;
    subject.keyframeId = sample.segmentStart;
    subject.field = "animationCurve";
    return diagnostic(code, std::move(summary), {}, std::move(subject));
}

[[nodiscard]] bool isImageProducing(const CompiledOperation& operation) noexcept {
    return !std::holds_alternative<CompiledCompositionOutput>(operation);
}

// Which domain an authored scalar field declares. A compiled plan names parameters by identity, not
// by schema key, so the domain travels with the operand from its call site instead of being guessed
// from the value kind. The values mirror document::hasUnitDomainSchemaKey()/
// hasTextSizeDomainSchemaKey() exactly; keeping the plan side an enum rather than re-reading schema
// keys is what lets the evaluator validate a plan that arrived from a retained frame with no
// document in hand.
enum class ScalarDomain : std::uint8_t {
    Unbounded,
    Unit,
    TextSize,
    Dimension,
    Positive,
};

[[nodiscard]] bool withinScalarDomain(const ScalarDomain domain, const double value) noexcept {
    switch (domain) {
    case ScalarDomain::Unbounded:
        return true;
    case ScalarDomain::Unit:
        return value >= 0.0 && value <= 1.0;
    case ScalarDomain::Positive:
        return value > 0.0;
    case ScalarDomain::Dimension:
        return value >= 1.0;
    case ScalarDomain::TextSize:
        return value > 0.0 && value <= document::kMaximumTextSizePixels;
    }
    return false;
}

[[nodiscard]] const char* scalarDomainFailureSummary(const ScalarDomain domain) noexcept {
    switch (domain) {
    case ScalarDomain::Unbounded:
        return "Scalar parameter is not finite";
    case ScalarDomain::Unit:
        return "Layer opacity is outside its unit domain";
    case ScalarDomain::Positive:
        return "Value must be positive";
    case ScalarDomain::Dimension:
        return "Dimension must be at least one pixel";
    case ScalarDomain::TextSize:
        return "Text size is outside its domain";
    }
    return "Scalar parameter is outside its domain";
}

// The two in-range checks a curve-indexing operand needs, shared by every operation that carries
// one rather than repeated per call site (a Solid's colour, a Text's size and colour, and --
// through the Layer Output arm's own loops -- the five transform operands).
[[nodiscard]] bool hasValidScalarCurveReference(const CompiledScalarParameter& parameter,
                                                const CompiledCompositionPlan& plan,
                                                const std::size_t index,
                                                EvaluationDiagnostic& failure) {
    if (!parameter.id.isValid()) {
        failure = diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                             "Operation has an invalid parameter identity", {},
                             subjectFor(OperationIndex::fromRaw(index), plan.operations()[index]));
        return false;
    }
    const auto* curve = std::get_if<ScalarCurveIndex>(&parameter.source);
    if (curve != nullptr && curve->value() >= plan.scalarCurves().size()) {
        failure = diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                             "Scalar parameter references an invalid animation curve", {},
                             subjectFor(OperationIndex::fromRaw(index), plan.operations()[index]));
        failure.subject.parameterId = parameter.id;
        return false;
    }
    return true;
}

[[nodiscard]] bool hasValidColorCurveReference(const CompiledColorParameter& parameter,
                                               const CompiledCompositionPlan& plan,
                                               const std::size_t index,
                                               EvaluationDiagnostic& failure) {
    if (!parameter.id.isValid()) {
        failure = diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                             "Operation has an invalid parameter identity", {},
                             subjectFor(OperationIndex::fromRaw(index), plan.operations()[index]));
        return false;
    }
    const auto* curve = std::get_if<Color4CurveIndex>(&parameter.source);
    if (curve != nullptr && curve->value() >= plan.color4Curves().size()) {
        failure = diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                             "Color parameter references an invalid animation curve", {},
                             subjectFor(OperationIndex::fromRaw(index), plan.operations()[index]));
        failure.subject.parameterId = parameter.id;
        return false;
    }
    return true;
}

[[nodiscard]] bool hasExpectedInputKinds(const CompiledCompositionPlan& plan,
                                         const std::size_t index, EvaluationDiagnostic& failure) {
    const auto& operation = plan.operations()[index];
    return std::visit(
        Overloaded{
            [&plan, index, &failure](const CompiledSolid& solid) {
                return hasValidColorCurveReference(solid.color, plan, index, failure) &&
                       (!solid.width ||
                        hasValidScalarCurveReference(*solid.width, plan, index, failure)) &&
                       (!solid.height ||
                        hasValidScalarCurveReference(*solid.height, plan, index, failure));
            },
            [](const CompiledImageSource& image) {
                return image.loopMode >= 0 && image.loopMode <= 2 && image.colorSpace >= 0 &&
                       image.colorSpace <= 3 && (!image.asset || image.asset->validate().ok());
            },
            [&plan, index, &failure](const CompiledText& text) {
                return hasValidScalarCurveReference(text.size, plan, index, failure) &&
                       hasValidColorCurveReference(text.color, plan, index, failure) &&
                       (!text.layout || (hasValidScalarCurveReference(text.layout->lineHeight, plan,
                                                                      index, failure) &&
                                         hasValidScalarCurveReference(text.layout->letterSpacing,
                                                                      plan, index, failure)));
            },
            [&plan, index, &failure](const CompiledLayerOutput& layer) {
                const auto sourcesAnImage = [&plan, index, &layer] {
                    if (layer.input.value() >= index) {
                        return false;
                    }
                    const auto& input = plan.operations()[layer.input.value()];
                    return std::holds_alternative<CompiledSolid>(input) ||
                           std::holds_alternative<CompiledText>(input) ||
                           std::holds_alternative<CompiledImageSource>(input) ||
                           std::holds_alternative<CompiledLayerOutput>(input) ||
                           std::holds_alternative<CompiledMerge>(input);
                };
                if (!sourcesAnImage()) {
                    failure = diagnostic(
                        EvaluationDiagnosticCode::InvalidPlan,
                        "Layer Output has an invalid image input",
                        "The input must name an earlier image-producing operation.",
                        subjectFor(OperationIndex::fromRaw(index), plan.operations()[index]));
                    return false;
                }
                const std::array vec2Parameters{&layer.position, &layer.anchor, &layer.scale};
                const std::array scalarParameters{&layer.rotation, &layer.opacity};
                const auto identitiesValid = std::ranges::all_of(vec2Parameters,
                                                                 [](const auto* parameter) {
                                                                     return parameter->id.isValid();
                                                                 }) &&
                                             std::ranges::all_of(scalarParameters,
                                                                 [](const auto* parameter) {
                                                                     return parameter->id.isValid();
                                                                 }) &&
                                             layer.blendModeParameterId.isValid();
                if (!identitiesValid) {
                    failure = diagnostic(
                        EvaluationDiagnosticCode::InvalidPlan,
                        "Layer Output has an invalid parameter identity", {},
                        subjectFor(OperationIndex::fromRaw(index), plan.operations()[index]));
                    return false;
                }
                for (const auto* parameter : vec2Parameters) {
                    if (const auto* curve = std::get_if<Vec2CurveIndex>(&parameter->source);
                        curve != nullptr && curve->value() >= plan.vec2Curves().size()) {
                        failure = diagnostic(
                            EvaluationDiagnosticCode::InvalidPlan,
                            "Layer transform parameter references an invalid animation curve", {},
                            subjectFor(OperationIndex::fromRaw(index), plan.operations()[index]));
                        failure.subject.parameterId = parameter->id;
                        return false;
                    }
                }
                for (const auto* parameter : scalarParameters) {
                    if (const auto* curve = std::get_if<ScalarCurveIndex>(&parameter->source);
                        curve != nullptr && curve->value() >= plan.scalarCurves().size()) {
                        failure = diagnostic(
                            EvaluationDiagnosticCode::InvalidPlan,
                            "Layer scalar parameter references an invalid animation curve", {},
                            subjectFor(OperationIndex::fromRaw(index), plan.operations()[index]));
                        failure.subject.parameterId = parameter->id;
                        return false;
                    }
                }
                return true;
            },
            [&plan, index, &failure](const CompiledMerge& stack) {
                for (const auto& entry : stack.entries) {
                    if (entry.input.value() >= index) {
                        failure = diagnostic(
                            EvaluationDiagnosticCode::InvalidPlan,
                            "Layer Stack has a non-topological input", {},
                            subjectFor(OperationIndex::fromRaw(index), plan.operations()[index]));
                        return false;
                    }
                    const auto* layer =
                        std::get_if<CompiledLayerOutput>(&plan.operations()[entry.input.value()]);
                    if (entry.layerId.isValid() &&
                        (layer == nullptr || layer->layerId != entry.layerId)) {
                        failure = diagnostic(
                            EvaluationDiagnosticCode::InvalidPlan,
                            "Layer Stack entry does not match its layer output", {},
                            subjectFor(OperationIndex::fromRaw(index), plan.operations()[index]));
                        return false;
                    }
                }
                return true;
            },
            [&plan, index, &failure](const CompiledCompositionOutput& output) {
                if (output.input.value() >= index || !std::holds_alternative<CompiledMerge>(
                                                         plan.operations()[output.input.value()])) {
                    failure = diagnostic(
                        EvaluationDiagnosticCode::InvalidPlan,
                        "Composition Output has an invalid stack input", {},
                        subjectFor(OperationIndex::fromRaw(index), plan.operations()[index]));
                    return false;
                }
                return true;
            },
        },
        operation);
}

template <typename Curve>
[[nodiscard]] bool hasCanonicalCurveIds(const std::span<const Curve> curves) noexcept {
    return std::adjacent_find(curves.begin(), curves.end(),
                              [](const Curve& left, const Curve& right) {
                                  return !left.id.isValid() || !right.id.isValid() ||
                                         left.id >= right.id;
                              }) == curves.end() &&
           (curves.empty() || curves.front().id.isValid());
}

// Pairwise disjointness over two ALREADY-canonical (valid, strictly ascending) tables. Templated
// over the two curve types so the three pairs the plan now has -- scalar/vec2, scalar/color4,
// vec2/color4 -- share one merge rather than three copies of it.
template <typename LeftCurve, typename RightCurve>
[[nodiscard]] static bool
hasDisjointCurveIds(const std::span<const LeftCurve> leftCurves,
                    const std::span<const RightCurve> rightCurves) noexcept {
    auto left = leftCurves.begin();
    auto right = rightCurves.begin();
    while (left != leftCurves.end() && right != rightCurves.end()) {
        if (left->id == right->id) {
            return false;
        }
        if (left->id < right->id) {
            ++left;
        } else {
            ++right;
        }
    }
    return true;
}

// One driven parameter's value, read out of the frame's already-evaluated value graph. A value of
// the wrong alternative cannot be substituted here -- there is no defensible fallback for "this
// parameter wanted a colour and the graph produced a string" -- so it is reported as an invalid
// plan, which is what a compiler that honoured the document's typing could never produce.
template <typename Value>
[[nodiscard]] static const Value* resolvedValue(const ValueOutputIndex output,
                                                const ResolvedEvaluation& resolved) noexcept {
    const auto index = output.value();
    if (index >= resolved.valueOutputs.size()) {
        return nullptr;
    }
    return std::get_if<Value>(&resolved.valueOutputs[index]);
}

template <typename Value> struct ResolvedParameter final {
    Value value;
    document::ParameterId parameterId;
    std::optional<document::AnimationCurveId> animationCurveId;
    std::optional<document::KeyframeId> keyframeId;
};

[[nodiscard]] static std::optional<ResolvedParameter<document::Vec2d>>
resolveParameter(const CompiledVec2Parameter& parameter, const CompiledCompositionPlan& plan,
                 const ResolvedEvaluation& resolved) noexcept {
    if (const auto* constant = std::get_if<document::Vec2d>(&parameter.source)) {
        return ResolvedParameter<document::Vec2d>{*constant, parameter.id, std::nullopt,
                                                  std::nullopt};
    }
    if (const auto* driven = std::get_if<ValueOutputIndex>(&parameter.source)) {
        const auto* value = resolvedValue<document::Vec2d>(*driven, resolved);
        return value == nullptr ? std::nullopt
                                : std::optional(ResolvedParameter<document::Vec2d>{
                                      *value, parameter.id, std::nullopt, std::nullopt});
    }
    const auto* curve = std::get_if<Vec2CurveIndex>(&parameter.source);
    if (curve == nullptr) {
        return std::nullopt;
    }
    const auto index = curve->value();
    if (index >= resolved.vec2CurveValues.size() || index >= plan.vec2Curves().size()) {
        return std::nullopt;
    }
    const auto& sample = resolved.vec2CurveValues[index];
    return ResolvedParameter<document::Vec2d>{sample.value, parameter.id,
                                              plan.vec2Curves()[index].id, sample.segmentStart};
}

[[nodiscard]] static std::optional<ResolvedParameter<core::Color4d>>
resolveParameter(const CompiledColorParameter& parameter, const CompiledCompositionPlan& plan,
                 const ResolvedEvaluation& resolved) noexcept {
    if (const auto* constant = std::get_if<core::Color4d>(&parameter.source)) {
        return ResolvedParameter<core::Color4d>{*constant, parameter.id, std::nullopt,
                                                std::nullopt};
    }
    if (const auto* driven = std::get_if<ValueOutputIndex>(&parameter.source)) {
        const auto* value = resolvedValue<core::Color4d>(*driven, resolved);
        return value == nullptr ? std::nullopt
                                : std::optional(ResolvedParameter<core::Color4d>{
                                      *value, parameter.id, std::nullopt, std::nullopt});
    }
    const auto* curve = std::get_if<Color4CurveIndex>(&parameter.source);
    if (curve == nullptr) {
        return std::nullopt;
    }
    const auto index = curve->value();
    if (index >= resolved.color4CurveValues.size() || index >= plan.color4Curves().size()) {
        return std::nullopt;
    }
    const auto& sample = resolved.color4CurveValues[index];
    return ResolvedParameter<core::Color4d>{sample.value, parameter.id,
                                            plan.color4Curves()[index].id, sample.segmentStart};
}

[[nodiscard]] static std::optional<ResolvedParameter<double>>
resolveParameter(const CompiledScalarParameter& parameter, const CompiledCompositionPlan& plan,
                 const ResolvedEvaluation& resolved) noexcept {
    if (const auto* constant = std::get_if<double>(&parameter.source)) {
        return ResolvedParameter<double>{*constant, parameter.id, std::nullopt, std::nullopt};
    }
    if (const auto* driven = std::get_if<ValueOutputIndex>(&parameter.source)) {
        const auto* value = resolvedValue<double>(*driven, resolved);
        return value == nullptr ? std::nullopt
                                : std::optional(ResolvedParameter<double>{
                                      *value, parameter.id, std::nullopt, std::nullopt});
    }
    const auto* curve = std::get_if<ScalarCurveIndex>(&parameter.source);
    if (curve == nullptr) {
        return std::nullopt;
    }
    const auto index = curve->value();
    if (index >= resolved.scalarCurveValues.size() || index >= plan.scalarCurves().size()) {
        return std::nullopt;
    }
    const auto& sample = resolved.scalarCurveValues[index];
    return ResolvedParameter<double>{sample.value, parameter.id, plan.scalarCurves()[index].id,
                                     sample.segmentStart};
}

// Task DRIVE-1. The fourth member of this family, for the parameter kinds that cannot interpolate
// -- a String, an Integer (every enum-backed one included) and a Boolean. They have no curve table
// to index, so unlike their three animatable siblings they answer only the two questions that are
// left: the authored constant the compiler resolved, or the value-graph output a driver binding
// hands them this frame. A driven output of the wrong alternative answers nothing, for exactly the
// reason resolvedValue() gives -- there is no defensible substitute for "this parameter wanted a
// String and the graph produced a colour" -- so a malformed plan is diagnosed rather than trusted.
template <typename Value>
[[nodiscard]] static std::optional<ResolvedParameter<Value>>
resolveParameter(const document::ParameterId id, const Value& authored,
                 const std::optional<ValueOutputIndex>& driven,
                 const ResolvedEvaluation& resolved) noexcept {
    if (!driven.has_value()) {
        return ResolvedParameter<Value>{authored, id, std::nullopt, std::nullopt};
    }
    const auto* value = resolvedValue<Value>(*driven, resolved);
    return value == nullptr
               ? std::nullopt
               : std::optional(ResolvedParameter<Value>{*value, id, std::nullopt, std::nullopt});
}

// The blend mode one layer composites with at this frame. An authored mode and a driven one are the
// same closed set of modes, because both go through core::blendModeFromStoredValue(): a driven
// Integer naming no implemented mode answers nothing rather than silently compositing Normal. The
// document cannot store such an integer -- ParameterStore refuses it on insert and validation
// refuses it on publication -- so only a malformed plan can produce one.
[[nodiscard]] static std::optional<core::BlendMode>
resolveParameter(const CompiledLayerOutput& layer, const ResolvedEvaluation& resolved) noexcept {
    const auto stored =
        resolveParameter(layer.blendModeParameterId, core::blendModeStoredValue(layer.blendMode),
                         layer.drivenBlendMode, resolved);
    return stored.has_value() ? core::blendModeFromStoredValue(stored->value) : std::nullopt;
}

template <typename Value>
[[nodiscard]] EvaluationSubject parameterSubject(EvaluationSubject subject,
                                                 const ResolvedParameter<Value>& parameter,
                                                 std::string field) {
    subject.parameterId = parameter.parameterId;
    subject.animationCurveId = parameter.animationCurveId;
    subject.keyframeId = parameter.keyframeId;
    subject.field = std::move(field);
    return subject;
}

[[nodiscard]] PreflightOutcome preflight(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                                         const EvaluationRequest& request,
                                         const CancellationToken& cancellation,
                                         const EvaluationProgressCallback& progress,
                                         OperationCache* cache,
                                         OperationCacheStatistics* statistics) {
    if (plan == nullptr) {
        return PreflightOutcome::failure(diagnostic(EvaluationDiagnosticCode::InvalidRequest,
                                                    "Evaluation has no compiled plan"));
    }
    if (plan->planSemanticsVersion() != kCompiledCompositionPlanSemanticsVersion ||
        plan->animationSamplingSemanticsVersion() != kAnimationSamplingSemanticsVersion) {
        return PreflightOutcome::failure(diagnostic(
            EvaluationDiagnosticCode::InvalidPlan, "Compiled plan semantics are unsupported",
            "Recompile the document snapshot with the current runtime semantics."));
    }
    if (request.quality != EvaluationQuality::Reference ||
        request.colorIntent != EvaluationColorIntent::LinearRec709Scene) {
        return PreflightOutcome::failure(
            diagnostic(EvaluationDiagnosticCode::InvalidRequest,
                       "Evaluation request uses an unsupported intent"));
    }
    if (plan->operations().empty() || request.output.value() >= plan->operations().size() ||
        request.output != plan->output() ||
        request.output.value() + 1 != plan->operations().size() ||
        !std::holds_alternative<CompiledCompositionOutput>(
            plan->operations()[request.output.value()])) {
        return PreflightOutcome::failure(
            diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                       "Evaluation output is not the plan's terminal Composition Output"));
    }

    const auto extentResult =
        render::ImageExtent::create(plan->format().width(), plan->format().height());
    if (!extentResult) {
        return PreflightOutcome::failure(
            imageDiagnostic(*extentResult.error(), {}, "Evaluation extent is invalid"));
    }
    auto extent = *extentResult.value();
    core::PixelAspectRatio pixelAspect = plan->format().pixelAspect();
    double horizontalScale = 1.0;
    double verticalScale = 1.0;
    if (const auto* proxy = std::get_if<ProxyResolution>(&request.resolution)) {
        extent = proxy->extent;
        const auto derivedPixelAspect = proxyPixelAspect(plan->format(), extent);
        if (!derivedPixelAspect.has_value()) {
            return PreflightOutcome::failure(diagnostic(
                EvaluationDiagnosticCode::InvalidProxyPixelAspect,
                "Proxy pixel aspect cannot be represented exactly",
                "Choose a proxy extent whose reduced pixel aspect fits Bloom's ratio type."));
        }
        pixelAspect = *derivedPixelAspect;
        horizontalScale = static_cast<double>(extent.width()) / plan->format().width();
        verticalScale = static_cast<double>(extent.height()) / plan->format().height();
    }

    const auto windowResult = render::ImageWindow::create(0, 0, extent.width(), extent.height());
    if (!windowResult) {
        return PreflightOutcome::failure(
            imageDiagnostic(*windowResult.error(), {}, "Evaluation window is invalid"));
    }
    const auto window = *windowResult.value();
    const auto descriptorResult =
        render::Rgba32fImageDescriptor::create(window, window, pixelAspect);
    if (!descriptorResult) {
        return PreflightOutcome::failure(
            imageDiagnostic(*descriptorResult.error(), {}, "Process image descriptor is invalid"));
    }
    const auto operationCount = plan->operations().size();
    std::vector<bool> reachable(operationCount, false);
    std::vector<std::size_t> pending{request.output.value()};
    while (!pending.empty()) {
        if (cancellation.isCancellationRequested()) {
            return PreflightOutcome::cancellation();
        }
        const auto index = pending.back();
        pending.pop_back();
        if (index >= operationCount) {
            return PreflightOutcome::failure(
                diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                           "Compiled plan references an invalid operation"));
        }
        if (reachable[index]) {
            continue;
        }
        reachable[index] = true;
        forEachInput(plan->operations()[index],
                     [&pending](const OperationIndex input) { pending.push_back(input.value()); });
    }

    std::vector<std::size_t> consumers(operationCount, 0);
    for (std::size_t index = 0; index < operationCount; ++index) {
        if (cancellation.isCancellationRequested()) {
            return PreflightOutcome::cancellation();
        }
        reportProgress(progress, {.stage = EvaluationProgressStage::Preflight,
                                  .operation = OperationIndex::fromRaw(index),
                                  .completed = index,
                                  .total = operationCount});
        if (!reachable[index]) {
            return PreflightOutcome::failure(
                diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                           "Compiled plan contains an unreachable operation", {},
                           subjectFor(OperationIndex::fromRaw(index), plan->operations()[index])));
        }
        EvaluationDiagnostic invalidOperation;
        if (!hasExpectedInputKinds(*plan, index, invalidOperation)) {
            return PreflightOutcome::failure(std::move(invalidOperation));
        }
        bool overflow = false;
        forEachInput(plan->operations()[index],
                     [&consumers, &overflow](const OperationIndex input) {
                         auto& count = consumers[input.value()];
                         if (count == std::numeric_limits<std::size_t>::max()) {
                             overflow = true;
                         } else {
                             ++count;
                         }
                     });
        if (overflow) {
            return PreflightOutcome::failure(diagnostic(
                EvaluationDiagnosticCode::ArithmeticOverflow, "Operation consumer count overflowed",
                {}, subjectFor(OperationIndex::fromRaw(index), plan->operations()[index])));
        }
        reportProgress(progress, {.stage = EvaluationProgressStage::Preflight,
                                  .operation = OperationIndex::fromRaw(index),
                                  .completed = index + 1,
                                  .total = operationCount});
    }

    if (!hasCanonicalCurveIds(plan->scalarCurves()) || !hasCanonicalCurveIds(plan->vec2Curves()) ||
        !hasCanonicalCurveIds(plan->color4Curves()) || !hasCanonicalCurveIds(plan->vec3Curves()) ||
        !hasDisjointCurveIds(plan->scalarCurves(), plan->vec2Curves()) ||
        !hasDisjointCurveIds(plan->scalarCurves(), plan->color4Curves()) ||
        !hasDisjointCurveIds(plan->scalarCurves(), plan->vec3Curves()) ||
        !hasDisjointCurveIds(plan->vec2Curves(), plan->color4Curves()) ||
        !hasDisjointCurveIds(plan->vec2Curves(), plan->vec3Curves()) ||
        !hasDisjointCurveIds(plan->vec3Curves(), plan->color4Curves())) {
        return PreflightOutcome::failure(diagnostic(
            EvaluationDiagnosticCode::InvalidPlan, "Animation curve tables are not canonical",
            "Curve identities must be valid, globally unique, and strictly ordered."));
    }

    std::vector<std::uint8_t> scalarCurveReferences(plan->scalarCurves().size(), 0);
    std::vector<std::uint8_t> vec2CurveReferences(plan->vec2Curves().size(), 0);
    std::vector<std::uint8_t> color4CurveReferences(plan->color4Curves().size(), 0);
    std::vector<std::uint8_t> vec3CurveReferences(plan->vec3Curves().size(), 0);
    std::vector<document::ParameterId> scalarCurveOwners(plan->scalarCurves().size());
    std::vector<document::ParameterId> vec2CurveOwners(plan->vec2Curves().size());
    std::vector<document::ParameterId> color4CurveOwners(plan->color4Curves().size());
    // Which authored field each curve drives, and -- for scalars -- which DOMAIN that field
    // declares. A compiled plan names parameters by identity rather than by schema key, so the
    // domain a key must satisfy travels with the curve rather than being guessed from the value
    // kind: opacity is confined to [0, 1], text size to (0, kMaximumTextSizePixels], and a rotation
    // key is any finite number of degrees.
    std::vector<std::string_view> scalarCurveFields(plan->scalarCurves().size());
    std::vector<ScalarDomain> scalarCurveDomains(plan->scalarCurves().size(),
                                                 ScalarDomain::Unbounded);
    std::vector<std::string_view> vec2CurveFields(plan->vec2Curves().size());
    std::vector<std::string_view> color4CurveFields(plan->color4Curves().size());
    std::unordered_set<document::ParameterId> parameterIds;
    std::optional<EvaluationDiagnostic> parameterFailure;
    const auto registerParameter = [&](const document::ParameterId parameterId,
                                       const EvaluationSubject& operationSubject) {
        if (!parameterId.isValid() || !parameterIds.insert(parameterId).second) {
            auto subject = operationSubject;
            subject.parameterId = parameterId;
            subject.field = "parameter";
            parameterFailure = diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                                          "Compiled parameter identity is not canonical",
                                          "Parameter identities must be valid and globally unique.",
                                          std::move(subject));
            return false;
        }
        return true;
    };
    // Registering one typed operand: claim its parameter identity, then either validate its
    // constant against the schema domain the CALL SITE names or claim sole ownership of the curve
    // it indexes and record that domain for the per-key pass below. Hoisted out of the Layer Output
    // arm (where it used to live as two local lambdas) because task S5 gave a Solid a colour
    // operand and a Text a size and colour operand: the claim rules must be one copy, not one per
    // operation kind.
    const auto registerScalar = [&](const CompiledScalarParameter& parameter,
                                    const std::string_view field, const ScalarDomain domain,
                                    const EvaluationSubject& operationSubject) {
        if (!registerParameter(parameter.id, operationSubject)) {
            return false;
        }
        if (const auto* constant = std::get_if<double>(&parameter.source)) {
            if (!std::isfinite(*constant) || !withinScalarDomain(domain, *constant)) {
                auto subject = operationSubject;
                subject.parameterId = parameter.id;
                subject.field = std::string(field);
                parameterFailure =
                    diagnostic(EvaluationDiagnosticCode::InvalidParameter,
                               scalarDomainFailureSummary(domain), {}, std::move(subject));
                return false;
            }
            return true;
        }
        // A driven parameter claims no curve: its value comes from the value graph, which owns its
        // own bounds checking. Returning here rather than reaching the std::get below is what keeps
        // a value-graph arm from being read as a curve index.
        if (const auto* driven = std::get_if<ValueOutputIndex>(&parameter.source)) {
            if (driven->value() >= plan->valueOutputCount()) {
                auto subject = operationSubject;
                subject.parameterId = parameter.id;
                subject.field = std::string(field);
                parameterFailure = diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                                              "Parameter references an invalid value-graph output",
                                              {}, std::move(subject));
                return false;
            }
            return true;
        }
        const auto curveIndex = std::get<ScalarCurveIndex>(parameter.source).value();
        auto& references = scalarCurveReferences[curveIndex];
        if (references != 0) {
            auto subject = operationSubject;
            subject.parameterId = parameter.id;
            subject.animationCurveId = plan->scalarCurves()[curveIndex].id;
            subject.field = std::string(field);
            parameterFailure =
                diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                           "Animation curve has multiple parameter owners", {}, std::move(subject));
            return false;
        }
        references = 1;
        scalarCurveOwners[curveIndex] = parameter.id;
        scalarCurveFields[curveIndex] = field;
        scalarCurveDomains[curveIndex] = domain;
        return true;
    };
    const auto registerVec2 = [&](const CompiledVec2Parameter& parameter,
                                  const std::string_view field,
                                  const EvaluationSubject& operationSubject) {
        if (!registerParameter(parameter.id, operationSubject)) {
            return false;
        }
        if (const auto* constant = std::get_if<document::Vec2d>(&parameter.source)) {
            if (!std::isfinite(constant->x) || !std::isfinite(constant->y)) {
                auto subject = operationSubject;
                subject.parameterId = parameter.id;
                subject.field = std::string(field);
                parameterFailure =
                    diagnostic(EvaluationDiagnosticCode::InvalidParameter,
                               "Layer transform value is not finite", {}, std::move(subject));
                return false;
            }
            return true;
        }
        // A driven parameter claims no curve: its value comes from the value graph, which owns its
        // own bounds checking. Returning here rather than reaching the std::get below is what keeps
        // a value-graph arm from being read as a curve index.
        if (const auto* driven = std::get_if<ValueOutputIndex>(&parameter.source)) {
            if (driven->value() >= plan->valueOutputCount()) {
                auto subject = operationSubject;
                subject.parameterId = parameter.id;
                subject.field = std::string(field);
                parameterFailure = diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                                              "Parameter references an invalid value-graph output",
                                              {}, std::move(subject));
                return false;
            }
            return true;
        }
        const auto curveIndex = std::get<Vec2CurveIndex>(parameter.source).value();
        auto& references = vec2CurveReferences[curveIndex];
        if (references != 0) {
            auto subject = operationSubject;
            subject.parameterId = parameter.id;
            subject.animationCurveId = plan->vec2Curves()[curveIndex].id;
            subject.field = std::string(field);
            parameterFailure =
                diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                           "Animation curve has multiple parameter owners", {}, std::move(subject));
            return false;
        }
        references = 1;
        vec2CurveOwners[curveIndex] = parameter.id;
        vec2CurveFields[curveIndex] = field;
        return true;
    };
    const auto registerColor = [&](const CompiledColorParameter& parameter,
                                   const std::string_view field,
                                   const EvaluationSubject& operationSubject) {
        if (!registerParameter(parameter.id, operationSubject)) {
            return false;
        }
        if (const auto* constant = std::get_if<core::Color4d>(&parameter.source)) {
            if (!constant->isValid()) {
                auto subject = operationSubject;
                subject.parameterId = parameter.id;
                subject.field = std::string(field);
                parameterFailure =
                    diagnostic(EvaluationDiagnosticCode::InvalidParameter,
                               "Color is not a valid authoring color", {}, std::move(subject));
                return false;
            }
            return true;
        }
        // A driven parameter claims no curve: its value comes from the value graph, which owns its
        // own bounds checking. Returning here rather than reaching the std::get below is what keeps
        // a value-graph arm from being read as a curve index.
        if (const auto* driven = std::get_if<ValueOutputIndex>(&parameter.source)) {
            if (driven->value() >= plan->valueOutputCount()) {
                auto subject = operationSubject;
                subject.parameterId = parameter.id;
                subject.field = std::string(field);
                parameterFailure = diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                                              "Parameter references an invalid value-graph output",
                                              {}, std::move(subject));
                return false;
            }
            return true;
        }
        const auto curveIndex = std::get<Color4CurveIndex>(parameter.source).value();
        auto& references = color4CurveReferences[curveIndex];
        if (references != 0) {
            auto subject = operationSubject;
            subject.parameterId = parameter.id;
            subject.animationCurveId = plan->color4Curves()[curveIndex].id;
            subject.field = std::string(field);
            parameterFailure =
                diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                           "Animation curve has multiple parameter owners", {}, std::move(subject));
            return false;
        }
        references = 1;
        color4CurveOwners[curveIndex] = parameter.id;
        color4CurveFields[curveIndex] = field;
        return true;
    };
    for (std::size_t index = 0; index < plan->operations().size() && !parameterFailure.has_value();
         ++index) {
        if (cancellation.isCancellationRequested()) {
            return PreflightOutcome::cancellation();
        }
        const auto operationSubject =
            subjectFor(OperationIndex::fromRaw(index), plan->operations()[index]);
        std::visit(
            Overloaded{
                [&](const CompiledSolid& solid) {
                    if (solid.width.has_value() != solid.height.has_value()) {
                        parameterFailure = diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                                                      "Solid dimensions must be present together",
                                                      {}, operationSubject);
                        return;
                    }
                    static_cast<void>(registerColor(solid.color, "color", operationSubject));
                    if (solid.width)
                        static_cast<void>(registerScalar(
                            *solid.width, "width", ScalarDomain::Dimension, operationSubject));
                    if (solid.height)
                        static_cast<void>(registerScalar(
                            *solid.height, "height", ScalarDomain::Dimension, operationSubject));
                },
                [&](const CompiledText& text) {
                    if (text.layout) {
                        static_cast<void>(
                            registerParameter(text.layout->alignmentId, operationSubject) &&
                            registerScalar(text.layout->lineHeight, "line-height",
                                           ScalarDomain::Positive, operationSubject) &&
                            registerScalar(text.layout->letterSpacing, "letter-spacing",
                                           ScalarDomain::Unbounded, operationSubject));
                    }
                    // Every check is re-run here rather than trusted from compilation: the
                    // evaluator validates the plan it is handed, because a plan can also
                    // arrive from a retained frame or a test fixture rather than straight
                    // from the compiler.
                    static_cast<void>(
                        registerParameter(text.contentParameterId, operationSubject) &&
                        registerScalar(text.size, "size", ScalarDomain::TextSize,
                                       operationSubject) &&
                        registerColor(text.color, "color", operationSubject));
                },
                [&](const CompiledLayerOutput& layer) {
                    // One rule applied five times, through exactly the helpers a Solid's and
                    // a Text's own operands use.
                    static_cast<void>(registerVec2(layer.position, "position", operationSubject) &&
                                      registerVec2(layer.anchor, "anchor", operationSubject) &&
                                      registerVec2(layer.scale, "scale", operationSubject) &&
                                      registerScalar(layer.rotation, "rotation",
                                                     ScalarDomain::Unbounded, operationSubject) &&
                                      registerScalar(layer.opacity, "opacity", ScalarDomain::Unit,
                                                     operationSubject));
                },
                [](const CompiledImageSource&) {},
                [](const CompiledMerge&) {},
                [](const CompiledCompositionOutput&) {},
            },
            plan->operations()[index]);
    }
    if (parameterFailure.has_value()) {
        return PreflightOutcome::failure(std::move(*parameterFailure));
    }
    // The VALUE graph references curves too (task FIX1, item G): a literal Scalar, Vector 2 or
    // Colour node whose authored value is on a curve lowers to a curve index, and its curve is as
    // referenced as any layer parameter's. Counted here, before the unreferenced-curve rule below
    // runs, so that rule keeps meaning "no compiled curve is dead" rather than "no curve outside
    // the image chain exists".
    for (const auto& operation : plan->valueOperations()) {
        if (cancellation.isCancellationRequested()) {
            return PreflightOutcome::cancellation();
        }
        forEachValueOperand(operation.kernel, [&](const CompiledValueOperand& operand) {
            if (const auto* scalar = std::get_if<ScalarCurveIndex>(&operand.source);
                scalar != nullptr && scalar->value() < scalarCurveReferences.size()) {
                scalarCurveReferences[scalar->value()] = 1;
            } else if (const auto* vector = std::get_if<Vec2CurveIndex>(&operand.source);
                       vector != nullptr && vector->value() < vec2CurveReferences.size()) {
                vec2CurveReferences[vector->value()] = 1;
            } else if (const auto* color = std::get_if<Color4CurveIndex>(&operand.source);
                       color != nullptr && color->value() < color4CurveReferences.size()) {
                color4CurveReferences[color->value()] = 1;
            } else if (const auto* vector3 = std::get_if<Vec3CurveIndex>(&operand.source);
                       vector3 != nullptr && vector3->value() < vec3CurveReferences.size()) {
                vec3CurveReferences[vector3->value()] = 1;
            }
        });
    }
    for (std::size_t index = 0; index < scalarCurveReferences.size(); ++index) {
        if (scalarCurveReferences[index] == 0) {
            EvaluationSubject subject;
            subject.animationCurveId = plan->scalarCurves()[index].id;
            subject.field = "animationCurve";
            return PreflightOutcome::failure(diagnostic(
                EvaluationDiagnosticCode::InvalidPlan,
                "Compiled plan contains an unreferenced animation curve", {}, std::move(subject)));
        }
    }
    for (std::size_t index = 0; index < vec2CurveReferences.size(); ++index) {
        if (vec2CurveReferences[index] == 0) {
            EvaluationSubject subject;
            subject.animationCurveId = plan->vec2Curves()[index].id;
            subject.field = "animationCurve";
            return PreflightOutcome::failure(diagnostic(
                EvaluationDiagnosticCode::InvalidPlan,
                "Compiled plan contains an unreferenced animation curve", {}, std::move(subject)));
        }
    }
    for (std::size_t index = 0; index < color4CurveReferences.size(); ++index) {
        if (color4CurveReferences[index] == 0) {
            EvaluationSubject subject;
            subject.animationCurveId = plan->color4Curves()[index].id;
            subject.field = "animationCurve";
            return PreflightOutcome::failure(diagnostic(
                EvaluationDiagnosticCode::InvalidPlan,
                "Compiled plan contains an unreferenced animation curve", {}, std::move(subject)));
        }
    }
    for (std::size_t index = 0; index < vec3CurveReferences.size(); ++index) {
        if (vec3CurveReferences[index] == 0) {
            EvaluationSubject subject;
            subject.animationCurveId = plan->vec3Curves()[index].id;
            subject.field = "animationCurve";
            return PreflightOutcome::failure(diagnostic(
                EvaluationDiagnosticCode::InvalidPlan,
                "Compiled plan contains an unreferenced animation curve", {}, std::move(subject)));
        }
    }

    std::vector<ResolvedCurveSample<double>> scalarCurveValues;
    scalarCurveValues.reserve(plan->scalarCurves().size());
    std::unordered_set<document::KeyframeId> keyframeIds;
    for (std::size_t curveIndex = 0; curveIndex < plan->scalarCurves().size(); ++curveIndex) {
        const auto& curve = plan->scalarCurves()[curveIndex];
        if (cancellation.isCancellationRequested()) {
            return PreflightOutcome::cancellation();
        }
        for (const auto& keyframe : curve.keyframes) {
            if (cancellation.isCancellationRequested()) {
                return PreflightOutcome::cancellation();
            }
            if (!keyframe.id.isValid() || !keyframeIds.insert(keyframe.id).second) {
                EvaluationSubject subject;
                subject.animationCurveId = curve.id;
                subject.keyframeId = keyframe.id;
                subject.field = "animationCurve.keyframes";
                return PreflightOutcome::failure(diagnostic(
                    EvaluationDiagnosticCode::InvalidPlan,
                    "Animation keyframe identity is not canonical",
                    "Keyframe identities must be valid and globally unique.", std::move(subject)));
            }
            const auto domain = scalarCurveDomains[curveIndex];
            if (!std::isfinite(keyframe.value) || !withinScalarDomain(domain, keyframe.value)) {
                EvaluationSubject subject;
                subject.parameterId = scalarCurveOwners[curveIndex];
                subject.animationCurveId = curve.id;
                subject.keyframeId = keyframe.id;
                subject.field = std::string(scalarCurveFields[curveIndex]);
                return PreflightOutcome::failure(
                    diagnostic(EvaluationDiagnosticCode::InvalidParameter,
                               scalarDomainFailureSummary(domain), {}, std::move(subject)));
            }
        }
        const auto sample = sampleAnimationCurve(curve, request.time, cancellation);
        if (sample.error == AnimationSamplingError::Cancelled) {
            return PreflightOutcome::cancellation();
        }
        if (!sample || !sample.value.has_value() || !sample.segmentStart.has_value()) {
            return PreflightOutcome::failure(animationDiagnostic(sample.error, curve.id, sample));
        }
        scalarCurveValues.push_back({*sample.value, *sample.segmentStart});
    }

    std::vector<ResolvedCurveSample<document::Vec2d>> vec2CurveValues;
    vec2CurveValues.reserve(plan->vec2Curves().size());
    for (std::size_t curveIndex = 0; curveIndex < plan->vec2Curves().size(); ++curveIndex) {
        const auto& curve = plan->vec2Curves()[curveIndex];
        if (cancellation.isCancellationRequested()) {
            return PreflightOutcome::cancellation();
        }
        const bool componentCurve = std::ranges::any_of(
            curve.components, [](const auto& component) { return !component.empty(); });
        if (componentCurve) {
            for (const auto& component : curve.components) {
                for (const auto& keyframe : component) {
                    if (cancellation.isCancellationRequested()) {
                        return PreflightOutcome::cancellation();
                    }
                    if (!keyframe.id.isValid() || !keyframeIds.insert(keyframe.id).second) {
                        EvaluationSubject subject;
                        subject.animationCurveId = curve.id;
                        subject.keyframeId = keyframe.id;
                        subject.field = "animationCurve.components";
                        return PreflightOutcome::failure(
                            diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                                       "Animation keyframe identity is not canonical",
                                       "Keyframe identities must be valid and globally unique.",
                                       std::move(subject)));
                    }
                    if (!std::isfinite(keyframe.value)) {
                        EvaluationSubject subject;
                        subject.parameterId = vec2CurveOwners[curveIndex];
                        subject.animationCurveId = curve.id;
                        subject.keyframeId = keyframe.id;
                        subject.field = std::string(vec2CurveFields[curveIndex]);
                        return PreflightOutcome::failure(diagnostic(
                            EvaluationDiagnosticCode::InvalidParameter,
                            "Animated layer transform key is not finite", {}, std::move(subject)));
                    }
                }
            }
        } else {
            for (const auto& keyframe : curve.keyframes) {
                if (cancellation.isCancellationRequested()) {
                    return PreflightOutcome::cancellation();
                }
                if (!keyframe.id.isValid() || !keyframeIds.insert(keyframe.id).second) {
                    EvaluationSubject subject;
                    subject.animationCurveId = curve.id;
                    subject.keyframeId = keyframe.id;
                    subject.field = "animationCurve.keyframes";
                    return PreflightOutcome::failure(
                        diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                                   "Animation keyframe identity is not canonical",
                                   "Keyframe identities must be valid and globally unique.",
                                   std::move(subject)));
                }
                if (!std::isfinite(keyframe.value.x) || !std::isfinite(keyframe.value.y)) {
                    EvaluationSubject subject;
                    subject.parameterId = vec2CurveOwners[curveIndex];
                    subject.animationCurveId = curve.id;
                    subject.keyframeId = keyframe.id;
                    subject.field = std::string(vec2CurveFields[curveIndex]);
                    return PreflightOutcome::failure(diagnostic(
                        EvaluationDiagnosticCode::InvalidParameter,
                        "Animated layer transform key is not finite", {}, std::move(subject)));
                }
            }
        }
        const auto sample = sampleAnimationCurve(curve, request.time, cancellation);
        if (sample.error == AnimationSamplingError::Cancelled) {
            return PreflightOutcome::cancellation();
        }
        if (!sample || !sample.value.has_value() || !sample.segmentStart.has_value()) {
            return PreflightOutcome::failure(animationDiagnostic(sample.error, curve.id, sample));
        }
        vec2CurveValues.push_back({*sample.value, *sample.segmentStart});
    }

    std::vector<ResolvedCurveSample<core::Color4d>> color4CurveValues;
    color4CurveValues.reserve(plan->color4Curves().size());
    for (std::size_t curveIndex = 0; curveIndex < plan->color4Curves().size(); ++curveIndex) {
        const auto& curve = plan->color4Curves()[curveIndex];
        if (cancellation.isCancellationRequested()) {
            return PreflightOutcome::cancellation();
        }
        const bool componentCurve = std::ranges::any_of(
            curve.components, [](const auto& component) { return !component.empty(); });
        if (componentCurve) {
            for (std::size_t componentIndex = 0; componentIndex < curve.components.size();
                 ++componentIndex) {
                for (const auto& keyframe : curve.components[componentIndex]) {
                    if (cancellation.isCancellationRequested()) {
                        return PreflightOutcome::cancellation();
                    }
                    if (!keyframe.id.isValid() || !keyframeIds.insert(keyframe.id).second) {
                        EvaluationSubject subject;
                        subject.animationCurveId = curve.id;
                        subject.keyframeId = keyframe.id;
                        subject.field = "animationCurve.components";
                        return PreflightOutcome::failure(
                            diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                                       "Animation keyframe identity is not canonical",
                                       "Keyframe identities must be valid and globally unique.",
                                       std::move(subject)));
                    }
                    const bool valid = componentIndex == 3
                                           ? std::isfinite(keyframe.value) &&
                                                 keyframe.value >= 0.0 && keyframe.value <= 1.0
                                           : std::isfinite(keyframe.value);
                    if (!valid) {
                        EvaluationSubject subject;
                        subject.parameterId = color4CurveOwners[curveIndex];
                        subject.animationCurveId = curve.id;
                        subject.keyframeId = keyframe.id;
                        subject.field = std::string(color4CurveFields[curveIndex]);
                        return PreflightOutcome::failure(
                            diagnostic(EvaluationDiagnosticCode::InvalidParameter,
                                       "Animated color key is not a valid authoring color", {},
                                       std::move(subject)));
                    }
                }
            }
        } else {
            for (const auto& keyframe : curve.keyframes) {
                if (cancellation.isCancellationRequested()) {
                    return PreflightOutcome::cancellation();
                }
                if (!keyframe.id.isValid() || !keyframeIds.insert(keyframe.id).second) {
                    EvaluationSubject subject;
                    subject.animationCurveId = curve.id;
                    subject.keyframeId = keyframe.id;
                    subject.field = "animationCurve.keyframes";
                    return PreflightOutcome::failure(
                        diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                                   "Animation keyframe identity is not canonical",
                                   "Keyframe identities must be valid and globally unique.",
                                   std::move(subject)));
                }
                // The authoring-colour contract, exactly as a constant colour satisfies it.
                if (!keyframe.value.isValid()) {
                    EvaluationSubject subject;
                    subject.parameterId = color4CurveOwners[curveIndex];
                    subject.animationCurveId = curve.id;
                    subject.keyframeId = keyframe.id;
                    subject.field = std::string(color4CurveFields[curveIndex]);
                    return PreflightOutcome::failure(
                        diagnostic(EvaluationDiagnosticCode::InvalidParameter,
                                   "Animated color key is not a valid authoring color", {},
                                   std::move(subject)));
                }
            }
        }
        const auto sample = sampleAnimationCurve(curve, request.time, cancellation);
        if (sample.error == AnimationSamplingError::Cancelled) {
            return PreflightOutcome::cancellation();
        }
        if (!sample || !sample.value.has_value() || !sample.segmentStart.has_value()) {
            return PreflightOutcome::failure(animationDiagnostic(sample.error, curve.id, sample));
        }
        color4CurveValues.push_back({*sample.value, *sample.segmentStart});
    }

    const auto imageBytes = descriptorResult.value()->layout().pixelStorageBytes;
    auto remaining = consumers;
    std::size_t residentBytes = 0;
    std::size_t peakBytes = 0;
    for (std::size_t index = 0; index < operationCount; ++index) {
        if (cancellation.isCancellationRequested()) {
            return PreflightOutcome::cancellation();
        }
        if (isImageProducing(plan->operations()[index])) {
            if (!checkedAdd(residentBytes, imageBytes, residentBytes)) {
                return PreflightOutcome::failure(
                    diagnostic(EvaluationDiagnosticCode::ArithmeticOverflow,
                               "Evaluation working set overflowed"));
            }
            peakBytes = std::max(peakBytes, residentBytes);
        }
        if (index == request.output.value()) {
            continue;
        }
        forEachInput(plan->operations()[index],
                     [&remaining, &residentBytes, imageBytes](const OperationIndex input) {
                         auto& count = remaining[input.value()];
                         --count;
                         if (count == 0) {
                             residentBytes -= imageBytes;
                         }
                     });
    }
    if (peakBytes > request.pixelStorageByteLimit) {
        return PreflightOutcome::failure(
            diagnostic(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                       "Evaluation exceeds its pixel-storage budget",
                       "requiredPeakBytes=" + std::to_string(peakBytes) +
                           " byteLimit=" + std::to_string(request.pixelStorageByteLimit)));
    }

    // The value graph, evaluated once for this frame from the SAME request time the curves above
    // were sampled at. Its diagnostics are scoped per node and substitute each node's documented
    // fallback, so a divisor that reached zero degrades one value rather than failing the frame --
    // which is why they are reported as warnings beside a rendered picture instead of becoming a
    // preflight failure.
    auto valueGraph = evaluateValueGraph(
        plan->valueOperations(), plan->valueOutputCount(), request.time, plan->format().frameRate(),
        ValueGraphCurves{plan->scalarCurves(), plan->vec2Curves(), plan->color4Curves(),
                         plan->vec3Curves()},
        {cache, statistics, plan->sourceRevision(), plan->projectId(), plan->compositionId(),
         &cancellation, plan->valueTimeDependence()});
    if (cancellation.isCancellationRequested()) {
        return PreflightOutcome::cancellation();
    }

    return PreflightOutcome::success(
        ResolvedEvaluation{.imageDescriptor = *descriptorResult.value(),
                           .horizontalScale = horizontalScale,
                           .verticalScale = verticalScale,
                           .imageBytes = imageBytes,
                           .remainingConsumers = std::move(consumers),
                           .scalarCurveValues = std::move(scalarCurveValues),
                           .vec2CurveValues = std::move(vec2CurveValues),
                           .color4CurveValues = std::move(color4CurveValues),
                           .valueOutputs = std::move(valueGraph.outputs)});
}

[[nodiscard]] EvaluationResult unexpectedAllocationFailure() {
    return EvaluationResult::failed(
        diagnostic(EvaluationDiagnosticCode::AllocationFailure,
                   "Evaluation control storage could not be allocated"));
}

} // namespace detail

using detail::diagnostic;
using detail::forEachInput;
using detail::imageDiagnostic;
using detail::Overloaded;
using detail::preflight;
using detail::reportProgress;
using detail::reportRowPassFinished;
using detail::reportRowPassStarted;
using detail::RowFailure;
using detail::rowPassFailure;
using detail::subjectFor;
using detail::unexpectedAllocationFailure;

namespace {

[[nodiscard]] std::optional<core::RationalTime>
audioStartTime(const std::int64_t frame, const document::FrameRate rate) noexcept {
    const auto magnitude = frame < 0 ? static_cast<std::uint64_t>(-(frame + 1)) + std::uint64_t{1}
                                     : static_cast<std::uint64_t>(frame);
    const auto denominator = static_cast<std::uint64_t>(rate.numerator());
    constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (denominator == 0 || magnitude > (maximum + (frame < 0 ? 1U : 0U)) / rate.denominator())
        return std::nullopt;
    const auto product = magnitude * rate.denominator();
    std::int64_t numerator = 0;
    if (frame < 0) {
        numerator = product == maximum + 1U ? std::numeric_limits<std::int64_t>::min()
                                            : -static_cast<std::int64_t>(product);
    } else {
        numerator = static_cast<std::int64_t>(product);
    }
    return core::RationalTime::create(numerator, static_cast<std::int64_t>(denominator));
}

} // namespace

std::optional<AudioMixDescription> CpuCompositionEvaluator::evaluateAudioMix(
    const std::shared_ptr<const CompiledCompositionPlan>& plan, const core::RationalTime time,
    const CancellationToken& cancellation) const {
    if (!plan || cancellation.isCancellationRequested())
        return std::nullopt;

    std::optional<ValueGraphEvaluation> valueGraph;
    const auto needsValueGraph =
        std::ranges::any_of(plan->audioMix().sources, [](const auto& source) {
            return std::holds_alternative<ValueOutputIndex>(source.level.source);
        });
    if (needsValueGraph) {
        valueGraph = evaluateValueGraph(plan->valueOperations(), plan->valueOutputCount(), time,
                                        plan->format().frameRate(),
                                        ValueGraphCurves{plan->scalarCurves(), plan->vec2Curves(),
                                                         plan->color4Curves(), plan->vec3Curves()});
        if (cancellation.isCancellationRequested() || !valueGraph->diagnostics.empty())
            return std::nullopt;
    }

    AudioMixDescription description{.outputNodeId = plan->audioMix().outputNodeId, .clips = {}};
    description.clips.reserve(plan->audioMix().layers.size());
    for (const auto& layer : plan->audioMix().layers) {
        if (cancellation.isCancellationRequested() ||
            layer.sourceIndex >= plan->audioMix().sources.size())
            return std::nullopt;
        const auto& source = plan->audioMix().sources[layer.sourceIndex];
        const auto start = audioStartTime(source.startFrame, plan->format().frameRate());
        if (!start)
            return std::nullopt;
        double level = 1.0;
        if (const auto* constant = std::get_if<double>(&source.level.source)) {
            level = *constant;
        } else if (const auto* curve = std::get_if<ScalarCurveIndex>(&source.level.source)) {
            if (curve->value() >= plan->scalarCurves().size())
                return std::nullopt;
            const auto sample = sampleAnimationCurve(plan->scalarCurves()[curve->value()], time);
            if (!sample || !sample.value.has_value())
                return std::nullopt;
            level = *sample.value;
        } else if (const auto* output = std::get_if<ValueOutputIndex>(&source.level.source)) {
            if (!valueGraph || output->value() >= valueGraph->outputs.size())
                return std::nullopt;
            const auto* resolved = std::get_if<double>(&valueGraph->outputs[output->value()]);
            if (resolved == nullptr)
                return std::nullopt;
            level = *resolved;
        } else {
            return std::nullopt;
        }
        if (!std::isfinite(level) || level < 0.0 || level > 2.0)
            return std::nullopt;
        description.clips.push_back({source.sourceNodeId, source.assetId, *start, layer.outPoint,
                                     level, !layer.enabled, layer.solo});
    }
    return description;
}

EvaluationResult CpuCompositionEvaluator::evaluate(
    std::shared_ptr<const CompiledCompositionPlan> plan, const EvaluationRequest& request,
    const CancellationToken& cancellation, EvaluationProgressCallback progress,
    CpuRowBandExecutor* const rowBands, OperationCacheStatistics* statistics) const {
    OperationCacheStatistics frameStatistics;
    if (statistics)
        *statistics = {};
    auto* cache = (request.bypassOperationCache || (plan && plan->bypassOperationCache()))
                      ? nullptr
                      : cache_.get();
    std::vector<EvaluationDiagnostic> imageWarnings;
    const auto mediaBase = assetBaseDirectory();
    try {
        auto checked = preflight(plan, request, cancellation, progress, cache, &frameStatistics);
        if (checked.cancelled || cancellation.isCancellationRequested()) {
            return EvaluationResult::cancelled();
        }
        if (!checked.resolved.has_value()) {
            if (checked.diagnostic.has_value()) {
                return EvaluationResult::failed(std::move(*checked.diagnostic));
            }
            return EvaluationResult::failed(
                diagnostic(EvaluationDiagnosticCode::InternalInvariant,
                           "Evaluation preflight produced no result or diagnostic"));
        }
        auto resolved = std::move(*checked.resolved);
        std::vector<std::shared_ptr<const render::Rgba32fImage>> slots(plan->operations().size());
        std::vector<EvaluatedOperationBounds> bounds(plan->operations().size());
        const auto remainingPixelBudget = [&] {
            auto remaining = request.pixelStorageByteLimit;
            for (const auto& slot : slots) {
                if (!slot)
                    continue;
                const auto bytes = slot->pixels().size_bytes();
                if (bytes > remaining)
                    return std::size_t{0};
                remaining -= bytes;
            }
            return remaining;
        };
        std::shared_ptr<const render::Rgba32fImage> processImage;
        std::vector<std::string> contentHashes(plan->operations().size());

        for (std::size_t index = 0; index < plan->operations().size(); ++index) {
            if (cancellation.isCancellationRequested()) {
                return EvaluationResult::cancelled();
            }
            const auto operationIndex = OperationIndex::fromRaw(index);
            const auto operationSubject = subjectFor(operationIndex, plan->operations()[index]);
            std::optional<render::Rgba32fImage> produced;
            std::optional<EvaluationDiagnostic> operationFailure;
            bool operationCancelled = false;

            std::optional<detail::ImageSourceSelection> selectedImage;
            if (const auto* source = std::get_if<CompiledImageSource>(&plan->operations()[index])) {
                selectedImage = detail::selectImageSource(
                    *source, request.time, plan->format().frameRate(), mediaBase, cancellation);
                if (selectedImage->cancelled)
                    return EvaluationResult::cancelled();
                if (!selectedImage->warning.empty())
                    imageWarnings.push_back({EvaluationDiagnosticCode::InvalidParameter,
                                             DiagnosticSeverity::Warning,
                                             operationSubject,
                                             selectedImage->warning,
                                             {}});
            }
            detail::OperationKey key;
            if (cache) {
                key.add(std::string("image"));
                key.add(plan->projectId());
                key.add(plan->compositionId());
                key.add(plan->format().width());
                key.add(plan->format().height());
                key.add(plan->format().pixelAspect());
                key.add(plan->format().frameRate());
                key.add(request.resolution.index());
                if (const auto* proxy = std::get_if<ProxyResolution>(&request.resolution)) {
                    key.add(proxy->extent.width());
                    key.add(proxy->extent.height());
                }
                key.add(plan->operationTimeDependent(operationIndex));
                if (plan->operationTimeDependent(operationIndex))
                    key.add(request.time);
                key.add(plan->operations()[index].index());
                const auto parameter = [&](const auto& operand) {
                    const auto value = detail::resolveParameter(operand, *plan, resolved);
                    key.add(value.has_value());
                    if (value)
                        key.add(value->value);
                };
                // Task DRIVE-1. The same participation for the kinds with no curve arm: what enters
                // the key is the value this FRAME resolves to, never the authored constant beside
                // it, so a text layer whose words change per frame cannot serve a cached image of
                // the words it had last frame.
                const auto authored = [&](const document::ParameterId id, const auto& constant,
                                          const std::optional<ValueOutputIndex>& driven) {
                    const auto value = detail::resolveParameter(id, constant, driven, resolved);
                    key.add(value.has_value());
                    if (value)
                        key.add(value->value);
                };
                std::visit(
                    [&](const auto& step) {
                        key.add(step.sourceNodeId);
                        using Step = std::decay_t<decltype(step)>;
                        if constexpr (std::is_same_v<Step, CompiledSolid>) {
                            parameter(step.color);
                            if (step.width)
                                parameter(*step.width);
                            if (step.height)
                                parameter(*step.height);
                        } else if constexpr (std::is_same_v<Step, CompiledImageSource>) {
                            key.add(selectedImage->cacheKey);
                        } else if constexpr (std::is_same_v<Step, CompiledText>) {
                            authored(step.contentParameterId, step.content, step.drivenContent);
                            key.add(step.layout.has_value());
                            if (step.layout) {
                                authored(step.layout->alignmentId, step.layout->alignment,
                                         step.layout->drivenAlignment);
                                parameter(step.layout->lineHeight);
                                parameter(step.layout->letterSpacing);
                            }
                            parameter(step.size);
                            parameter(step.color);
                        } else if constexpr (std::is_same_v<Step, CompiledLayerOutput>) {
                            key.add(step.localBounds);
                            parameter(step.position);
                            parameter(step.anchor);
                            parameter(step.scale);
                            parameter(step.rotation);
                            parameter(step.opacity);
                            authored(step.blendModeParameterId,
                                     core::blendModeStoredValue(step.blendMode),
                                     step.drivenBlendMode);
                            key.add(request.time >= step.inPoint &&
                                    (!step.outPoint || request.time < *step.outPoint));
                        } else if constexpr (std::is_same_v<Step, CompiledMerge>) {
                            key.add(step.localBounds);
                            key.add(step.entries.size());
                            for (const auto& entry : step.entries)
                                key.add(entry.layerId.isValid());
                        }
                    },
                    plan->operations()[index]);
                forEachInput(plan->operations()[index],
                             [&](OperationIndex input) { key.add(contentHashes[input.value()]); });
                contentHashes[index] = key.digest();
            }
            const auto hit =
                cache ? cache->find(key.bytes(), plan->sourceRevision()) : std::nullopt;
            if (hit) {
                if (hit->image && hit->image->pixels().size_bytes() > remainingPixelBudget())
                    return EvaluationResult::failed(
                        diagnostic(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                                   "Cached operation exceeds the resident pixel budget", {},
                                   operationSubject));
                ++frameStatistics.hits;
                slots[index] = hit->image;
                bounds[index] = hit->bounds;
                if (index == request.output.value())
                    processImage = hit->image;
                reportProgress(progress, {.stage = EvaluationProgressStage::Operation,
                                          .operation = operationIndex,
                                          .completed = 1,
                                          .total = 1});
            } else {
                ++frameStatistics.misses;
                frameStatistics.evaluatedNodes.push_back(
                    operationSubject.nodeId.value_or(document::NodeId{}));
                std::visit(
                    Overloaded{
                        [&](const CompiledSolid& solid) {
                            const auto color =
                                detail::resolveParameter(solid.color, *plan, resolved);
                            if (!color.has_value()) {
                                operationFailure = diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                                                              "Solid color could not be resolved",
                                                              {}, operationSubject);
                                return;
                            }
                            const auto pixel =
                                render::solidPixelFromStraightLinearRec709Scene(color->value);
                            if (!pixel) {
                                operationFailure = imageDiagnostic(
                                    *pixel.error(),
                                    detail::parameterSubject(operationSubject, *color, "color"),
                                    "Solid color is not evaluable");
                                return;
                            }
                            auto descriptor = resolved.imageDescriptor;
                            double exactWidth =
                                static_cast<double>(descriptor.dataWindow().extent().width());
                            double exactHeight =
                                static_cast<double>(descriptor.dataWindow().extent().height());
                            double authorWidth = plan->format().width();
                            double authorHeight = plan->format().height();
                            if (solid.width && solid.height) {
                                const auto width =
                                    detail::resolveParameter(*solid.width, *plan, resolved);
                                const auto height =
                                    detail::resolveParameter(*solid.height, *plan, resolved);
                                if (!width || !height || width->value < 1.0 ||
                                    height->value < 1.0) {
                                    operationFailure = diagnostic(
                                        EvaluationDiagnosticCode::InvalidParameter,
                                        "Solid dimensions are not evaluable", {}, operationSubject);
                                    return;
                                }
                                // Preserve the exact device extent of composition-sized sources:
                                // width * (proxyWidth / width) can round above the integer and
                                // otherwise allocate an extra column, changing legacy pivots.
                                authorWidth = width->value;
                                authorHeight = height->value;
                                if (authorWidth != plan->format().width())
                                    exactWidth = authorWidth * resolved.horizontalScale;
                                if (authorHeight != plan->format().height())
                                    exactHeight = authorHeight * resolved.verticalScale;
                                const auto w = std::ceil(exactWidth);
                                const auto h = std::ceil(exactHeight);
                                if (!std::isfinite(w) || !std::isfinite(h) || w > 16777216.0 ||
                                    h > 16777216.0) {
                                    operationFailure =
                                        diagnostic(EvaluationDiagnosticCode::InvalidParameter,
                                                   "Solid dimensions exceed the supported extent",
                                                   {}, operationSubject);
                                    return;
                                }
                                const auto window =
                                    render::ImageWindow::create(0, 0, static_cast<std::uint64_t>(w),
                                                                static_cast<std::uint64_t>(h));
                                const auto local = render::Rgba32fImageDescriptor::create(
                                    *window.value(), descriptor.displayWindow(),
                                    descriptor.pixelAspect());
                                if (!local) {
                                    operationFailure =
                                        imageDiagnostic(*local.error(), operationSubject,
                                                        "Solid bounds are invalid");
                                    return;
                                }
                                descriptor = *local.value();
                            }
                            bounds[index].local = detail::boundsForWindow(descriptor.dataWindow(),
                                                                          resolved.horizontalScale,
                                                                          resolved.verticalScale);
                            bounds[index].local.right = authorWidth;
                            bounds[index].local.bottom = authorHeight;
                            bounds[index].output = bounds[index].local;
                            auto builder = render::Rgba32fImageBuilder::create(
                                descriptor, remainingPixelBudget());
                            if (!builder) {
                                operationFailure =
                                    imageDiagnostic(*builder.error(), operationSubject,
                                                    "Solid process image could not be allocated");
                                return;
                            }
                            const auto window = descriptor.dataWindow();
                            const auto height = window.extent().height();
                            reportRowPassStarted(progress, operationIndex, height);
                            auto& image = *builder.value();
                            const auto solidPixel = *pixel.value();
                            const auto outcome = runRowBandPass(
                                rowBands, cancellation, height, window.originY(),
                                [&image, solidPixel, exactWidth, exactHeight,
                                 &operationSubject](const std::int64_t y) -> RowFailure {
                                    auto row = image.row(y);
                                    if (!row) {
                                        return imageDiagnostic(
                                            *row.error(), operationSubject,
                                            "Solid output row could not be addressed");
                                    }
                                    render::fillSolidRow(*row.value(), solidPixel);
                                    const auto verticalCoverage =
                                        std::clamp(exactHeight - static_cast<double>(y), 0.0, 1.0);
                                    if (verticalCoverage == 1.0 &&
                                        exactWidth == static_cast<double>(row.value()->size()))
                                        return std::nullopt;
                                    const auto firstPartial =
                                        verticalCoverage == 1.0 ? row.value()->size() - 1 : 0;
                                    for (std::size_t x = firstPartial; x < row.value()->size();
                                         ++x) {
                                        const auto coverage =
                                            verticalCoverage *
                                            std::clamp(exactWidth - static_cast<double>(x), 0.0,
                                                       1.0);
                                        if (coverage == 1.0)
                                            continue;
                                        const auto value = render::Rgba32f::fromPremultiplied(
                                            static_cast<float>(
                                                static_cast<double>(solidPixel.red()) * coverage),
                                            static_cast<float>(
                                                static_cast<double>(solidPixel.green()) * coverage),
                                            static_cast<float>(
                                                static_cast<double>(solidPixel.blue()) * coverage),
                                            static_cast<float>(
                                                static_cast<double>(solidPixel.alpha()) *
                                                coverage));
                                        if (!value)
                                            return imageDiagnostic(
                                                *value.error(), operationSubject,
                                                "Solid edge coverage is invalid");
                                        (*row.value())[x] = *value.value();
                                    }
                                    return std::nullopt;
                                });
                            if (outcome.cancelled) {
                                operationCancelled = true;
                                return;
                            }
                            if (outcome.failure.has_value() || outcome.incomplete) {
                                operationFailure = rowPassFailure(outcome, operationSubject);
                                return;
                            }
                            reportRowPassFinished(progress, operationIndex, height);
                            auto frozen = std::move(*builder.value()).freeze();
                            if (!frozen) {
                                operationFailure =
                                    imageDiagnostic(*frozen.error(), operationSubject,
                                                    "Solid process image could not be published");
                                return;
                            }
                            produced.emplace(std::move(*frozen.value()));
                        },
                        [&](const CompiledImageSource&) {
                            if (!selectedImage || !selectedImage->available)
                                return;
                            auto image = detail::evaluateImageSource(
                                *selectedImage, resolved.imageDescriptor, resolved.horizontalScale,
                                resolved.verticalScale, remainingPixelBudget(), *decodedImages_,
                                cancellation);
                            if (image.cancelled) {
                                operationCancelled = true;
                                return;
                            }
                            if (!image.value.has_value()) {
                                imageWarnings.push_back({EvaluationDiagnosticCode::InvalidParameter,
                                                         DiagnosticSeverity::Warning,
                                                         operationSubject,
                                                         image.diagnostic,
                                                         {}});
                                return;
                            }
                            bounds[index].local = detail::boundsForWindow(
                                image.value->descriptor()->dataWindow(), resolved.horizontalScale,
                                resolved.verticalScale);
                            bounds[index].output = bounds[index].local;
                            produced.emplace(std::move(*image.value));
                        },
                        [&](const CompiledText& text) {
                            // A text source produces a full-frame image exactly like a solid, so
                            // the Layer Output stage transforms and fades it with the same
                            // primitive and the same five transform parameters. The difference is
                            // only what is inside the frame: transparent black everywhere except
                            // where glyph coverage lands.
                            //
                            // Placement: the text origin is the frame's own data-window origin, so
                            // the first line's ascender is flush with the top edge and its pen
                            // starts at the left edge. The layer transform then moves, turns, and
                            // scales the whole layer from there, which is why nothing here reads
                            // any of it.
                            const auto size = detail::resolveParameter(text.size, *plan, resolved);
                            const auto color =
                                detail::resolveParameter(text.color, *plan, resolved);
                            // Task DRIVE-1: the words themselves, which a driver may change every
                            // frame -- a frame counter, a timecode, a label assembled by a String
                            // node -- resolved by the same family that resolves the size they are
                            // drawn at.
                            const auto content =
                                detail::resolveParameter(text.contentParameterId, text.content,
                                                         text.drivenContent, resolved);
                            if (!size.has_value() || !color.has_value() || !content.has_value()) {
                                operationFailure = diagnostic(
                                    EvaluationDiagnosticCode::InvalidPlan,
                                    "Text parameter could not be resolved", {}, operationSubject);
                                return;
                            }
                            const auto pixel =
                                render::solidPixelFromStraightLinearRec709Scene(color->value);
                            if (!pixel) {
                                operationFailure = imageDiagnostic(
                                    *pixel.error(),
                                    detail::parameterSubject(operationSubject, *color, "color"),
                                    "Text color is not evaluable");
                                return;
                            }
                            // Proxy evaluation scales the em size per axis by exactly the factors
                            // the Layer Output stage scales translation by, so a proxy frame is a
                            // smaller picture of the same composition rather than full-size glyphs
                            // in a small frame.
                            const auto rasterParameters = render::TextRasterParameters::create(
                                size->value * resolved.horizontalScale,
                                size->value * resolved.verticalScale);
                            if (!rasterParameters) {
                                operationFailure = imageDiagnostic(
                                    *rasterParameters.error(),
                                    detail::parameterSubject(operationSubject, *size, "size"),
                                    "Text size is not rasterizable");
                                return;
                            }
                            render::TextLayoutOptions layout{.multiline = false};
                            if (text.layout) {
                                const auto lineHeight = detail::resolveParameter(
                                    text.layout->lineHeight, *plan, resolved);
                                const auto letterSpacing = detail::resolveParameter(
                                    text.layout->letterSpacing, *plan, resolved);
                                const auto alignment = detail::resolveParameter(
                                    text.layout->alignmentId, text.layout->alignment,
                                    text.layout->drivenAlignment, resolved);
                                if (!lineHeight || !letterSpacing || !alignment ||
                                    alignment->value < 0 || alignment->value > 2) {
                                    operationFailure =
                                        diagnostic(EvaluationDiagnosticCode::InvalidParameter,
                                                   "Text layout is invalid", {}, operationSubject);
                                    return;
                                }
                                layout = {static_cast<render::TextAlignment>(alignment->value),
                                          lineHeight->value,
                                          letterSpacing->value * resolved.horizontalScale, true};
                            }
                            auto coverage = render::TextCoverageBitmap::rasterizeEmbeddedDejaVuSans(
                                content->value, *rasterParameters.value(), remainingPixelBudget(),
                                layout);
                            if (!coverage) {
                                operationFailure =
                                    imageDiagnostic(*coverage.error(), operationSubject,
                                                    "Text content could not be rasterized");
                                operationFailure->subject.parameterId = text.contentParameterId;
                                operationFailure->subject.field = "content";
                                return;
                            }
                            auto descriptor = resolved.imageDescriptor;
                            if (text.layout) {
                                if (!coverage.value()->hasCoverage())
                                    return;
                                const auto window = render::ImageWindow::create(
                                    coverage.value()->originX(), coverage.value()->originY(),
                                    coverage.value()->width(), coverage.value()->height());
                                const auto local = render::Rgba32fImageDescriptor::create(
                                    *window.value(), descriptor.displayWindow(),
                                    descriptor.pixelAspect());
                                if (!local) {
                                    operationFailure =
                                        imageDiagnostic(*local.error(), operationSubject,
                                                        "Text bounds are invalid");
                                    return;
                                }
                                descriptor = *local.value();
                            }
                            bounds[index].local = detail::boundsForWindow(descriptor.dataWindow(),
                                                                          resolved.horizontalScale,
                                                                          resolved.verticalScale);
                            bounds[index].output = bounds[index].local;
                            auto builder = render::Rgba32fImageBuilder::create(
                                descriptor,
                                remainingPixelBudget() -
                                    std::min(remainingPixelBudget(),
                                             coverage.value()->coverage().size()),
                                render::Rgba32f::transparent());
                            if (!builder) {
                                operationFailure =
                                    imageDiagnostic(*builder.error(), operationSubject,
                                                    "Text process image could not be allocated");
                                return;
                            }
                            const auto window = descriptor.dataWindow();
                            const auto height = window.extent().height();
                            const auto& bitmap = *coverage.value();
                            reportRowPassStarted(progress, operationIndex, height);
                            auto& image = *builder.value();
                            const auto textPixel = *pixel.value();
                            const auto outcome = runRowBandPass(
                                rowBands, cancellation, height, window.originY(),
                                [&image, &bitmap, window, textPixel,
                                 &operationSubject](const std::int64_t y) -> RowFailure {
                                    const auto clipped = detail::clipCoverageRow(bitmap, window, y);
                                    if (clipped.coverage.empty()) {
                                        return std::nullopt;
                                    }
                                    auto outputRow = image.row(y);
                                    if (!outputRow) {
                                        return imageDiagnostic(
                                            *outputRow.error(), operationSubject,
                                            "Text output row could not be addressed");
                                    }
                                    if (const auto rowStatus = render::coverageSolidRow(
                                            clipped.coverage, textPixel,
                                            outputRow.value()->subspan(clipped.outputOffset,
                                                                       clipped.coverage.size()))) {
                                        return imageDiagnostic(
                                            *rowStatus, operationSubject,
                                            "Text coverage could not be composited");
                                    }
                                    return std::nullopt;
                                });
                            if (outcome.cancelled) {
                                operationCancelled = true;
                                return;
                            }
                            if (outcome.failure.has_value() || outcome.incomplete) {
                                operationFailure = rowPassFailure(outcome, operationSubject);
                                return;
                            }
                            reportRowPassFinished(progress, operationIndex, height);
                            auto frozen = std::move(*builder.value()).freeze();
                            if (!frozen) {
                                operationFailure =
                                    imageDiagnostic(*frozen.error(), operationSubject,
                                                    "Text process image could not be published");
                                return;
                            }
                            produced.emplace(std::move(*frozen.value()));
                        },
                        [&](const CompiledLayerOutput& layer) {
                            if (request.time < layer.inPoint ||
                                (layer.outPoint && request.time >= *layer.outPoint))
                                return;
                            const auto position =
                                detail::resolveParameter(layer.position, *plan, resolved);
                            const auto anchor =
                                detail::resolveParameter(layer.anchor, *plan, resolved);
                            const auto scale =
                                detail::resolveParameter(layer.scale, *plan, resolved);
                            const auto rotation =
                                detail::resolveParameter(layer.rotation, *plan, resolved);
                            const auto opacity =
                                detail::resolveParameter(layer.opacity, *plan, resolved);
                            if (!position.has_value() || !anchor.has_value() ||
                                !scale.has_value() || !rotation.has_value() ||
                                !opacity.has_value()) {
                                operationFailure = diagnostic(
                                    EvaluationDiagnosticCode::InvalidPlan,
                                    "Layer parameter could not be resolved", {}, operationSubject);
                                return;
                            }
                            // Position is authored in composition coordinates and means "put the
                            // layer centre here", so the displacement the transform needs is
                            // position minus that centre. The proxy factor is applied inside
                            // render::LayerTransform, with the same expression the pre-proxy-aware
                            // code used, which is what keeps a translate-only layer bit-identical
                            // to the previous primitive.
                            const auto fullCenterX =
                                static_cast<double>(plan->format().width()) / 2.0;
                            const auto fullCenterY =
                                static_cast<double>(plan->format().height()) / 2.0;
                            render::LayerTransform::Authored authored{
                                .translationX = position->value.x - fullCenterX,
                                .translationY = position->value.y - fullCenterY,
                                .anchorX = anchor->value.x,
                                .anchorY = anchor->value.y,
                                .scaleX = scale->value.x,
                                .scaleY = scale->value.y,
                                .rotationDegrees = rotation->value,
                                .opacity = opacity->value,
                            };
                            // Checked here as well as inside the transform so the diagnostic can
                            // name the position parameter: a finite position can still scale past
                            // the representable range on a large proxy.
                            if (!std::isfinite(authored.translationX * resolved.horizontalScale) ||
                                !std::isfinite(authored.translationY * resolved.verticalScale)) {
                                operationFailure = diagnostic(
                                    EvaluationDiagnosticCode::InvalidParameter,
                                    "Layer position produces a non-finite translation", {},
                                    detail::parameterSubject(operationSubject, *position,
                                                             "position"));
                                return;
                            }
                            if (!slots[layer.input.value()])
                                return;
                            auto sourceView = slots[layer.input.value()]->view();
                            if (!sourceView) {
                                operationFailure =
                                    imageDiagnostic(*sourceView.error(), operationSubject,
                                                    "Layer source image is unavailable");
                                return;
                            }
                            const auto sourceDescriptor = sourceView.value()->descriptor();
                            if (!sourceDescriptor.has_value()) {
                                operationFailure = diagnostic(
                                    EvaluationDiagnosticCode::InternalInvariant,
                                    "Layer source image has no descriptor", {}, operationSubject);
                                return;
                            }
                            auto& geometry = bounds[index];
                            geometry.layerId = layer.layerId;
                            geometry.local = bounds[layer.input.value()].output;
                            if (geometry.local.empty())
                                return;
                            if (layer.localBounds) {
                                const auto centre = geometry.local.centre();
                                const auto window = sourceDescriptor->dataWindow();
                                const auto bufferCentre =
                                    detail::boundsForWindow(window, resolved.horizontalScale,
                                                            resolved.verticalScale)
                                        .centre();
                                authored.translationX =
                                    position->value.x - centre.x - anchor->value.x;
                                authored.translationY =
                                    position->value.y - centre.y - anchor->value.y;
                                authored.anchorX = centre.x - bufferCentre.x + anchor->value.x;
                                authored.anchorY = centre.y - bufferCentre.y + anchor->value.y;
                            }
                            // A scale factor of exactly zero collapses the layer to no area at all.
                            // That is an authorable value -- a scale curve starting from nothing --
                            // not an error, so the layer simply contributes no pixels and publishes
                            // no image, exactly as an entirely off-frame layer does below.
                            if (authored.scaleX == 0.0 || authored.scaleY == 0.0) {
                                reportProgress(progress,
                                               {.stage = EvaluationProgressStage::Operation,
                                                .operation = operationIndex,
                                                .completed = 1,
                                                .total = 1});
                                return;
                            }
                            const auto transform = render::LayerTransform::create(
                                authored, sourceDescriptor->dataWindow(), resolved.horizontalScale,
                                resolved.verticalScale);
                            if (!transform) {
                                operationFailure =
                                    imageDiagnostic(*transform.error(), operationSubject,
                                                    "Layer transform parameters are not evaluable");
                                return;
                            }
                            // Local content remains available outside the composition so a parent
                            // transform can bring it back. Compatibility layers retain frame
                            // clipping.
                            const auto sourceWindow = sourceDescriptor->dataWindow();
                            const auto map = [&](const document::Vec2d point) {
                                const auto mapped = transform.value()->forwardMap(
                                    point.x * resolved.horizontalScale - 0.5 -
                                        static_cast<double>(sourceWindow.originX()),
                                    point.y * resolved.verticalScale - 0.5 -
                                        static_cast<double>(sourceWindow.originY()));
                                return document::Vec2d{(mapped.x + 0.5) / resolved.horizontalScale,
                                                       (mapped.y + 0.5) / resolved.verticalScale};
                            };
                            geometry.polygon = detail::boundsCorners(geometry.local);
                            for (auto& point : geometry.polygon)
                                point = map(point);
                            geometry.output = {geometry.polygon[0].x, geometry.polygon[0].y,
                                               geometry.polygon[0].x, geometry.polygon[0].y};
                            for (const auto point : geometry.polygon) {
                                geometry.output.left = std::min(geometry.output.left, point.x);
                                geometry.output.top = std::min(geometry.output.top, point.y);
                                geometry.output.right = std::max(geometry.output.right, point.x);
                                geometry.output.bottom = std::max(geometry.output.bottom, point.y);
                            }
                            const auto centre = layer.localBounds
                                                    ? geometry.local.centre()
                                                    : detail::boundsForWindow(
                                                          sourceWindow, resolved.horizontalScale,
                                                          resolved.verticalScale)
                                                          .centre();
                            geometry.anchor =
                                map({centre.x + anchor->value.x, centre.y + anchor->value.y});
                            if (layer.localBounds) {
                                constexpr double limit = 16777214.0;
                                const double right = sourceWindow.extent().width();
                                const double bottom = sourceWindow.extent().height();
                                const std::array support{
                                    transform.value()->forwardMap(-1.0, -1.0),
                                    transform.value()->forwardMap(right, -1.0),
                                    transform.value()->forwardMap(right, bottom),
                                    transform.value()->forwardMap(-1.0, bottom)};
                                for (const auto point : support) {
                                    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                                        std::abs(point.x) > limit || std::abs(point.y) > limit) {
                                        operationFailure =
                                            diagnostic(EvaluationDiagnosticCode::InvalidParameter,
                                                       "Transformed content exceeds the supported "
                                                       "coordinate range",
                                                       {}, operationSubject);
                                        return;
                                    }
                                }
                            }
                            const auto compositionWindow = resolved.imageDescriptor.dataWindow();
                            const auto workingWindow = render::ImageWindow::create(
                                -16777216, -16777216, 33554432, 33554432);
                            const auto layerWindow = transform.value()->supportBounds(
                                layer.localBounds ? *workingWindow.value() : compositionWindow);
                            if (!layerWindow.has_value()) {
                                reportProgress(progress,
                                               {.stage = EvaluationProgressStage::Operation,
                                                .operation = operationIndex,
                                                .completed = 1,
                                                .total = 1});
                                return;
                            }
                            const auto layerDescriptor = render::Rgba32fImageDescriptor::create(
                                *layerWindow, resolved.imageDescriptor.displayWindow(),
                                resolved.imageDescriptor.pixelAspect());
                            if (!layerDescriptor) {
                                operationFailure =
                                    imageDiagnostic(*layerDescriptor.error(), operationSubject,
                                                    "Transformed layer descriptor is invalid");
                                return;
                            }
                            auto builder = render::Rgba32fImageBuilder::create(
                                *layerDescriptor.value(), remainingPixelBudget());
                            if (!builder) {
                                operationFailure = imageDiagnostic(
                                    *builder.error(), operationSubject,
                                    "Transformed layer image could not be allocated");
                                return;
                            }
                            const auto height = layerWindow->extent().height();
                            reportRowPassStarted(progress, operationIndex, height);
                            auto& image = *builder.value();
                            const auto& source = *sourceView.value();
                            const auto& layerTransform = *transform.value();
                            const auto outputWindow = *layerWindow;
                            const auto outcome = runRowBandPass(
                                rowBands, cancellation, height, outputWindow.originY(),
                                [&image, &source, &layerTransform, outputWindow,
                                 &operationSubject](const std::int64_t y) -> RowFailure {
                                    auto outputRow = image.row(y);
                                    if (!outputRow) {
                                        return imageDiagnostic(
                                            *outputRow.error(), operationSubject,
                                            "Layer output row could not be addressed");
                                    }
                                    if (const auto rowStatus = render::layerTransformBilinearRow(
                                            source, outputWindow, y, layerTransform,
                                            *outputRow.value())) {
                                        return imageDiagnostic(
                                            *rowStatus, operationSubject,
                                            "Layer transform could not be evaluated");
                                    }
                                    return std::nullopt;
                                });
                            if (outcome.cancelled) {
                                operationCancelled = true;
                                return;
                            }
                            if (outcome.failure.has_value() || outcome.incomplete) {
                                operationFailure = rowPassFailure(outcome, operationSubject);
                                return;
                            }
                            reportRowPassFinished(progress, operationIndex, height);
                            auto frozen = std::move(*builder.value()).freeze();
                            if (!frozen) {
                                operationFailure = imageDiagnostic(
                                    *frozen.error(), operationSubject,
                                    "Transformed layer image could not be published");
                                return;
                            }
                            produced.emplace(std::move(*frozen.value()));
                        },
                        [&](const CompiledMerge& stack) {
                            ContentBounds storage;
                            for (const auto& entry : stack.entries) {
                                bounds[index].local = detail::unionBounds(
                                    bounds[index].local, bounds[entry.input.value()].output);
                                if (slots[entry.input.value()]) {
                                    const auto view = slots[entry.input.value()]->view();
                                    storage = detail::unionBounds(
                                        storage,
                                        detail::boundsForWindow(
                                            view.value()->descriptor()->dataWindow(), 1.0, 1.0));
                                }
                            }
                            bounds[index].output = bounds[index].local;
                            auto descriptor = resolved.imageDescriptor;
                            if (stack.localBounds && !storage.empty()) {
                                const auto window = render::ImageWindow::create(
                                    static_cast<std::int64_t>(storage.left),
                                    static_cast<std::int64_t>(storage.top),
                                    static_cast<std::uint64_t>(storage.right - storage.left),
                                    static_cast<std::uint64_t>(storage.bottom - storage.top));
                                if (!window) {
                                    operationFailure =
                                        imageDiagnostic(*window.error(), operationSubject,
                                                        "Merge bounds are invalid");
                                    return;
                                }
                                const auto local = render::Rgba32fImageDescriptor::create(
                                    *window.value(), descriptor.displayWindow(),
                                    descriptor.pixelAspect());
                                if (!local) {
                                    operationFailure =
                                        imageDiagnostic(*local.error(), operationSubject,
                                                        "Merge descriptor is invalid");
                                    return;
                                }
                                descriptor = *local.value();
                            }
                            auto builder = render::Rgba32fImageBuilder::create(
                                descriptor, remainingPixelBudget(), render::Rgba32f::transparent());
                            if (!builder) {
                                operationFailure =
                                    imageDiagnostic(*builder.error(), operationSubject,
                                                    "Layer Stack image could not be allocated");
                                return;
                            }
                            const auto window = descriptor.dataWindow();
                            const auto height = window.extent().height();
                            const std::uint64_t totalRows =
                                static_cast<std::uint64_t>(height) * stack.entries.size();
                            std::uint64_t completedRows = 0;
                            reportRowPassStarted(progress, operationIndex, totalRows);
                            for (auto entry = stack.entries.rbegin(); entry != stack.entries.rend();
                                 ++entry) {
                                // A Layer Output that published no image is a layer with no
                                // reachable pixels -- carried entirely off the frame, or collapsed
                                // by a zero scale. Compositing nothing over the accumulation is
                                // exactly right, so the entry is skipped rather than treated as a
                                // missing input.
                                if (!slots[entry->input.value()]) {
                                    completedRows += height;
                                    reportProgress(progress,
                                                   {.stage = EvaluationProgressStage::Operation,
                                                    .operation = operationIndex,
                                                    .completed = completedRows,
                                                    .total = totalRows});
                                    continue;
                                }
                                // Only a direct Layer input contributes its blend mode. An elided
                                // reroute can resolve to a Layer operation while remaining a plain
                                // image input, so the slot identity also participates in this
                                // choice.
                                const auto* layerOutput = std::get_if<CompiledLayerOutput>(
                                    &plan->operations()[entry->input.value()]);
                                auto blendMode = core::BlendMode::Normal;
                                if (layerOutput != nullptr && entry->layerId.isValid()) {
                                    // Task DRIVE-1: the mode may be driven, so it is READ here
                                    // rather than copied out of the plan.
                                    const auto resolvedMode =
                                        detail::resolveParameter(*layerOutput, resolved);
                                    if (!resolvedMode.has_value()) {
                                        auto modeSubject = operationSubject;
                                        modeSubject.parameterId = layerOutput->blendModeParameterId;
                                        modeSubject.field = "blend-mode";
                                        operationFailure =
                                            diagnostic(EvaluationDiagnosticCode::InvalidParameter,
                                                       "Layer blend mode could not be resolved", {},
                                                       std::move(modeSubject));
                                        return;
                                    }
                                    blendMode = *resolvedMode;
                                }
                                auto sourceView = slots[entry->input.value()]->view();
                                if (!sourceView) {
                                    operationFailure =
                                        imageDiagnostic(*sourceView.error(), operationSubject,
                                                        "Layer Stack source image is unavailable");
                                    return;
                                }
                                const auto sourceDescriptor = sourceView.value()->descriptor();
                                if (!sourceDescriptor.has_value()) {
                                    operationFailure =
                                        diagnostic(EvaluationDiagnosticCode::InternalInvariant,
                                                   "Layer Stack source image has no descriptor", {},
                                                   operationSubject);
                                    return;
                                }
                                // A layer's data window is its own transformed bounds, always
                                // inside the composition window, so compositing walks only the rows
                                // the layer occupies and writes into the matching span of each
                                // destination row.
                                const auto sourceWindow = sourceDescriptor->dataWindow();
                                const auto firstColumn =
                                    std::max(sourceWindow.originX(), window.originX());
                                const auto lastColumn =
                                    std::min(sourceWindow.maxXExclusive(), window.maxXExclusive());
                                if (lastColumn <= firstColumn)
                                    continue;
                                const auto columnOffset = firstColumn - window.originX();
                                const auto sourceOffset = firstColumn - sourceWindow.originX();
                                const auto columnCount =
                                    static_cast<std::size_t>(lastColumn - firstColumn);
                                // Entries fold in order, bottom to top, because each one composites
                                // over what the ones beneath it left behind. The ROWS of one entry
                                // are independent of each other, so they band; the entries
                                // themselves never can.
                                auto& image = *builder.value();
                                const auto& source = *sourceView.value();
                                const auto outcome = runRowBandPass(
                                    rowBands, cancellation, height, window.originY(),
                                    [&image, &source, sourceWindow, columnOffset, sourceOffset,
                                     columnCount, blendMode,
                                     &operationSubject](const std::int64_t y) -> RowFailure {
                                        if (y < sourceWindow.originY() ||
                                            y >= sourceWindow.maxYExclusive()) {
                                            return std::nullopt;
                                        }
                                        auto sourceRow = source.row(y);
                                        if (!sourceRow) {
                                            return imageDiagnostic(
                                                *sourceRow.error(), operationSubject,
                                                "Layer Stack source row could not be addressed");
                                        }
                                        auto destinationRow = image.row(y);
                                        if (!destinationRow) {
                                            return imageDiagnostic(*destinationRow.error(),
                                                                   operationSubject,
                                                                   "Layer Stack destination row "
                                                                   "could not be addressed");
                                        }
                                        if (const auto rowStatus =
                                                render::blendLinearRec709SceneRow(
                                                    blendMode,
                                                    sourceRow.value()->subspan(
                                                        static_cast<std::size_t>(sourceOffset),
                                                        columnCount),
                                                    destinationRow.value()->subspan(
                                                        static_cast<std::size_t>(columnOffset),
                                                        columnCount))) {
                                            return imageDiagnostic(
                                                *rowStatus, operationSubject,
                                                "Layer Stack blend could not be evaluated");
                                        }
                                        return std::nullopt;
                                    });
                                if (outcome.cancelled) {
                                    operationCancelled = true;
                                    return;
                                }
                                if (outcome.failure.has_value() || outcome.incomplete) {
                                    operationFailure = rowPassFailure(outcome, operationSubject);
                                    return;
                                }
                                completedRows += height;
                                reportProgress(progress,
                                               {.stage = EvaluationProgressStage::Operation,
                                                .operation = operationIndex,
                                                .completed = completedRows,
                                                .total = totalRows});
                            }
                            if (stack.entries.empty()) {
                                reportProgress(progress,
                                               {.stage = EvaluationProgressStage::Operation,
                                                .operation = operationIndex,
                                                .completed = 1,
                                                .total = 1});
                            }
                            auto frozen = std::move(*builder.value()).freeze();
                            if (!frozen) {
                                operationFailure =
                                    imageDiagnostic(*frozen.error(), operationSubject,
                                                    "Layer Stack image could not be published");
                                return;
                            }
                            produced.emplace(std::move(*frozen.value()));
                        },
                        [&](const CompiledCompositionOutput& output) {
                            bounds[index] = bounds[output.input.value()];
                            processImage = slots[output.input.value()];
                            const auto sourceView = processImage->view();
                            const auto sourceWindow =
                                sourceView.value()->descriptor()->dataWindow();
                            const auto destinationWindow = resolved.imageDescriptor.dataWindow();
                            if (sourceWindow != destinationWindow) {
                                auto builder = render::Rgba32fImageBuilder::create(
                                    resolved.imageDescriptor, remainingPixelBudget(),
                                    render::Rgba32f::transparent());
                                if (!builder) {
                                    operationFailure =
                                        imageDiagnostic(*builder.error(), operationSubject,
                                                        "Output image could not be allocated");
                                    return;
                                }
                                const auto left =
                                    std::max(sourceWindow.originX(), destinationWindow.originX());
                                const auto right = std::min(sourceWindow.maxXExclusive(),
                                                            destinationWindow.maxXExclusive());
                                auto& image = *builder.value();
                                const auto& source = *sourceView.value();
                                const auto outcome = runRowBandPass(
                                    rowBands, cancellation, destinationWindow.extent().height(),
                                    destinationWindow.originY(),
                                    [&](const std::int64_t y) -> RowFailure {
                                        if (right <= left || y < sourceWindow.originY() ||
                                            y >= sourceWindow.maxYExclusive())
                                            return std::nullopt;
                                        const auto row = source.row(y);
                                        auto destination = image.row(y);
                                        if (!row || !destination)
                                            return diagnostic(
                                                EvaluationDiagnosticCode::InternalInvariant,
                                                "Output row is unavailable", {}, operationSubject);
                                        const auto sourcePixels = row.value()->subspan(
                                            static_cast<std::size_t>(left - sourceWindow.originX()),
                                            static_cast<std::size_t>(right - left));
                                        std::copy(sourcePixels.begin(), sourcePixels.end(),
                                                  destination.value()->begin() +
                                                      (left - destinationWindow.originX()));
                                        return std::nullopt;
                                    });
                                if (outcome.cancelled) {
                                    operationCancelled = true;
                                    return;
                                }
                                if (outcome.failure || outcome.incomplete) {
                                    operationFailure = rowPassFailure(outcome, operationSubject);
                                    return;
                                }
                                auto frozen = std::move(*builder.value()).freeze();
                                if (!frozen) {
                                    operationFailure =
                                        imageDiagnostic(*frozen.error(), operationSubject,
                                                        "Output image could not be published");
                                    return;
                                }
                                processImage = std::make_shared<const render::Rgba32fImage>(
                                    std::move(*frozen.value()));
                            }
                            slots[output.input.value()].reset();
                            reportProgress(progress, {.stage = EvaluationProgressStage::Operation,
                                                      .operation = operationIndex,
                                                      .completed = 1,
                                                      .total = 1});
                        },
                    },
                    plan->operations()[index]);

                if (operationCancelled || cancellation.isCancellationRequested()) {
                    return EvaluationResult::cancelled();
                }
                if (operationFailure.has_value()) {
                    return EvaluationResult::failed(std::move(*operationFailure));
                }
                if (produced.has_value()) {
                    slots[index] =
                        std::make_shared<const render::Rgba32fImage>(std::move(*produced));
                }
                if (cache)
                    cache->store(
                        key.bytes(), plan->sourceRevision(),
                        {.image = index == request.output.value() ? processImage : slots[index],
                         .values = {},
                         .bounds = bounds[index]});
            }
            if (index != request.output.value()) {
                forEachInput(plan->operations()[index], [&](const OperationIndex input) {
                    auto& remaining = resolved.remainingConsumers[input.value()];
                    --remaining;
                    if (remaining == 0) {
                        slots[input.value()].reset();
                    }
                });
            }
        }

        if (!processImage || cancellation.isCancellationRequested()) {
            return cancellation.isCancellationRequested()
                       ? EvaluationResult::cancelled()
                       : EvaluationResult::failed(
                             diagnostic(EvaluationDiagnosticCode::InternalInvariant,
                                        "Composition Output did not publish a process image"));
        }
        const auto animationSamplingSemanticsVersion = plan->animationSamplingSemanticsVersion();
        ProcessFrameIdentity identity{
            .plan = std::move(plan),
            .time = request.time,
            .output = request.output,
            .resolution = request.resolution,
            .quality = request.quality,
            .colorIntent = request.colorIntent,
            .provider = EvaluationProvider::CpuReference,
            .evaluatorSemanticsVersion = kCpuCompositionEvaluatorSemanticsVersion,
            .animationSamplingSemanticsVersion = animationSamplingSemanticsVersion,
            .imagePrimitiveSemanticsVersion = render::kCpuImagePrimitiveSemanticsVersion,
        };
        auto frame = std::shared_ptr<const ProcessFrame>(new ProcessFrame(
            std::move(identity), std::move(processImage), frameStatistics, std::move(bounds)));
        if (statistics)
            *statistics = std::move(frameStatistics);
        return EvaluationResult::evaluated(std::move(frame), std::move(imageWarnings));
    } catch (const std::bad_alloc&) {
        return unexpectedAllocationFailure();
    } catch (const std::length_error&) {
        return unexpectedAllocationFailure();
    }
}

} // namespace bloom::runtime
