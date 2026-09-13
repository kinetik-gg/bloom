#include <bloom/core/value_primitives.hpp>

#include <bloom/core/floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace {

using bloom::core::primitives::VectorEvaluationError;
using bloom::core::primitives::VectorPrimitive;
using bloom::core::primitives::VectorPrimitiveSignature;

constexpr std::array kSignatures{
    VectorPrimitiveSignature{VectorPrimitive::Length, "bloom.core.vector.length", 1, true, false},
    VectorPrimitiveSignature{VectorPrimitive::Normalize, "bloom.core.vector.normalize", 1, false,
                             false},
    VectorPrimitiveSignature{VectorPrimitive::DotProduct, "bloom.core.vector.dot-product", 2, true,
                             false},
    VectorPrimitiveSignature{VectorPrimitive::Distance, "bloom.core.vector.distance", 2, true,
                             false},
    VectorPrimitiveSignature{VectorPrimitive::CrossProduct, "bloom.core.vector.cross-product", 2,
                             false, true},
};

struct Evaluation final {
    std::array<double, 3> values{};
    std::uint8_t count = 0;
    VectorEvaluationError error = VectorEvaluationError::None;
};

[[nodiscard]] Evaluation failure(const VectorEvaluationError error) noexcept {
    return {{}, 0, error};
}

[[nodiscard]] Evaluation finiteScalar(const double value) noexcept {
    if (!std::isfinite(value)) {
        return failure(VectorEvaluationError::NonFiniteResult);
    }
    return {{value, 0.0, 0.0}, 1, VectorEvaluationError::None};
}

[[nodiscard]] Evaluation finiteVector(const std::array<double, 3> values,
                                      const std::uint8_t count) noexcept {
    for (std::uint8_t component = 0; component < count; ++component) {
        if (!std::isfinite(values[component])) {
            return failure(VectorEvaluationError::NonFiniteResult);
        }
    }
    return {values, count, VectorEvaluationError::None};
}

// The one magnitude routine both Length and Normalize read, scaled by the operand's own largest
// component so a vector whose squares overflow still reports the finite length it mathematically
// has. std::hypot is deliberately not used: it is specified for two and three arguments only in
// C++17 and its accuracy is implementation-defined, and this is a reference kernel whose result
// must be identical on every platform.
[[nodiscard]] double magnitude(const std::span<const double> operand) noexcept {
    double largest = 0.0;
    for (const double component : operand) {
        largest = std::max(largest, std::abs(component));
    }
    if (largest == 0.0) {
        return 0.0;
    }
    double sum = 0.0;
    for (const double component : operand) {
        const double scaled = component / largest;
        sum += scaled * scaled;
    }
    return largest * std::sqrt(sum);
}

} // namespace

namespace bloom::core::primitives {

std::span<const VectorPrimitiveSignature> vectorPrimitiveSignatures() noexcept {
    return kSignatures;
}

const VectorPrimitiveSignature* vectorPrimitiveSignature(const VectorPrimitive primitive) noexcept {
    const auto* const match =
        std::ranges::find(kSignatures, primitive, &VectorPrimitiveSignature::primitive);
    return match == kSignatures.end() ? nullptr : &*match;
}

VectorResult evaluateVector(const VectorPrimitive primitive, const std::uint8_t components,
                            const std::span<const double> inputs) noexcept {
    const auto evaluation = [&]() -> Evaluation {
        const auto* signature = vectorPrimitiveSignature(primitive);
        if (signature == nullptr) {
            return failure(VectorEvaluationError::UnknownPrimitive);
        }
        if (components < kMinimumVectorComponents || components > kMaximumVectorComponents ||
            (signature->requiresThreeComponents && components != kMaximumVectorComponents)) {
            return failure(VectorEvaluationError::UnsupportedComponentCount);
        }
        if (inputs.size() != static_cast<std::size_t>(components) * signature->vectorOperandCount) {
            return failure(VectorEvaluationError::InvalidArity);
        }
        if (std::ranges::any_of(inputs, [](const double value) { return !std::isfinite(value); })) {
            return failure(VectorEvaluationError::NonFiniteInput);
        }
        if (!supportsReferenceFloatingPointEnvironment<double>()) {
            return failure(VectorEvaluationError::UnsupportedFloatingPointEnvironment);
        }

        const auto left = inputs.subspan(0, components);
        switch (primitive) {
        case VectorPrimitive::Length:
            return finiteScalar(magnitude(left));
        case VectorPrimitive::Normalize: {
            const double length = magnitude(left);
            if (length == 0.0) {
                return failure(VectorEvaluationError::DegenerateVector);
            }
            std::array<double, 3> result{};
            for (std::uint8_t component = 0; component < components; ++component) {
                result[component] = left[component] / length;
            }
            return finiteVector(result, components);
        }
        case VectorPrimitive::DotProduct: {
            const auto right = inputs.subspan(components, components);
            double sum = 0.0;
            for (std::uint8_t component = 0; component < components; ++component) {
                sum = std::fma(left[component], right[component], sum);
            }
            return finiteScalar(sum);
        }
        case VectorPrimitive::Distance: {
            const auto right = inputs.subspan(components, components);
            std::array<double, 3> difference{};
            for (std::uint8_t component = 0; component < components; ++component) {
                difference[component] = left[component] - right[component];
            }
            return finiteScalar(magnitude(std::span<const double>(difference.data(), components)));
        }
        case VectorPrimitive::CrossProduct: {
            const auto right = inputs.subspan(components, components);
            const std::array<double, 3> result{std::fma(left[1], right[2], -(left[2] * right[1])),
                                               std::fma(left[2], right[0], -(left[0] * right[2])),
                                               std::fma(left[0], right[1], -(left[1] * right[0]))};
            return finiteVector(result, components);
        }
        case VectorPrimitive::Invalid:
            return failure(VectorEvaluationError::UnknownPrimitive);
        }
        return failure(VectorEvaluationError::UnknownPrimitive);
    }();
    return VectorResult(evaluation.values, evaluation.count, evaluation.error);
}

// SplitMix64's finalizer. Chosen because it is a fixed, fully specified integer sequence with no
// platform-dependent step: the same seed produces the same 64 bits under every compiler and
// architecture, which is what makes a seeded Random node safe to cache a frame from.
double hashUnitValue(const std::int64_t seed) noexcept {
    auto state = static_cast<std::uint64_t>(seed) + 0x9E37'79B9'7F4A'7C15ULL;
    state ^= state >> 30U;
    state *= 0xBF58'476D'1CE4'E5B9ULL;
    state ^= state >> 27U;
    state *= 0x94D0'49BB'1331'11EBULL;
    state ^= state >> 31U;
    // Exactly 53 bits, binary64's significand width: every value is representable with no rounding,
    // the spacing is uniform, and the result can never round up to 1.0.
    return static_cast<double>(state >> 11U) * 0x1.0p-53;
}

} // namespace bloom::core::primitives
