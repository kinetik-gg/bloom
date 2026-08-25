#include <bloom/core/scalar_primitives.hpp>

#include <bloom/core/floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {

using bloom::core::primitives::ScalarEvaluationError;
using bloom::core::primitives::ScalarPrimitive;
using bloom::core::primitives::ScalarPrimitiveSignature;

constexpr std::array kSignatures{
    ScalarPrimitiveSignature{
        ScalarPrimitive::Add, "bloom.core.scalar.add", {"left", "right", "", "", ""}, 2},
    ScalarPrimitiveSignature{
        ScalarPrimitive::Subtract, "bloom.core.scalar.subtract", {"left", "right", "", "", ""}, 2},
    ScalarPrimitiveSignature{
        ScalarPrimitive::Multiply, "bloom.core.scalar.multiply", {"left", "right", "", "", ""}, 2},
    ScalarPrimitiveSignature{ScalarPrimitive::Divide,
                             "bloom.core.scalar.divide",
                             {"numerator", "denominator", "", "", ""},
                             2},
    ScalarPrimitiveSignature{ScalarPrimitive::MultiplyAdd,
                             "bloom.core.scalar.multiply-add",
                             {"multiplicand", "multiplier", "addend", "", ""},
                             3},
    ScalarPrimitiveSignature{
        ScalarPrimitive::Minimum, "bloom.core.scalar.minimum", {"left", "right", "", "", ""}, 2},
    ScalarPrimitiveSignature{
        ScalarPrimitive::Maximum, "bloom.core.scalar.maximum", {"left", "right", "", "", ""}, 2},
    ScalarPrimitiveSignature{ScalarPrimitive::Clamp,
                             "bloom.core.scalar.clamp",
                             {"value", "minimum", "maximum", "", ""},
                             3},
    ScalarPrimitiveSignature{
        ScalarPrimitive::Remap,
        "bloom.core.scalar.remap",
        {"value", "sourceMinimum", "sourceMaximum", "destinationMinimum", "destinationMaximum"},
        5},
    ScalarPrimitiveSignature{
        ScalarPrimitive::Mix, "bloom.core.scalar.mix", {"start", "end", "factor", "", ""}, 3},
    ScalarPrimitiveSignature{
        ScalarPrimitive::Absolute, "bloom.core.scalar.absolute", {"value", "", "", "", ""}, 1},
    ScalarPrimitiveSignature{
        ScalarPrimitive::Negate, "bloom.core.scalar.negate", {"value", "", "", "", ""}, 1},
    ScalarPrimitiveSignature{
        ScalarPrimitive::Sign, "bloom.core.scalar.sign", {"value", "", "", "", ""}, 1},
    ScalarPrimitiveSignature{
        ScalarPrimitive::Reciprocal, "bloom.core.scalar.reciprocal", {"value", "", "", "", ""}, 1},
    ScalarPrimitiveSignature{
        ScalarPrimitive::SquareRoot, "bloom.core.scalar.square-root", {"value", "", "", "", ""}, 1},
    ScalarPrimitiveSignature{
        ScalarPrimitive::Floor, "bloom.core.scalar.floor", {"value", "", "", "", ""}, 1},
    ScalarPrimitiveSignature{
        ScalarPrimitive::Ceiling, "bloom.core.scalar.ceiling", {"value", "", "", "", ""}, 1},
    ScalarPrimitiveSignature{
        ScalarPrimitive::Round, "bloom.core.scalar.round", {"value", "", "", "", ""}, 1},
    ScalarPrimitiveSignature{
        ScalarPrimitive::Truncate, "bloom.core.scalar.truncate", {"value", "", "", "", ""}, 1},
    ScalarPrimitiveSignature{
        ScalarPrimitive::Fraction, "bloom.core.scalar.fraction", {"value", "", "", "", ""}, 1},
    ScalarPrimitiveSignature{ScalarPrimitive::Modulo,
                             "bloom.core.scalar.modulo",
                             {"dividend", "divisor", "", "", ""},
                             2},
    ScalarPrimitiveSignature{
        ScalarPrimitive::Step, "bloom.core.scalar.step", {"edge", "value", "", "", ""}, 2},
    ScalarPrimitiveSignature{ScalarPrimitive::Smoothstep,
                             "bloom.core.scalar.smoothstep",
                             {"lowerEdge", "upperEdge", "value", "", ""},
                             3},
    ScalarPrimitiveSignature{ScalarPrimitive::Smootherstep,
                             "bloom.core.scalar.smootherstep",
                             {"lowerEdge", "upperEdge", "value", "", ""},
                             3},
};

template <bloom::core::primitives::PrimitiveFloat T> struct Evaluation final {
    T value = T{0};
    ScalarEvaluationError error = ScalarEvaluationError::None;
};

template <bloom::core::primitives::PrimitiveFloat T>
[[nodiscard]] Evaluation<T> failure(const ScalarEvaluationError error) noexcept {
    return {T{0}, error};
}

template <bloom::core::primitives::PrimitiveFloat T>
[[nodiscard]] Evaluation<T> finiteResult(const T value) noexcept {
    if (!std::isfinite(value)) {
        return failure<T>(ScalarEvaluationError::NonFiniteResult);
    }
    return {value, ScalarEvaluationError::None};
}

template <bloom::core::primitives::PrimitiveFloat T>
[[nodiscard]] Evaluation<T> interpolate(const T start, const T end, const T factor) noexcept {
    if (factor == T{0}) {
        return {start, ScalarEvaluationError::None};
    }
    if (factor == T{1}) {
        return {end, ScalarEvaluationError::None};
    }

    // This is Bloom's fixed interpolation sequence. Explicit std::fma defines the only fused
    // operation; compiler flags prohibit implicit contraction elsewhere. Opposite-sign endpoints
    // use a weighted sum so end-start cannot overflow when the mathematical result is finite.
    if ((start <= T{0} && end >= T{0}) || (start >= T{0} && end <= T{0})) {
        const auto startContribution = (T{1} - factor) * start;
        return finiteResult(std::fma(factor, end, startContribution));
    }
    return finiteResult(std::fma(factor, end - start, start));
}

template <bloom::core::primitives::PrimitiveFloat T>
[[nodiscard]] Evaluation<T> remapFactor(const T value, const T sourceMinimum,
                                        const T sourceMaximum) noexcept {
    if (value == sourceMinimum) {
        return {T{0}, ScalarEvaluationError::None};
    }
    if (value == sourceMaximum) {
        return {T{1}, ScalarEvaluationError::None};
    }

    const auto sourceSpan = sourceMaximum - sourceMinimum;
    if (std::isfinite(sourceSpan)) {
        const auto sourceOffset = value - sourceMinimum;
        if (std::isfinite(sourceOffset)) {
            return finiteResult(sourceOffset / sourceSpan);
        }

        // The direct offset can overflow for opposite-sign value/minimum pairs even when their
        // ratio against a finite span is representable. Divide first in that case.
        return finiteResult((value / sourceSpan) - (sourceMinimum / sourceSpan));
    }

    // A finite subtraction overflows only when the endpoints have opposite signs. Scaling both
    // endpoints and the value first keeps every term in [-1, 1] and preserves a finite factor.
    const auto scale = std::max(std::abs(sourceMinimum), std::abs(sourceMaximum));
    const auto scaledMinimum = sourceMinimum / scale;
    const auto scaledSpan = (sourceMaximum / scale) - scaledMinimum;
    const auto scaledOffset = (value / scale) - scaledMinimum;
    return finiteResult(scaledOffset / scaledSpan);
}

template <bloom::core::primitives::PrimitiveFloat T>
[[nodiscard]] T minimumValue(const T left, const T right) noexcept {
    if (left == right) {
        if (left == T{0} && (std::signbit(left) || std::signbit(right))) {
            return std::copysign(T{0}, T{-1});
        }
        return left;
    }
    return left < right ? left : right;
}

template <bloom::core::primitives::PrimitiveFloat T>
[[nodiscard]] T maximumValue(const T left, const T right) noexcept {
    if (left == right) {
        if (left == T{0} && (!std::signbit(left) || !std::signbit(right))) {
            return T{0};
        }
        return left;
    }
    return left > right ? left : right;
}

template <bloom::core::primitives::PrimitiveFloat T>
[[nodiscard]] Evaluation<T> smoothFactor(const T lowerEdge, const T upperEdge,
                                         const T value) noexcept {
    if (lowerEdge == upperEdge) {
        return failure<T>(ScalarEvaluationError::DegenerateRange);
    }
    if (lowerEdge > upperEdge) {
        return failure<T>(ScalarEvaluationError::InvalidInterval);
    }
    if (value <= lowerEdge) {
        return {T{0}, ScalarEvaluationError::None};
    }
    if (value >= upperEdge) {
        return {T{1}, ScalarEvaluationError::None};
    }
    return remapFactor(value, lowerEdge, upperEdge);
}

template <bloom::core::primitives::PrimitiveFloat T>
[[nodiscard]] Evaluation<T> evaluate(const ScalarPrimitive primitive,
                                     const std::span<const T> inputs) noexcept {
    const auto* signature = bloom::core::primitives::scalarPrimitiveSignature(primitive);
    if (signature == nullptr) {
        return failure<T>(ScalarEvaluationError::UnknownPrimitive);
    }
    if (inputs.size() != signature->inputCount) {
        return failure<T>(ScalarEvaluationError::InvalidArity);
    }
    if (std::ranges::any_of(inputs, [](const T value) { return !std::isfinite(value); })) {
        return failure<T>(ScalarEvaluationError::NonFiniteInput);
    }
    if (!bloom::core::supportsReferenceFloatingPointEnvironment<T>()) {
        return failure<T>(ScalarEvaluationError::UnsupportedFloatingPointEnvironment);
    }

    switch (primitive) {
    case ScalarPrimitive::Add:
        return finiteResult(inputs[0] + inputs[1]);
    case ScalarPrimitive::Subtract:
        return finiteResult(inputs[0] - inputs[1]);
    case ScalarPrimitive::Multiply:
        return finiteResult(inputs[0] * inputs[1]);
    case ScalarPrimitive::Divide:
        if (inputs[1] == T{0}) {
            return failure<T>(ScalarEvaluationError::DivideByZero);
        }
        return finiteResult(inputs[0] / inputs[1]);
    case ScalarPrimitive::MultiplyAdd:
        return finiteResult(std::fma(inputs[0], inputs[1], inputs[2]));
    case ScalarPrimitive::Minimum:
        return finiteResult(minimumValue(inputs[0], inputs[1]));
    case ScalarPrimitive::Maximum:
        return finiteResult(maximumValue(inputs[0], inputs[1]));
    case ScalarPrimitive::Clamp:
        if (inputs[1] > inputs[2]) {
            return failure<T>(ScalarEvaluationError::InvalidInterval);
        }
        return finiteResult(maximumValue(inputs[1], minimumValue(inputs[0], inputs[2])));
    case ScalarPrimitive::Remap: {
        if (inputs[1] == inputs[2]) {
            return failure<T>(ScalarEvaluationError::DegenerateRange);
        }
        const auto factor = remapFactor(inputs[0], inputs[1], inputs[2]);
        if (factor.error != ScalarEvaluationError::None) {
            return factor;
        }
        return interpolate(inputs[3], inputs[4], factor.value);
    }
    case ScalarPrimitive::Mix:
        return interpolate(inputs[0], inputs[1], inputs[2]);
    case ScalarPrimitive::Absolute:
        return finiteResult(std::abs(inputs[0]));
    case ScalarPrimitive::Negate:
        return finiteResult(-inputs[0]);
    case ScalarPrimitive::Sign:
        if (inputs[0] == T{0}) {
            return {inputs[0], ScalarEvaluationError::None};
        }
        return {std::copysign(T{1}, inputs[0]), ScalarEvaluationError::None};
    case ScalarPrimitive::Reciprocal:
        if (inputs[0] == T{0}) {
            return failure<T>(ScalarEvaluationError::DivideByZero);
        }
        return finiteResult(T{1} / inputs[0]);
    case ScalarPrimitive::SquareRoot:
        if (inputs[0] < T{0}) {
            return failure<T>(ScalarEvaluationError::OutsideDomain);
        }
        return finiteResult(std::sqrt(inputs[0]));
    case ScalarPrimitive::Floor:
        return finiteResult(std::floor(inputs[0]));
    case ScalarPrimitive::Ceiling:
        return finiteResult(std::ceil(inputs[0]));
    case ScalarPrimitive::Round:
        return finiteResult(std::nearbyint(inputs[0]));
    case ScalarPrimitive::Truncate:
        return finiteResult(std::trunc(inputs[0]));
    case ScalarPrimitive::Fraction: {
        T integerPart = T{0};
        return finiteResult(std::modf(inputs[0], &integerPart));
    }
    case ScalarPrimitive::Modulo:
        if (inputs[1] == T{0}) {
            return failure<T>(ScalarEvaluationError::DivideByZero);
        }
        return finiteResult(std::fmod(inputs[0], inputs[1]));
    case ScalarPrimitive::Step:
        return {inputs[1] < inputs[0] ? T{0} : T{1}, ScalarEvaluationError::None};
    case ScalarPrimitive::Smoothstep: {
        const auto factor = smoothFactor(inputs[0], inputs[1], inputs[2]);
        if (factor.error != ScalarEvaluationError::None) {
            return factor;
        }
        const auto squared = factor.value * factor.value;
        return finiteResult(squared * (T{3} - (T{2} * factor.value)));
    }
    case ScalarPrimitive::Smootherstep: {
        const auto factor = smoothFactor(inputs[0], inputs[1], inputs[2]);
        if (factor.error != ScalarEvaluationError::None) {
            return factor;
        }
        const auto squared = factor.value * factor.value;
        const auto cubed = squared * factor.value;
        const auto polynomial = (factor.value * ((factor.value * T{6}) - T{15})) + T{10};
        return finiteResult(cubed * polynomial);
    }
    case ScalarPrimitive::Invalid:
        return failure<T>(ScalarEvaluationError::UnknownPrimitive);
    }

    return failure<T>(ScalarEvaluationError::UnknownPrimitive);
}

static_assert(sizeof(float) == 4, "Scalar Float32 primitives require 32-bit float");
} // namespace

namespace bloom::core::primitives {

std::span<const ScalarPrimitiveSignature> scalarPrimitiveSignatures() noexcept {
    return kSignatures;
}

const ScalarPrimitiveSignature* scalarPrimitiveSignature(const ScalarPrimitive primitive) noexcept {
    const auto* const match =
        std::ranges::find(kSignatures, primitive, &ScalarPrimitiveSignature::primitive);
    return match == kSignatures.end() ? nullptr : &*match;
}

ScalarResult<float> evaluateScalar(const ScalarPrimitive primitive,
                                   const std::span<const float> inputs) noexcept {
    const auto result = evaluate(primitive, inputs);
    return ScalarResult<float>(result.value, result.error);
}

ScalarResult<double> evaluateScalar(const ScalarPrimitive primitive,
                                    const std::span<const double> inputs) noexcept {
    const auto result = evaluate(primitive, inputs);
    return ScalarResult<double>(result.value, result.error);
}

} // namespace bloom::core::primitives
