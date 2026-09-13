#include <bloom/core/value_primitives.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_set>

namespace {

using bloom::core::primitives::evaluateVector;
using bloom::core::primitives::hashUnitValue;
using bloom::core::primitives::VectorEvaluationError;
using bloom::core::primitives::VectorPrimitive;
using bloom::core::primitives::vectorPrimitiveSignature;
using bloom::core::primitives::vectorPrimitiveSignatures;
using bloom::core::primitives::VectorResult;

template <typename Result>
concept ExposesComponentsFromConstRvalue =
    requires(Result result) { static_cast<const Result&&>(result).components(); };

static_assert(std::is_trivially_copyable_v<VectorResult>);
// The borrow-safety contract the scalar tranche already states: a temporary's span must not be
// reachable, because the array it views dies with the temporary.
static_assert(!ExposesComponentsFromConstRvalue<VectorResult>);

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] bool sameScalar(const VectorResult& result, const double expected) {
    return result.hasValue() && result.components().size() == 1 &&
           result.components()[0] == expected;
}

[[nodiscard]] bool sameVector(const VectorResult& result, const std::span<const double> expected) {
    if (!result.hasValue() || result.components().size() != expected.size()) {
        return false;
    }
    for (std::size_t component = 0; component < expected.size(); ++component) {
        if (result.components()[component] != expected[component]) {
            return false;
        }
    }
    return true;
}

void testSignatures(Expectations& expectations) {
    const auto signatures = vectorPrimitiveSignatures();
    expectations.expect(signatures.size() == 5, "the tranche publishes exactly five signatures");
    std::unordered_set<std::string_view> ids;
    for (const auto& signature : signatures) {
        expectations.expect(signature.id.starts_with("bloom.core.vector."),
                            "every signature id is namespaced under bloom.core.vector");
        expectations.expect(ids.insert(signature.id).second, "signature ids are unique");
        expectations.expect(signature.vectorOperandCount == 1 || signature.vectorOperandCount == 2,
                            "every signature takes one or two vector operands");
        expectations.expect(vectorPrimitiveSignature(signature.primitive) == &signature,
                            "lookup answers with the published signature");
    }
    expectations.expect(vectorPrimitiveSignature(VectorPrimitive::Invalid) == nullptr,
                        "the invalid primitive has no signature");
    const auto* cross = vectorPrimitiveSignature(VectorPrimitive::CrossProduct);
    expectations.expect(cross != nullptr && cross->requiresThreeComponents,
                        "cross product is declared three-component only");
    const auto* length = vectorPrimitiveSignature(VectorPrimitive::Length);
    expectations.expect(length != nullptr && length->producesScalar &&
                            !length->requiresThreeComponents,
                        "length is a scalar-producing operation at either width");
}

void testDomainValidResults(Expectations& expectations) {
    constexpr std::array<double, 2> unitX{1.0, 0.0};
    expectations.expect(sameScalar(evaluateVector(VectorPrimitive::Length, 2, unitX), 1.0),
                        "the length of a unit vector is one");
    constexpr std::array<double, 3> threeFourZero{3.0, 4.0, 0.0};
    expectations.expect(sameScalar(evaluateVector(VectorPrimitive::Length, 3, threeFourZero), 5.0),
                        "a 3-4-5 triangle's length is exact");

    constexpr std::array<double, 2> threeFour{3.0, 4.0};
    constexpr std::array<double, 2> normalized{0.6, 0.8};
    expectations.expect(
        sameVector(evaluateVector(VectorPrimitive::Normalize, 2, threeFour), normalized),
        "normalizing 3,4 yields 0.6,0.8 exactly");

    constexpr std::array<double, 4> dotOperands{1.0, 2.0, 3.0, 4.0};
    expectations.expect(
        sameScalar(evaluateVector(VectorPrimitive::DotProduct, 2, dotOperands), 11.0),
        "the dot product of 1,2 and 3,4 is eleven");

    constexpr std::array<double, 4> distanceOperands{0.0, 0.0, 3.0, 4.0};
    expectations.expect(
        sameScalar(evaluateVector(VectorPrimitive::Distance, 2, distanceOperands), 5.0),
        "the distance from the origin to 3,4 is five");

    constexpr std::array<double, 6> crossOperands{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    constexpr std::array<double, 3> unitZ{0.0, 0.0, 1.0};
    expectations.expect(
        sameVector(evaluateVector(VectorPrimitive::CrossProduct, 3, crossOperands), unitZ),
        "x cross y is z");
    constexpr std::array<double, 6> reversedCross{0.0, 1.0, 0.0, 1.0, 0.0, 0.0};
    constexpr std::array<double, 3> negativeUnitZ{0.0, 0.0, -1.0};
    expectations.expect(
        sameVector(evaluateVector(VectorPrimitive::CrossProduct, 3, reversedCross), negativeUnitZ),
        "y cross x is negative z");
}

void testDocumentedEdgeCases(Expectations& expectations) {
    constexpr std::array<double, 2> origin{0.0, 0.0};
    expectations.expect(evaluateVector(VectorPrimitive::Normalize, 2, origin).error() ==
                            VectorEvaluationError::DegenerateVector,
                        "normalizing a zero-length vector is a reported domain failure");
    constexpr std::array<double, 2> signedZeroes{-0.0, 0.0};
    expectations.expect(evaluateVector(VectorPrimitive::Normalize, 2, signedZeroes).error() ==
                            VectorEvaluationError::DegenerateVector,
                        "a signed-zero vector is still zero length");
    expectations.expect(sameScalar(evaluateVector(VectorPrimitive::Length, 2, signedZeroes), 0.0),
                        "the length of a signed-zero vector is positive zero");

    constexpr std::array<double, 4> identical{2.5, -7.0, 2.5, -7.0};
    expectations.expect(sameScalar(evaluateVector(VectorPrimitive::Distance, 2, identical), 0.0),
                        "the distance between identical vectors is zero");

    // Squaring these overflows binary64, but their magnitude does not: the scaled magnitude routine
    // is what makes that a finite answer rather than an infinity.
    constexpr double huge = 3.0e300;
    const std::array<double, 2> enormous{huge, 4.0 * huge / 3.0};
    const auto enormousLength = evaluateVector(VectorPrimitive::Length, 2, enormous);
    expectations.expect(enormousLength.hasValue() &&
                            std::isfinite(enormousLength.components()[0]) &&
                            enormousLength.components()[0] > huge,
                        "a length whose squares overflow is still reported finitely");
    const auto enormousNormalized = evaluateVector(VectorPrimitive::Normalize, 2, enormous);
    expectations.expect(enormousNormalized.hasValue() &&
                            std::abs(enormousNormalized.components()[0] - 0.6) < 1.0e-15,
                        "normalizing an overflow-scale vector still yields a unit direction");

    // Subnormal inputs survive: the magnitude routine divides by the largest component first, so a
    // vector entirely inside the subnormal range does not underflow to a degenerate one.
    constexpr double tiny = std::numeric_limits<double>::denorm_min();
    const std::array<double, 2> subnormal{tiny, 0.0};
    const auto subnormalNormalized = evaluateVector(VectorPrimitive::Normalize, 2, subnormal);
    expectations.expect(subnormalNormalized.hasValue() &&
                            subnormalNormalized.components()[0] == 1.0 &&
                            subnormalNormalized.components()[1] == 0.0,
                        "a subnormal vector normalizes rather than reporting degeneracy");
}

void testFailurePrecedence(Expectations& expectations) {
    constexpr std::array<double, 2> pair{1.0, 2.0};
    expectations.expect(evaluateVector(VectorPrimitive::Invalid, 2, pair).error() ==
                            VectorEvaluationError::UnknownPrimitive,
                        "an unknown primitive is reported before anything else");
    expectations.expect(evaluateVector(VectorPrimitive::Length, 1, pair).error() ==
                            VectorEvaluationError::UnsupportedComponentCount,
                        "one component is not a vector");
    expectations.expect(evaluateVector(VectorPrimitive::Length, 4, pair).error() ==
                            VectorEvaluationError::UnsupportedComponentCount,
                        "four components are outside the authored vector widths");
    constexpr std::array<double, 4> quad{1.0, 0.0, 0.0, 1.0};
    expectations.expect(evaluateVector(VectorPrimitive::CrossProduct, 2, quad).error() ==
                            VectorEvaluationError::UnsupportedComponentCount,
                        "a two-component cross product is refused by component count");
    expectations.expect(evaluateVector(VectorPrimitive::Length, 3, pair).error() ==
                            VectorEvaluationError::InvalidArity,
                        "arity is checked against the component count and operand count");
    expectations.expect(evaluateVector(VectorPrimitive::DotProduct, 2, pair).error() ==
                            VectorEvaluationError::InvalidArity,
                        "a two-operand signature needs both operands");

    const std::array<double, 2> withNan{std::numeric_limits<double>::quiet_NaN(), 0.0};
    expectations.expect(evaluateVector(VectorPrimitive::Length, 2, withNan).error() ==
                            VectorEvaluationError::NonFiniteInput,
                        "a NaN input is rejected rather than propagated");
    const std::array<double, 2> withInfinity{std::numeric_limits<double>::infinity(), 0.0};
    expectations.expect(evaluateVector(VectorPrimitive::Normalize, 2, withInfinity).error() ==
                            VectorEvaluationError::NonFiniteInput,
                        "an infinite input is rejected rather than propagated");
    // Component count is checked before arity: a call that is wrong about both reports the width.
    expectations.expect(evaluateVector(VectorPrimitive::Length, 9, pair).error() ==
                            VectorEvaluationError::UnsupportedComponentCount,
                        "component count precedes arity in the failure order");

    const auto failed = evaluateVector(VectorPrimitive::Length, 1, pair);
    expectations.expect(failed.components().empty(),
                        "a failed evaluation exposes no components at all");
}

void testValueHash(Expectations& expectations) {
    expectations.expect(hashUnitValue(0) == hashUnitValue(0),
                        "the same seed hashes to the same value every call");
    expectations.expect(hashUnitValue(0) != hashUnitValue(1), "adjacent seeds do not collide");
    std::unordered_set<std::uint64_t> seen;
    bool inRange = true;
    for (std::int64_t seed = -512; seed <= 512; ++seed) {
        const double value = hashUnitValue(seed);
        inRange = inRange && value >= 0.0 && value < 1.0 && std::isfinite(value);
        seen.insert(std::bit_cast<std::uint64_t>(value));
    }
    expectations.expect(inRange, "every hashed value lands in [0, 1)");
    expectations.expect(seen.size() == 1025, "1025 consecutive seeds produce 1025 distinct values");
    // Pinned so a change to the mixing sequence -- which would silently change every rendered
    // Random value -- cannot pass unnoticed.
    expectations.expect(hashUnitValue(0) == 0.8833108082136426,
                        "the seed-zero value is the pinned golden");
    expectations.expect(hashUnitValue(1) == 0.5665615751722809,
                        "the seed-one value is the pinned golden");
    expectations.expect(hashUnitValue(-1) == 0.8939429202831845,
                        "the seed-minus-one value is the pinned golden");
}

} // namespace

int main() {
    Expectations expectations;
    testSignatures(expectations);
    testDomainValidResults(expectations);
    testDocumentedEdgeCases(expectations);
    testFailurePrecedence(expectations);
    testValueHash(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
