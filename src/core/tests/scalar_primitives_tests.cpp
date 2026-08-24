#include <bloom/core/color.hpp>
#include <bloom/core/scalar_primitives.hpp>

#include <array>
#include <cfenv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_set>

#if defined(__SSE2__) || defined(_M_X64)
#include <xmmintrin.h>
#define BLOOM_SCALAR_TEST_HAS_MXCSR 1
#else
#define BLOOM_SCALAR_TEST_HAS_MXCSR 0
#endif

namespace {

using bloom::core::primitives::evaluateScalar;
using bloom::core::primitives::PrimitiveFloat;
using bloom::core::primitives::ScalarEvaluationError;
using bloom::core::primitives::ScalarPrimitive;
using bloom::core::primitives::ScalarResult;

template <typename T>
concept ScalarEvaluable =
    requires(const std::span<const T> inputs) { evaluateScalar(ScalarPrimitive::Add, inputs); };

template <typename Result>
concept ExposesValueFromLvalue = requires(Result& result) { result.value(); };

template <typename Result>
concept ExposesValueFromConstLvalue = requires(const Result& result) { result.value(); };

template <typename Result>
concept ExposesValueFromRvalue = requires(Result result) { static_cast<Result&&>(result).value(); };

template <typename Result>
concept ExposesValueFromConstRvalue =
    requires(Result result) { static_cast<const Result&&>(result).value(); };

static_assert(PrimitiveFloat<float>);
static_assert(PrimitiveFloat<double>);
static_assert(!PrimitiveFloat<long double>);
static_assert(ScalarEvaluable<float>);
static_assert(ScalarEvaluable<double>);
static_assert(!ScalarEvaluable<bloom::core::Color4d>);
static_assert(std::is_trivially_copyable_v<ScalarResult<float>>);
static_assert(std::is_trivially_copyable_v<ScalarResult<double>>);
static_assert(ExposesValueFromLvalue<ScalarResult<float>>);
static_assert(ExposesValueFromConstLvalue<ScalarResult<float>>);
static_assert(!ExposesValueFromRvalue<ScalarResult<float>>);
static_assert(!ExposesValueFromConstRvalue<ScalarResult<float>>);
static_assert(ExposesValueFromLvalue<ScalarResult<double>>);
static_assert(ExposesValueFromConstLvalue<ScalarResult<double>>);
static_assert(!ExposesValueFromRvalue<ScalarResult<double>>);
static_assert(!ExposesValueFromConstRvalue<ScalarResult<double>>);

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

class RoundingModeGuard final {
  public:
    RoundingModeGuard() noexcept : original_(std::fegetround()) {}
    ~RoundingModeGuard() { static_cast<void>(std::fesetround(original_)); }

    RoundingModeGuard(const RoundingModeGuard&) = delete;
    RoundingModeGuard& operator=(const RoundingModeGuard&) = delete;

  private:
    int original_ = FE_TONEAREST;
};

#if BLOOM_SCALAR_TEST_HAS_MXCSR
class MxcsrGuard final {
  public:
    MxcsrGuard() noexcept : original_(_mm_getcsr()) {}
    ~MxcsrGuard() { _mm_setcsr(original_); }

    MxcsrGuard(const MxcsrGuard&) = delete;
    MxcsrGuard& operator=(const MxcsrGuard&) = delete;

    [[nodiscard]] std::uint32_t original() const noexcept { return original_; }

  private:
    std::uint32_t original_ = 0;
};
#endif

template <typename T>
[[nodiscard]] bool hasValue(const ScalarResult<T>& result, const T expected) noexcept {
    return result && result.value() != nullptr && *result.value() == expected &&
           result.error() == ScalarEvaluationError::None;
}

template <typename T>
[[nodiscard]] bool hasError(const ScalarResult<T>& result,
                            const ScalarEvaluationError expected) noexcept {
    return !result && !result.hasValue() && result.value() == nullptr && result.error() == expected;
}

template <typename T, std::size_t Size>
[[nodiscard]] ScalarResult<T> evaluate(const ScalarPrimitive primitive,
                                       const std::array<T, Size>& inputs) noexcept {
    return evaluateScalar(primitive, std::span<const T>(inputs));
}

void testSignatures(Expectations& expectations) {
    using namespace bloom::core::primitives;
    const auto signatures = scalarPrimitiveSignatures();
    expectations.expect(kScalarPrimitiveSemanticsVersion == 1,
                        "the first scalar semantics revision is explicit");
    expectations.expect(signatures.size() == 10, "the first tranche has exactly ten operations");

    constexpr std::array expected{
        ScalarPrimitiveSignature{
            ScalarPrimitive::Add, "bloom.core.scalar.add", {"left", "right", "", "", ""}, 2},
        ScalarPrimitiveSignature{ScalarPrimitive::Subtract,
                                 "bloom.core.scalar.subtract",
                                 {"left", "right", "", "", ""},
                                 2},
        ScalarPrimitiveSignature{ScalarPrimitive::Multiply,
                                 "bloom.core.scalar.multiply",
                                 {"left", "right", "", "", ""},
                                 2},
        ScalarPrimitiveSignature{ScalarPrimitive::Divide,
                                 "bloom.core.scalar.divide",
                                 {"numerator", "denominator", "", "", ""},
                                 2},
        ScalarPrimitiveSignature{ScalarPrimitive::MultiplyAdd,
                                 "bloom.core.scalar.multiply-add",
                                 {"multiplicand", "multiplier", "addend", "", ""},
                                 3},
        ScalarPrimitiveSignature{ScalarPrimitive::Minimum,
                                 "bloom.core.scalar.minimum",
                                 {"left", "right", "", "", ""},
                                 2},
        ScalarPrimitiveSignature{ScalarPrimitive::Maximum,
                                 "bloom.core.scalar.maximum",
                                 {"left", "right", "", "", ""},
                                 2},
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
    };

    std::unordered_set<std::string_view> ids;
    for (std::size_t index = 0; index < signatures.size(); ++index) {
        const auto& signature = signatures[index];
        expectations.expect(signature == expected[index],
                            "primitive IDs, operand names, and arities are stable");
        expectations.expect(signature.id.starts_with("bloom.core.scalar.") &&
                                ids.insert(signature.id).second,
                            "primitive IDs are unique, non-empty, and namespaced");
        for (std::size_t inputIndex = 0; inputIndex < signature.inputNames.size(); ++inputIndex) {
            expectations.expect((inputIndex < signature.inputCount) ==
                                    !signature.inputNames[inputIndex].empty(),
                                "active operands are named and unused operand slots are empty");
        }
        expectations.expect(scalarPrimitiveSignature(signature.primitive) == &signature,
                            "signature lookup preserves stable table identity");
    }

    expectations.expect(scalarPrimitiveSignature(ScalarPrimitive::Invalid) == nullptr,
                        "invalid primitive values have no signature");
    constexpr std::array inputs{1.0F, 2.0F};
    expectations.expect(hasError(evaluate(ScalarPrimitive::Invalid, inputs),
                                 ScalarEvaluationError::UnknownPrimitive),
                        "invalid primitive evaluation has a distinct error");
}

template <typename T> void testNormalOperations(Expectations& expectations) {
    expectations.expect(hasValue(evaluate(ScalarPrimitive::Add, std::array<T, 2>{2, 3}), T{5}),
                        "addition produces the exact result");
    expectations.expect(
        hasValue(evaluate(ScalarPrimitive::Subtract, std::array<T, 2>{2, 3}), T{-1}),
        "subtraction produces the exact result");
    expectations.expect(
        hasValue(evaluate(ScalarPrimitive::Multiply, std::array<T, 2>{-2, 3}), T{-6}),
        "multiplication produces the exact result");
    expectations.expect(
        hasValue(evaluate(ScalarPrimitive::Divide, std::array<T, 2>{9, 4}), T{2.25}),
        "division produces the exact result");
    expectations.expect(
        hasValue(evaluate(ScalarPrimitive::MultiplyAdd, std::array<T, 3>{2, 3, 4}), T{10}),
        "multiply-add produces the exact result");
    expectations.expect(
        hasValue(evaluate(ScalarPrimitive::Minimum, std::array<T, 2>{-2, 3}), T{-2}),
        "minimum produces the exact result");
    expectations.expect(hasValue(evaluate(ScalarPrimitive::Maximum, std::array<T, 2>{-2, 3}), T{3}),
                        "maximum produces the exact result");
    expectations.expect(
        hasValue(evaluate(ScalarPrimitive::Clamp, std::array<T, 3>{4, -1, 2}), T{2}),
        "clamp uses a closed interval");
    expectations.expect(
        hasValue(evaluate(ScalarPrimitive::Remap, std::array<T, 5>{5, 0, 10, 20, 40}), T{30}),
        "remap produces the exact result");
    expectations.expect(
        hasValue(evaluate(ScalarPrimitive::Mix, std::array<T, 3>{10, 20, T{0.25}}), T{12.5}),
        "mix produces the exact result");
}

template <typename T> void testFusedMultiplyAdd(Expectations& expectations);

template <> void testFusedMultiplyAdd<float>(Expectations& expectations) {
    constexpr auto left = 0x1.000002p0F;
    constexpr auto right = 0x1.fffffcp-1F;
    constexpr auto addend = -1.0F;
    volatile const float roundedProduct = left * right;
    const auto unfused = roundedProduct + addend;
    const auto result =
        evaluate(ScalarPrimitive::MultiplyAdd, std::array<float, 3>{left, right, addend});
    expectations.expect(unfused == 0.0F && result && result.value() != nullptr &&
                            *result.value() != unfused &&
                            *result.value() == std::fma(left, right, addend),
                        "Float32 multiply-add deliberately uses fused semantics");
}

template <> void testFusedMultiplyAdd<double>(Expectations& expectations) {
    constexpr auto left = 0x1.0000000000001p0;
    constexpr auto right = 0x1.fffffffffffffp-1;
    constexpr auto addend = -1.0;
    volatile const double roundedProduct = left * right;
    const auto unfused = roundedProduct + addend;
    const auto result =
        evaluate(ScalarPrimitive::MultiplyAdd, std::array<double, 3>{left, right, addend});
    expectations.expect(unfused == 0.0 && result && result.value() != nullptr &&
                            *result.value() != unfused &&
                            *result.value() == std::fma(left, right, addend),
                        "Float64 multiply-add deliberately uses fused semantics");
}

template <typename T> void testIntervalsAndFactors(Expectations& expectations) {
    expectations.expect(
        hasValue(evaluate(ScalarPrimitive::Clamp, std::array<T, 3>{-4, -1, 2}), T{-1}) &&
            hasValue(evaluate(ScalarPrimitive::Clamp, std::array<T, 3>{1, 1, 1}), T{1}),
        "clamp includes endpoints and accepts an equal-bound interval");
    expectations.expect(hasError(evaluate(ScalarPrimitive::Clamp, std::array<T, 3>{0, 2, 1}),
                                 ScalarEvaluationError::InvalidInterval),
                        "clamp rejects reversed bounds without swapping them");

    expectations.expect(
        hasValue(evaluate(ScalarPrimitive::Remap, std::array<T, 5>{7.5, 10, 0, 0, 100}), T{25}) &&
            hasValue(evaluate(ScalarPrimitive::Remap, std::array<T, 5>{15, 0, 10, 20, 40}),
                     T{50}) &&
            hasValue(evaluate(ScalarPrimitive::Remap, std::array<T, 5>{5, 0, 10, 40, 20}), T{30}),
        "remap permits reversed ranges and extrapolates without clamping");
    expectations.expect(hasError(evaluate(ScalarPrimitive::Remap, std::array<T, 5>{1, 2, 2, 3, 4}),
                                 ScalarEvaluationError::DegenerateRange),
                        "remap rejects equal source endpoints");

    expectations.expect(
        hasValue(evaluate(ScalarPrimitive::Mix, std::array<T, 3>{4, 9, 0}), T{4}) &&
            hasValue(evaluate(ScalarPrimitive::Mix, std::array<T, 3>{4, 9, 1}), T{9}) &&
            hasValue(evaluate(ScalarPrimitive::Mix, std::array<T, 3>{4, 9, 2}), T{14}),
        "mix preserves exact endpoints and permits extrapolation");
}

template <typename T> void testInterpolationSequence(Expectations& expectations);

template <> void testInterpolationSequence<float>(Expectations& expectations) {
    constexpr auto start = -1.0F;
    constexpr auto end = -0x1p-24F;
    constexpr auto factor = 0x1.000002p0F;
    const auto result = evaluate(ScalarPrimitive::Mix, std::array{start, end, factor});
    volatile const float difference = end - start;
    volatile const float product = factor * difference;
    const auto unfused = product + start;
    expectations.expect(result && result.value() != nullptr && unfused == 0.0F &&
                            *result.value() == std::fma(factor, difference, start) &&
                            *result.value() != unfused,
                        "Float32 mix uses Bloom's deliberate same-sign FMA sequence");
}

template <> void testInterpolationSequence<double>(Expectations& expectations) {
    constexpr auto start = -1.0;
    constexpr auto end = -0x1p-53;
    constexpr auto factor = 0x1.0000000000001p0;
    const auto result = evaluate(ScalarPrimitive::Mix, std::array{start, end, factor});
    volatile const double difference = end - start;
    volatile const double product = factor * difference;
    const auto unfused = product + start;
    expectations.expect(result && result.value() != nullptr && unfused == 0.0 &&
                            *result.value() == std::fma(factor, difference, start) &&
                            *result.value() != unfused,
                        "Float64 mix uses Bloom's deliberate same-sign FMA sequence");
}

template <typename T> void testExtremeFiniteRanges(Expectations& expectations) {
    const auto maximum = std::numeric_limits<T>::max();
    expectations.expect(
        hasValue(
            evaluate(ScalarPrimitive::Remap, std::array<T, 5>{T{0}, -maximum, maximum, T{0}, T{1}}),
            T{0.5}) &&
            hasValue(evaluate(ScalarPrimitive::Remap,
                              std::array<T, 5>{T{0}, maximum, -maximum, T{0}, T{100}}),
                     T{50}) &&
            hasValue(evaluate(ScalarPrimitive::Remap,
                              std::array<T, 5>{-maximum, maximum, T{0}, T{0}, T{1}}),
                     T{2}),
        "remap normalizes extreme finite ranges without overflowing intermediate subtraction");
    expectations.expect(
        hasValue(evaluate(ScalarPrimitive::Remap,
                          std::array<T, 5>{T{0}, -maximum, maximum, -maximum, maximum}),
                 T{0}),
        "remap interpolates opposite-sign extreme destination endpoints without overflow");

    const auto negativeZero = -T{0};
    const auto startEndpoint =
        evaluate(ScalarPrimitive::Mix, std::array<T, 3>{negativeZero, T{1}, T{0}});
    const auto endEndpoint =
        evaluate(ScalarPrimitive::Mix, std::array<T, 3>{T{1}, negativeZero, T{1}});
    expectations.expect(startEndpoint && startEndpoint.value() != nullptr &&
                            std::signbit(*startEndpoint.value()) && endEndpoint &&
                            endEndpoint.value() != nullptr && std::signbit(*endEndpoint.value()),
                        "mix returns exact endpoint representations including signed zero");
}

template <typename T> void testNonFiniteInputs(Expectations& expectations) {
    struct Case final {
        ScalarPrimitive primitive;
        std::array<T, 5> inputs;
        std::size_t arity;
    };
    constexpr std::array cases{
        Case{ScalarPrimitive::Add, {T{1}, T{2}, T{0}, T{0}, T{0}}, 2},
        Case{ScalarPrimitive::Subtract, {T{1}, T{2}, T{0}, T{0}, T{0}}, 2},
        Case{ScalarPrimitive::Multiply, {T{1}, T{2}, T{0}, T{0}, T{0}}, 2},
        Case{ScalarPrimitive::Divide, {T{1}, T{2}, T{0}, T{0}, T{0}}, 2},
        Case{ScalarPrimitive::MultiplyAdd, {T{1}, T{2}, T{3}, T{0}, T{0}}, 3},
        Case{ScalarPrimitive::Minimum, {T{1}, T{2}, T{0}, T{0}, T{0}}, 2},
        Case{ScalarPrimitive::Maximum, {T{1}, T{2}, T{0}, T{0}, T{0}}, 2},
        Case{ScalarPrimitive::Clamp, {T{1}, T{0}, T{2}, T{0}, T{0}}, 3},
        Case{ScalarPrimitive::Remap, {T{1}, T{0}, T{2}, T{3}, T{4}}, 5},
        Case{ScalarPrimitive::Mix, {T{1}, T{2}, T{0.5}, T{0}, T{0}}, 3},
    };
    constexpr std::array nonFinite{std::numeric_limits<T>::quiet_NaN(),
                                   std::numeric_limits<T>::infinity(),
                                   -std::numeric_limits<T>::infinity()};

    for (const auto& testCase : cases) {
        for (std::size_t index = 0; index < testCase.arity; ++index) {
            for (const auto invalid : nonFinite) {
                auto inputs = testCase.inputs;
                inputs[index] = invalid;
                const auto result = evaluateScalar(
                    testCase.primitive, std::span<const T>(inputs).first(testCase.arity));
                expectations.expect(hasError(result, ScalarEvaluationError::NonFiniteInput),
                                    "every operand rejects NaN and infinity");
            }
        }
    }
}

template <typename T> void testErrorsAndPrecedence(Expectations& expectations) {
    expectations.expect(hasError(evaluate(ScalarPrimitive::Divide, std::array<T, 2>{1, T{0}}),
                                 ScalarEvaluationError::DivideByZero) &&
                            hasError(evaluate(ScalarPrimitive::Divide, std::array<T, 2>{1, -T{0}}),
                                     ScalarEvaluationError::DivideByZero),
                        "division rejects positive and negative zero");

    const auto maximum = std::numeric_limits<T>::max();
    expectations.expect(
        hasError(evaluate(ScalarPrimitive::Add, std::array<T, 2>{maximum, maximum}),
                 ScalarEvaluationError::NonFiniteResult) &&
            hasError(evaluate(ScalarPrimitive::Multiply, std::array<T, 2>{maximum, T{2}}),
                     ScalarEvaluationError::NonFiniteResult),
        "finite input overflow is a non-finite result error");

    constexpr std::array wrongArityWithNan{std::numeric_limits<T>::quiet_NaN()};
    expectations.expect(hasError(evaluate(ScalarPrimitive::Invalid, wrongArityWithNan),
                                 ScalarEvaluationError::UnknownPrimitive),
                        "unknown primitive takes precedence over arity and input validation");
    expectations.expect(hasError(evaluate(ScalarPrimitive::Divide, wrongArityWithNan),
                                 ScalarEvaluationError::InvalidArity),
                        "invalid arity takes precedence over non-finite input");
    expectations.expect(
        hasError(evaluate(ScalarPrimitive::Divide,
                          std::array<T, 2>{std::numeric_limits<T>::infinity(), T{0}}),
                 ScalarEvaluationError::NonFiniteInput),
        "non-finite input takes precedence over divide-by-zero");
    expectations.expect(
        hasError(evaluate(ScalarPrimitive::Clamp,
                          std::array<T, 3>{std::numeric_limits<T>::quiet_NaN(), T{2}, T{1}}),
                 ScalarEvaluationError::NonFiniteInput) &&
            hasError(evaluate(ScalarPrimitive::Remap,
                              std::array<T, 5>{std::numeric_limits<T>::infinity(), T{2}, T{2}, T{3},
                                               T{4}}),
                     ScalarEvaluationError::NonFiniteInput),
        "non-finite input takes precedence over interval and range errors");
}

template <typename T> void testFloatingPointEnvironment(Expectations& expectations) {
    expectations.expect(std::fegetround() == FE_TONEAREST,
                        "the scalar test begins in round-to-nearest mode");
    {
        RoundingModeGuard guard;
        expectations.expect(std::fesetround(FE_DOWNWARD) == 0,
                            "the test platform supports a non-default rounding mode");
        expectations.expect(hasError(evaluate(ScalarPrimitive::Add, std::array<T, 2>{T{1}, T{2}}),
                                     ScalarEvaluationError::UnsupportedFloatingPointEnvironment),
                            "non-default rounding is rejected before authored arithmetic");
        expectations.expect(
            hasError(evaluate(ScalarPrimitive::Divide, std::array<T, 2>{T{1}, T{0}}),
                     ScalarEvaluationError::UnsupportedFloatingPointEnvironment),
            "floating-point environment validation takes precedence over domain errors");
        expectations.expect(
            hasError(evaluate(ScalarPrimitive::Divide,
                              std::array<T, 2>{std::numeric_limits<T>::quiet_NaN(), T{0}}),
                     ScalarEvaluationError::NonFiniteInput),
            "input finiteness takes precedence over floating-environment and domain errors");
    }
    expectations.expect(std::fegetround() == FE_TONEAREST,
                        "the rounding-mode test restores round-to-nearest");

#if BLOOM_SCALAR_TEST_HAS_MXCSR
    constexpr std::uint32_t kDenormalsAreZero = 1U << 6U;
    constexpr std::uint32_t kFlushToZero = 1U << 15U;
    MxcsrGuard guard;
    const auto baseline = guard.original() & ~(kDenormalsAreZero | kFlushToZero);

    _mm_setcsr(baseline | kFlushToZero);
    expectations.expect(hasError(evaluate(ScalarPrimitive::Add, std::array<T, 2>{T{1}, T{2}}),
                                 ScalarEvaluationError::UnsupportedFloatingPointEnvironment),
                        "flush-to-zero mode is rejected");

    _mm_setcsr(baseline | kDenormalsAreZero);
    expectations.expect(hasError(evaluate(ScalarPrimitive::Add, std::array<T, 2>{T{1}, T{2}}),
                                 ScalarEvaluationError::UnsupportedFloatingPointEnvironment),
                        "denormals-are-zero mode is rejected");

    _mm_setcsr(baseline);
#endif
}

template <typename T> void testSignedZeroAndSubnormal(Expectations& expectations) {
    const auto negativeZero = -T{0};
    const auto minimum = evaluate(ScalarPrimitive::Minimum, std::array<T, 2>{T{0}, negativeZero});
    const auto maximum = evaluate(ScalarPrimitive::Maximum, std::array<T, 2>{negativeZero, T{0}});
    const auto multiplied =
        evaluate(ScalarPrimitive::Multiply, std::array<T, 2>{negativeZero, T{2}});
    expectations.expect(minimum && minimum.value() != nullptr && std::signbit(*minimum.value()) &&
                            maximum && maximum.value() != nullptr &&
                            !std::signbit(*maximum.value()) && multiplied &&
                            multiplied.value() != nullptr && std::signbit(*multiplied.value()),
                        "IEEE signed zero is preserved by defined operations");

    const auto subnormal = std::numeric_limits<T>::denorm_min();
    const auto preserved = evaluate(ScalarPrimitive::Add, std::array<T, 2>{subnormal, T{0}});
    expectations.expect(subnormal > T{0} && hasValue(preserved, subnormal),
                        "the CPU reference preserves finite subnormal values");
}

template <typename T> void runTypedTests(Expectations& expectations) {
    testNormalOperations<T>(expectations);
    testFusedMultiplyAdd<T>(expectations);
    testIntervalsAndFactors<T>(expectations);
    testInterpolationSequence<T>(expectations);
    testExtremeFiniteRanges<T>(expectations);
    testNonFiniteInputs<T>(expectations);
    testErrorsAndPrecedence<T>(expectations);
    testFloatingPointEnvironment<T>(expectations);
    testSignedZeroAndSubnormal<T>(expectations);
}

} // namespace

int main() {
    Expectations expectations;
    testSignatures(expectations);
    runTypedTests<float>(expectations);
    runTypedTests<double>(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
