#include <bloom/runtime/value_graph_evaluation.hpp>

#include <bloom/core/scalar_primitives.hpp>
#include <bloom/core/value_primitives.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using namespace bloom;
using runtime::CompiledValue;
using runtime::CompiledValueOperand;
using runtime::ValueGraphDiagnostic;
using runtime::ValueOutputIndex;

using Scalar = core::primitives::ScalarPrimitive;

[[nodiscard]] std::optional<std::int64_t> checkedMultiply(const std::int64_t left,
                                                          const std::int64_t right) noexcept {
    if (left == 0 || right == 0) {
        return std::int64_t{0};
    }
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    // Magnitudes only: both operands are already known non-zero, and the sign is reattached by the
    // multiplication itself. The most negative value has no positive magnitude, so it is refused
    // rather than negated.
    constexpr auto lowest = std::numeric_limits<std::int64_t>::min();
    if (left == lowest || right == lowest) {
        return std::nullopt;
    }
    const auto leftMagnitude = left < 0 ? -left : left;
    const auto rightMagnitude = right < 0 ? -right : right;
    if (leftMagnitude > limit / rightMagnitude) {
        return std::nullopt;
    }
    return left * right;
}

// Floor division over exact integers. std::int64_t division truncates toward zero, which would put
// the frame before time zero one frame too late.
[[nodiscard]] std::int64_t floorDivide(const std::int64_t numerator,
                                       const std::int64_t denominator) noexcept {
    const auto quotient = numerator / denominator;
    const auto remainder = numerator % denominator;
    const bool negative = (remainder != 0) && ((remainder < 0) != (denominator < 0));
    return negative ? quotient - 1 : quotient;
}

// The one place a value graph's operand is read. A constant answers with itself; an output
// reference answers with the entry already written for it. An index past what has been written
// cannot happen in a well-formed plan -- the operations are topologically ordered -- so nullptr
// here means the plan is malformed and the caller reports it.
[[nodiscard]] const CompiledValue* read(const CompiledValueOperand& operand,
                                        const std::vector<CompiledValue>& outputs,
                                        const std::size_t written) {
    if (const auto* constant = std::get_if<CompiledValue>(&operand.source)) {
        return constant;
    }
    const auto index = std::get<ValueOutputIndex>(operand.source).value();
    if (index >= written || index >= outputs.size()) {
        return nullptr;
    }
    return &outputs[index];
}

// Strict readers. A Boolean is NOT silently a number and an Integer is NOT silently a Scalar: every
// widening the connect-time whitelist allows became its own CompiledValuePromotion operation, so
// anything arriving here with the wrong alternative means the plan disagrees with the document's
// own typing and is reported rather than coerced.
[[nodiscard]] const double* asScalar(const CompiledValue* value) {
    return value == nullptr ? nullptr : std::get_if<double>(value);
}
[[nodiscard]] const std::int64_t* asInteger(const CompiledValue* value) {
    return value == nullptr ? nullptr : std::get_if<std::int64_t>(value);
}
[[nodiscard]] const bool* asBoolean(const CompiledValue* value) {
    return value == nullptr ? nullptr : std::get_if<bool>(value);
}
[[nodiscard]] const core::Color4d* asColor(const CompiledValue* value) {
    return value == nullptr ? nullptr : std::get_if<core::Color4d>(value);
}

// A vector operand of either width, as its components.
[[nodiscard]] std::optional<std::array<double, 3>> asVector(const CompiledValue* value,
                                                            const std::uint8_t components) {
    if (value == nullptr) {
        return std::nullopt;
    }
    if (components == 2) {
        const auto* vector = std::get_if<document::Vec2d>(value);
        return vector == nullptr ? std::nullopt
                                 : std::optional(std::array<double, 3>{vector->x, vector->y, 0.0});
    }
    const auto* vector = std::get_if<document::Vec3d>(value);
    return vector == nullptr
               ? std::nullopt
               : std::optional(std::array<double, 3>{vector->x, vector->y, vector->z});
}

[[nodiscard]] CompiledValue vectorValue(const std::array<double, 3>& components,
                                        const std::uint8_t width) {
    if (width == 2) {
        return document::Vec2d{components[0], components[1]};
    }
    return document::Vec3d{components[0], components[1], components[2]};
}

// The result of one scalar kernel call, with the tranche's domain failure already turned into this
// node's documented fallback.
struct ScalarOutcome final {
    double value = 0.0;
    bool failed = false;
};

[[nodiscard]] ScalarOutcome evaluate(const Scalar primitive, const std::span<const double> inputs,
                                     const double fallback) {
    const auto result = core::primitives::evaluateScalar(primitive, inputs);
    if (!result.hasValue()) {
        return {fallback, true};
    }
    return {*result.value(), false};
}

class Evaluator final {
  public:
    Evaluator(const std::size_t outputCount, const core::RationalTime time,
              const document::FrameRate rate)
        : time_(time), rate_(rate) {
        outputs_.assign(outputCount, CompiledValue{0.0});
    }

    void run(const std::span<const runtime::CompiledValueOperation> operations) {
        for (const auto& operation : operations) {
            const auto first = operation.firstOutput.value();
            const auto count = static_cast<std::size_t>(operation.outputCount);
            if (count == 0 || first != written_ || first + count > outputs_.size()) {
                // The operations partition the output table in order; anything else is a malformed
                // plan, and writing into it would corrupt a later operation's operands.
                fail(operation, {}, "Value operation claims an invalid output range",
                     "Value operations must partition the plan's output table in order.");
                return;
            }
            evaluateOperation(operation);
            written_ = first + count;
        }
    }

    [[nodiscard]] runtime::ValueGraphEvaluation release() {
        return {std::move(outputs_), std::move(diagnostics_)};
    }

  private:
    void fail(const runtime::CompiledValueOperation& operation,
              const document::ParameterId parameterId, std::string summary, std::string detail) {
        diagnostics_.push_back({operation.sourceNodeId, parameterId, operation.firstOutput,
                                std::move(summary), std::move(detail)});
    }

    [[nodiscard]] const CompiledValue* operandOf(const CompiledValueOperand& operand) const {
        return read(operand, outputs_, written_);
    }

    void write(const runtime::CompiledValueOperation& operation, const std::size_t slot,
               CompiledValue value) {
        outputs_[operation.firstOutput.value() + slot] = std::move(value);
    }

    void evaluateOperation(const runtime::CompiledValueOperation& operation);
    void evaluatePassthrough(const runtime::CompiledValueOperation& operation,
                             const runtime::CompiledValuePassthrough& kernel);
    void evaluateTime(const runtime::CompiledValueOperation& operation);
    void evaluateScalarMath(const runtime::CompiledValueOperation& operation,
                            const runtime::CompiledValueScalarMath& kernel);
    void evaluateVectorMath(const runtime::CompiledValueOperation& operation,
                            const runtime::CompiledValueVectorMath& kernel);
    void evaluateVectorReduce(const runtime::CompiledValueOperation& operation,
                              const runtime::CompiledValueVectorReduce& kernel);
    void evaluateMapRange(const runtime::CompiledValueOperation& operation,
                          const runtime::CompiledValueMapRange& kernel);
    void evaluateClamp(const runtime::CompiledValueOperation& operation,
                       const runtime::CompiledValueClamp& kernel);
    void evaluateMix(const runtime::CompiledValueOperation& operation,
                     const runtime::CompiledValueMix& kernel);
    void evaluateCompare(const runtime::CompiledValueOperation& operation,
                         const runtime::CompiledValueCompare& kernel);
    void evaluateSwitch(const runtime::CompiledValueOperation& operation,
                        const runtime::CompiledValueSwitch& kernel);
    void evaluateSeparate(const runtime::CompiledValueOperation& operation,
                          const runtime::CompiledValueSeparate& kernel);
    void evaluateCombine(const runtime::CompiledValueOperation& operation,
                         const runtime::CompiledValueCombine& kernel);
    void evaluateRandom(const runtime::CompiledValueOperation& operation,
                        const runtime::CompiledValueRandom& kernel);
    void evaluatePromotion(const runtime::CompiledValueOperation& operation,
                           const runtime::CompiledValuePromotion& kernel);

    core::RationalTime time_;
    document::FrameRate rate_;
    std::vector<CompiledValue> outputs_;
    std::vector<ValueGraphDiagnostic> diagnostics_;
    std::size_t written_ = 0;
};

void Evaluator::evaluateOperation(const runtime::CompiledValueOperation& operation) {
    std::visit(
        [&](const auto& kernel) {
            using Kernel = std::decay_t<decltype(kernel)>;
            if constexpr (std::is_same_v<Kernel, runtime::CompiledValuePassthrough>) {
                evaluatePassthrough(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueTime>) {
                evaluateTime(operation);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueScalarMath>) {
                evaluateScalarMath(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueVectorMath>) {
                evaluateVectorMath(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueVectorReduce>) {
                evaluateVectorReduce(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueMapRange>) {
                evaluateMapRange(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueClamp>) {
                evaluateClamp(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueMix>) {
                evaluateMix(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueCompare>) {
                evaluateCompare(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueSwitch>) {
                evaluateSwitch(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueSeparate>) {
                evaluateSeparate(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueCombine>) {
                evaluateCombine(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueRandom>) {
                evaluateRandom(operation, kernel);
            } else {
                evaluatePromotion(operation, kernel);
            }
        },
        operation.kernel);
}

void Evaluator::evaluatePassthrough(const runtime::CompiledValueOperation& operation,
                                    const runtime::CompiledValuePassthrough& kernel) {
    const auto* value = operandOf(kernel.value);
    if (value == nullptr) {
        fail(operation, kernel.value.id, "Value operand is unavailable",
             "The operand names a value-graph output that has not been computed.");
        return;
    }
    write(operation, 0, *value);
}

void Evaluator::evaluateTime(const runtime::CompiledValueOperation& operation) {
    write(operation, 0, time_.toSeconds());
    const auto frame = runtime::valueGraphFrameIndex(time_, rate_);
    if (!frame.has_value()) {
        // Fallback zero plus a diagnostic, not a frame number nobody can name: the product
        // overflowed the exact integer range, which means the request time is outside anything a
        // frame index could address.
        fail(operation, {}, "Frame number is not representable at this time",
             "The request time multiplied by the frame rate overflows an exact integer.");
        write(operation, 1, std::int64_t{0});
        return;
    }
    write(operation, 1, *frame);
}

void Evaluator::evaluateScalarMath(const runtime::CompiledValueOperation& operation,
                                   const runtime::CompiledValueScalarMath& kernel) {
    std::array<double, document::kMaximumScalarOperationOperands> inputs{};
    if (kernel.operands.size() > inputs.size()) {
        fail(operation, {}, "Math operation has too many operands",
             "No scalar primitive reads more operands than the vocabulary's widest.");
        write(operation, 0, 0.0);
        return;
    }
    for (std::size_t index = 0; index < kernel.operands.size(); ++index) {
        const auto* value = asScalar(operandOf(kernel.operands[index]));
        if (value == nullptr) {
            fail(operation, kernel.operands[index].id, "Math operand is not a scalar",
                 "Every Math operand must reach the kernel as a Scalar.");
            write(operation, 0, 0.0);
            return;
        }
        inputs[index] = *value;
    }
    // Zero is the documented fallback for every scalar domain failure, which is also Blender's
    // divide-by-zero answer -- so the most common failure an artist will actually hit behaves the
    // way they expect, and the rest behave consistently with it.
    auto outcome =
        evaluate(kernel.operation, std::span(inputs.data(), kernel.operands.size()), 0.0);
    if (outcome.failed) {
        fail(operation, {}, "Math operation is outside its domain",
             "The operation's fallback value of zero was substituted.");
    }
    if (kernel.clampResult) {
        const std::array<double, 3> clampInputs{outcome.value, 0.0, 1.0};
        outcome = evaluate(Scalar::Clamp, clampInputs, 0.0);
    }
    write(operation, 0, outcome.value);
}

void Evaluator::evaluateVectorMath(const runtime::CompiledValueOperation& operation,
                                   const runtime::CompiledValueVectorMath& kernel) {
    const auto width = kernel.components;
    const auto left = asVector(operandOf(kernel.left), width);
    const auto right = asVector(operandOf(kernel.right), width);
    const std::array<double, 3> zero{};
    if (!left.has_value() || !right.has_value()) {
        fail(operation, kernel.left.id, "Vector Math operand has the wrong width",
             "Both operands must reach the kernel as vectors of the node's own width.");
        write(operation, 0, vectorValue(zero, width));
        return;
    }
    std::array<double, 3> result{};
    bool failed = false;
    const auto componentwise = [&](const Scalar primitive) {
        for (std::uint8_t component = 0; component < width; ++component) {
            const std::array<double, 2> inputs{(*left)[component], (*right)[component]};
            const auto outcome = evaluate(primitive, inputs, 0.0);
            result[component] = outcome.value;
            failed = failed || outcome.failed;
        }
    };
    switch (kernel.operation) {
    case document::VectorOperation::Add:
        componentwise(Scalar::Add);
        break;
    case document::VectorOperation::Subtract:
        componentwise(Scalar::Subtract);
        break;
    case document::VectorOperation::Multiply:
        componentwise(Scalar::Multiply);
        break;
    case document::VectorOperation::Divide:
        componentwise(Scalar::Divide);
        break;
    case document::VectorOperation::Scale: {
        const auto* factor = asScalar(operandOf(kernel.factor));
        if (factor == nullptr) {
            fail(operation, kernel.factor.id, "Scale factor is not a scalar",
                 "The scale operand must reach the kernel as a Scalar.");
            write(operation, 0, vectorValue(zero, width));
            return;
        }
        for (std::uint8_t component = 0; component < width; ++component) {
            const std::array<double, 2> inputs{(*left)[component], *factor};
            const auto outcome = evaluate(Scalar::Multiply, inputs, 0.0);
            result[component] = outcome.value;
            failed = failed || outcome.failed;
        }
        break;
    }
    case document::VectorOperation::Normalize:
    case document::VectorOperation::CrossProduct: {
        const auto primitive = kernel.operation == document::VectorOperation::Normalize
                                   ? core::primitives::VectorPrimitive::Normalize
                                   : core::primitives::VectorPrimitive::CrossProduct;
        std::array<double, 6> inputs{};
        std::copy_n(left->begin(), width, inputs.begin());
        std::copy_n(right->begin(), width, inputs.begin() + width);
        const auto operandCount =
            static_cast<std::size_t>(width) *
            (primitive == core::primitives::VectorPrimitive::Normalize ? 1U : 2U);
        const auto evaluated = core::primitives::evaluateVector(
            primitive, width, std::span(inputs.data(), operandCount));
        if (!evaluated.hasValue()) {
            // A zero-length vector has no direction, so the documented fallback is the zero vector
            // itself rather than an arbitrary axis.
            failed = true;
        } else {
            std::copy_n(evaluated.components().begin(), width, result.begin());
        }
        break;
    }
    }
    if (failed) {
        fail(operation, {}, "Vector Math operation is outside its domain",
             "The operation's fallback of zero in the affected components was substituted.");
    }
    write(operation, 0, vectorValue(result, width));
}

void Evaluator::evaluateVectorReduce(const runtime::CompiledValueOperation& operation,
                                     const runtime::CompiledValueVectorReduce& kernel) {
    const auto width = kernel.components;
    const auto left = asVector(operandOf(kernel.left), width);
    const auto right = asVector(operandOf(kernel.right), width);
    if (!left.has_value() || !right.has_value()) {
        fail(operation, kernel.left.id, "Vector operand has the wrong width",
             "Both operands must reach the kernel as vectors of the node's own width.");
        write(operation, 0, 0.0);
        return;
    }
    const auto primitive = document::vectorReductionPrimitive(kernel.reduction);
    const auto* signature = core::primitives::vectorPrimitiveSignature(primitive);
    std::array<double, 6> inputs{};
    std::copy_n(left->begin(), width, inputs.begin());
    std::copy_n(right->begin(), width, inputs.begin() + width);
    const auto operandCount =
        signature == nullptr ? 0 : static_cast<std::size_t>(width) * signature->vectorOperandCount;
    const auto evaluated =
        core::primitives::evaluateVector(primitive, width, std::span(inputs.data(), operandCount));
    if (!evaluated.hasValue()) {
        fail(operation, {}, "Vector reduction is outside its domain",
             "The reduction's fallback value of zero was substituted.");
        write(operation, 0, 0.0);
        return;
    }
    write(operation, 0, evaluated.components()[0]);
}

void Evaluator::evaluateMapRange(const runtime::CompiledValueOperation& operation,
                                 const runtime::CompiledValueMapRange& kernel) {
    const auto* value = asScalar(operandOf(kernel.value));
    const auto* fromMinimum = asScalar(operandOf(kernel.fromMinimum));
    const auto* fromMaximum = asScalar(operandOf(kernel.fromMaximum));
    const auto* toMinimum = asScalar(operandOf(kernel.toMinimum));
    const auto* toMaximum = asScalar(operandOf(kernel.toMaximum));
    if (value == nullptr || fromMinimum == nullptr || fromMaximum == nullptr ||
        toMinimum == nullptr || toMaximum == nullptr) {
        fail(operation, kernel.value.id, "Map Range operand is not a scalar",
             "Every Map Range operand must reach the kernel as a Scalar.");
        write(operation, 0, 0.0);
        return;
    }
    // A degenerate source range has no factor at all, so the documented fallback is the destination
    // minimum: the answer every value in that range would map to if the range had any width.
    const double fallback = *toMinimum;
    ScalarOutcome outcome{fallback, false};
    if (kernel.interpolation == document::RangeInterpolation::Linear) {
        const std::array<double, 5> inputs{*value, *fromMinimum, *fromMaximum, *toMinimum,
                                           *toMaximum};
        outcome = evaluate(Scalar::Remap, inputs, fallback);
    } else {
        const auto primitive = kernel.interpolation == document::RangeInterpolation::Smoothstep
                                   ? Scalar::Smoothstep
                                   : Scalar::Smootherstep;
        const std::array<double, 3> edges{*fromMinimum, *fromMaximum, *value};
        const auto factor = evaluate(primitive, edges, 0.0);
        if (factor.failed) {
            outcome = {fallback, true};
        } else {
            const std::array<double, 3> inputs{*toMinimum, *toMaximum, factor.value};
            outcome = evaluate(Scalar::Mix, inputs, fallback);
        }
    }
    if (outcome.failed) {
        fail(operation, {}, "Map Range is outside its domain",
             "The destination minimum was substituted for a degenerate source range.");
    }
    if (kernel.clampResult) {
        // Clamped to the DESTINATION range, in whichever order it was authored: a reversed To range
        // is a legitimate inversion, not a mistake to be corrected.
        const std::array<double, 3> inputs{outcome.value, std::min(*toMinimum, *toMaximum),
                                           std::max(*toMinimum, *toMaximum)};
        outcome = evaluate(Scalar::Clamp, inputs, outcome.value);
    }
    write(operation, 0, outcome.value);
}

void Evaluator::evaluateClamp(const runtime::CompiledValueOperation& operation,
                              const runtime::CompiledValueClamp& kernel) {
    const auto* value = asScalar(operandOf(kernel.value));
    const auto* minimum = asScalar(operandOf(kernel.minimum));
    const auto* maximum = asScalar(operandOf(kernel.maximum));
    if (value == nullptr || minimum == nullptr || maximum == nullptr) {
        fail(operation, kernel.value.id, "Clamp operand is not a scalar",
             "Every Clamp operand must reach the kernel as a Scalar.");
        write(operation, 0, 0.0);
        return;
    }
    const std::array<double, 3> inputs{*value, *minimum, *maximum};
    // Reversed bounds name no interval, so the documented fallback is the value UNCLAMPED: passing
    // it through is visibly wrong in the picture, where silently swapping the bounds would look
    // correct and hide the authoring mistake.
    const auto outcome = evaluate(Scalar::Clamp, inputs, *value);
    if (outcome.failed) {
        fail(operation, {}, "Clamp bounds are reversed",
             "The unclamped value was substituted for an inverted bound pair.");
    }
    write(operation, 0, outcome.value);
}

void Evaluator::evaluateMix(const runtime::CompiledValueOperation& operation,
                            const runtime::CompiledValueMix& kernel) {
    const auto* factor = asScalar(operandOf(kernel.factor));
    if (factor == nullptr) {
        fail(operation, kernel.factor.id, "Mix factor is not a scalar",
             "The factor operand must reach the kernel as a Scalar.");
        write(operation, 0, kernel.color ? CompiledValue{core::Color4d{}} : CompiledValue{0.0});
        return;
    }
    if (!kernel.color) {
        const auto* start = asScalar(operandOf(kernel.start));
        const auto* end = asScalar(operandOf(kernel.end));
        if (start == nullptr || end == nullptr) {
            fail(operation, kernel.start.id, "Mix operand is not a scalar",
                 "Both Mix endpoints must reach the kernel as Scalars.");
            write(operation, 0, 0.0);
            return;
        }
        const std::array<double, 3> inputs{*start, *end, *factor};
        const auto outcome = evaluate(Scalar::Mix, inputs, *start);
        if (outcome.failed) {
            fail(operation, {}, "Mix is outside its domain", "The start value was substituted.");
        }
        write(operation, 0, outcome.value);
        return;
    }
    const auto* start = asColor(operandOf(kernel.start));
    const auto* end = asColor(operandOf(kernel.end));
    if (start == nullptr || end == nullptr) {
        fail(operation, kernel.start.id, "Mix operand is not a color",
             "Both Mix endpoints must reach the kernel as Colors.");
        write(operation, 0, core::Color4d{});
        return;
    }
    // Four independent straight-value interpolations, alpha included and alpha LAST -- not a vector
    // mix of four floats. A Color is not a Vec4, and nothing here implies a gamut or OCIO step.
    const std::array<std::pair<double, double>, 4> channels{
        std::pair{start->red, end->red}, std::pair{start->green, end->green},
        std::pair{start->blue, end->blue}, std::pair{start->alpha, end->alpha}};
    std::array<double, 4> mixed{};
    bool failed = false;
    for (std::size_t channel = 0; channel < channels.size(); ++channel) {
        const std::array<double, 3> inputs{channels[channel].first, channels[channel].second,
                                           *factor};
        const auto outcome = evaluate(Scalar::Mix, inputs, channels[channel].first);
        mixed[channel] = outcome.value;
        failed = failed || outcome.failed;
    }
    if (failed) {
        fail(operation, {}, "Color Mix is outside its domain",
             "The start color's affected channels were substituted.");
    }
    write(operation, 0, core::Color4d{mixed[0], mixed[1], mixed[2], mixed[3]});
}

void Evaluator::evaluateCompare(const runtime::CompiledValueOperation& operation,
                                const runtime::CompiledValueCompare& kernel) {
    const auto* left = asScalar(operandOf(kernel.left));
    const auto* right = asScalar(operandOf(kernel.right));
    const auto* epsilon = asScalar(operandOf(kernel.epsilon));
    if (left == nullptr || right == nullptr || epsilon == nullptr) {
        fail(operation, kernel.left.id, "Compare operand is not a scalar",
             "Every Compare operand must reach the kernel as a Scalar.");
        write(operation, 0, false);
        return;
    }
    const double difference = std::abs(*left - *right);
    const bool within = difference <= *epsilon;
    bool result = false;
    switch (kernel.operation) {
    case document::CompareOperation::Equal:
        result = within;
        break;
    case document::CompareOperation::NotEqual:
        result = !within;
        break;
    case document::CompareOperation::Less:
        result = *left < *right;
        break;
    case document::CompareOperation::LessOrEqual:
        result = *left <= *right;
        break;
    case document::CompareOperation::Greater:
        result = *left > *right;
        break;
    case document::CompareOperation::GreaterOrEqual:
        result = *left >= *right;
        break;
    }
    write(operation, 0, result);
}

void Evaluator::evaluateSwitch(const runtime::CompiledValueOperation& operation,
                               const runtime::CompiledValueSwitch& kernel) {
    const auto* condition = asBoolean(operandOf(kernel.condition));
    if (condition == nullptr) {
        fail(operation, kernel.condition.id, "Switch condition is not a boolean",
             "The condition operand must reach the kernel as a Boolean.");
    }
    const auto& chosen = (condition != nullptr && *condition) ? kernel.ifTrue : kernel.ifFalse;
    const auto* value = operandOf(chosen);
    if (value == nullptr) {
        fail(operation, chosen.id, "Switch branch is unavailable",
             "The selected branch names a value-graph output that has not been computed.");
        return;
    }
    write(operation, 0, *value);
}

void Evaluator::evaluateSeparate(const runtime::CompiledValueOperation& operation,
                                 const runtime::CompiledValueSeparate& kernel) {
    const auto* value = operandOf(kernel.value);
    if (kernel.color) {
        const auto* color = asColor(value);
        if (color == nullptr) {
            fail(operation, kernel.value.id, "Separate operand is not a color",
                 "The operand must reach the kernel as a Color.");
            for (std::size_t slot = 0; slot < kernel.componentCount; ++slot) {
                write(operation, slot, 0.0);
            }
            return;
        }
        const std::array<double, 4> channels{color->red, color->green, color->blue, color->alpha};
        for (std::size_t slot = 0; slot < kernel.componentCount && slot < channels.size(); ++slot) {
            write(operation, slot, channels[slot]);
        }
        return;
    }
    const auto components = asVector(value, kernel.componentCount);
    if (!components.has_value()) {
        fail(operation, kernel.value.id, "Separate operand has the wrong width",
             "The operand must reach the kernel as a vector of the node's own width.");
        for (std::size_t slot = 0; slot < kernel.componentCount; ++slot) {
            write(operation, slot, 0.0);
        }
        return;
    }
    for (std::size_t slot = 0; slot < kernel.componentCount; ++slot) {
        write(operation, slot, (*components)[slot]);
    }
}

void Evaluator::evaluateCombine(const runtime::CompiledValueOperation& operation,
                                const runtime::CompiledValueCombine& kernel) {
    std::array<double, 4> components{};
    for (std::size_t index = 0; index < kernel.components.size() && index < components.size();
         ++index) {
        const auto* value = asScalar(operandOf(kernel.components[index]));
        if (value == nullptr) {
            fail(operation, kernel.components[index].id, "Combine operand is not a scalar",
                 "Every Combine component must reach the kernel as a Scalar.");
            const std::array<double, 3> zero{};
            write(operation, 0,
                  kernel.color
                      ? CompiledValue{core::Color4d{}}
                      : vectorValue(zero, static_cast<std::uint8_t>(kernel.components.size())));
            return;
        }
        components[index] = *value;
    }
    if (kernel.color) {
        write(operation, 0,
              core::Color4d{components[0], components[1], components[2], components[3]});
        return;
    }
    const std::array<double, 3> assembled{components[0], components[1], components[2]};
    write(operation, 0,
          vectorValue(assembled, static_cast<std::uint8_t>(kernel.components.size())));
}

void Evaluator::evaluateRandom(const runtime::CompiledValueOperation& operation,
                               const runtime::CompiledValueRandom& kernel) {
    const auto* seed = asInteger(operandOf(kernel.seed));
    const auto* minimum = asScalar(operandOf(kernel.minimum));
    const auto* maximum = asScalar(operandOf(kernel.maximum));
    if (seed == nullptr || minimum == nullptr || maximum == nullptr) {
        fail(operation, kernel.seed.id, "Random operand has the wrong kind",
             "The seed must reach the kernel as an Integer and the bounds as Scalars.");
        write(operation, 0, 0.0);
        return;
    }
    const double unit = core::primitives::hashUnitValue(*seed);
    const std::array<double, 3> inputs{*minimum, *maximum, unit};
    // The hashed position is in [0, 1), so the result is in the half-open span [min, max) -- the
    // bound convention every sampling API uses, so a seeded value never lands exactly on the
    // maximum.
    const auto outcome = evaluate(Scalar::Mix, inputs, *minimum);
    if (outcome.failed) {
        fail(operation, {}, "Random bounds are outside the representable range",
             "The minimum bound was substituted.");
    }
    write(operation, 0, outcome.value);
}

void Evaluator::evaluatePromotion(const runtime::CompiledValueOperation& operation,
                                  const runtime::CompiledValuePromotion& kernel) {
    const auto* value = operandOf(kernel.value);
    const auto reject = [&](CompiledValue fallback) {
        fail(operation, kernel.value.id, "Promotion operand has the wrong kind",
             "A promotion widens exactly one kind into exactly one other.");
        write(operation, 0, std::move(fallback));
    };
    switch (kernel.promotion) {
    case runtime::ValuePromotion::IntegerToScalar: {
        const auto* integer = asInteger(value);
        if (integer == nullptr) {
            reject(0.0);
            return;
        }
        // Exact for every magnitude a document can hold below 2^53; beyond that the nearest double
        // is the only answer binary64 has, and the widening is still total rather than failing.
        write(operation, 0, static_cast<double>(*integer));
        return;
    }
    case runtime::ValuePromotion::BooleanToInteger: {
        const auto* boolean = asBoolean(value);
        if (boolean == nullptr) {
            reject(std::int64_t{0});
            return;
        }
        write(operation, 0, *boolean ? std::int64_t{1} : std::int64_t{0});
        return;
    }
    case runtime::ValuePromotion::BooleanToScalar: {
        const auto* boolean = asBoolean(value);
        if (boolean == nullptr) {
            reject(0.0);
            return;
        }
        write(operation, 0, *boolean ? 1.0 : 0.0);
        return;
    }
    case runtime::ValuePromotion::ScalarToVector2:
    case runtime::ValuePromotion::ScalarToVector3: {
        const bool wide = kernel.promotion == runtime::ValuePromotion::ScalarToVector3;
        const auto* scalar = asScalar(value);
        if (scalar == nullptr) {
            reject(wide ? CompiledValue{document::Vec3d{}} : CompiledValue{document::Vec2d{}});
            return;
        }
        // Splat: the one value in every component. A vector built from a single number has no other
        // defensible reading.
        if (wide) {
            write(operation, 0, document::Vec3d{*scalar, *scalar, *scalar});
        } else {
            write(operation, 0, document::Vec2d{*scalar, *scalar});
        }
        return;
    }
    }
}

} // namespace

namespace bloom::runtime {

std::optional<std::int64_t> valueGraphFrameIndex(const core::RationalTime time,
                                                 const document::FrameRate rate) noexcept {
    // Reduce across the fraction bar first: it is what keeps an ordinary frame-aligned time at an
    // ordinary rate far away from the overflow check below, rather than relying on the magnitudes
    // happening to be small.
    auto numerator = time.numerator();
    auto rateNumerator = static_cast<std::int64_t>(rate.numerator());
    auto denominator = time.denominator();
    auto rateDenominator = static_cast<std::int64_t>(rate.denominator());
    if (denominator == 0 || rateDenominator == 0) {
        return std::nullopt;
    }
    const auto reduce = [](std::int64_t& left, std::int64_t& right) noexcept {
        const auto magnitude = [](const std::int64_t value) noexcept {
            return value < 0 ? -static_cast<std::uint64_t>(value)
                             : static_cast<std::uint64_t>(value);
        };
        const auto divisor = std::gcd(magnitude(left), magnitude(right));
        if (divisor > 1) {
            left /= static_cast<std::int64_t>(divisor);
            right /= static_cast<std::int64_t>(divisor);
        }
    };
    reduce(numerator, rateDenominator);
    reduce(rateNumerator, denominator);
    const auto scaled = checkedMultiply(numerator, rateNumerator);
    const auto divisor = checkedMultiply(denominator, rateDenominator);
    if (!scaled.has_value() || !divisor.has_value() || *divisor == 0) {
        return std::nullopt;
    }
    return floorDivide(*scaled, *divisor);
}

ValueGraphEvaluation evaluateValueGraph(const std::span<const CompiledValueOperation> operations,
                                        const std::size_t outputCount,
                                        const core::RationalTime time,
                                        const document::FrameRate rate) {
    Evaluator evaluator(outputCount, time, rate);
    evaluator.run(operations);
    return evaluator.release();
}

} // namespace bloom::runtime
