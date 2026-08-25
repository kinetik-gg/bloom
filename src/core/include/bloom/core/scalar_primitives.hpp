#ifndef BLOOM_CORE_SCALAR_PRIMITIVES_HPP
#define BLOOM_CORE_SCALAR_PRIMITIVES_HPP

#include <array>
#include <concepts>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace bloom::core::primitives {

enum class ScalarPrimitive : std::uint8_t {
    Add,
    Subtract,
    Multiply,
    Divide,
    MultiplyAdd,
    Minimum,
    Maximum,
    Clamp,
    Remap,
    Mix,
    Absolute,
    Negate,
    Sign,
    Reciprocal,
    SquareRoot,
    Floor,
    Ceiling,
    Round,
    Truncate,
    Fraction,
    Modulo,
    Step,
    Smoothstep,
    Smootherstep,
    Invalid = 0xFF,
};

inline constexpr std::uint32_t kScalarPrimitiveSemanticsVersion = 2;

struct ScalarPrimitiveSignature final {
    ScalarPrimitive primitive;
    std::string_view id;
    // Input names are stable semantic roles. Slots at and after inputCount are empty.
    std::array<std::string_view, 5> inputNames;
    std::uint8_t inputCount;

    friend constexpr bool operator==(const ScalarPrimitiveSignature&,
                                     const ScalarPrimitiveSignature&) noexcept = default;
};

enum class ScalarEvaluationError : std::uint8_t {
    None,
    UnknownPrimitive,
    InvalidArity,
    NonFiniteInput,
    UnsupportedFloatingPointEnvironment,
    DivideByZero,
    InvalidInterval,
    DegenerateRange,
    NonFiniteResult,
    OutsideDomain,
};

template <typename T>
concept PrimitiveFloat =
    std::same_as<std::remove_cv_t<T>, float> || std::same_as<std::remove_cv_t<T>, double>;

template <PrimitiveFloat T> class ScalarResult;

// Evaluation reports the first failure in this order: unknown primitive, arity, input finiteness,
// floating-point environment, operation domain, then result finiteness.
[[nodiscard]] ScalarResult<float> evaluateScalar(ScalarPrimitive primitive,
                                                 std::span<const float> inputs) noexcept;
[[nodiscard]] ScalarResult<double> evaluateScalar(ScalarPrimitive primitive,
                                                  std::span<const double> inputs) noexcept;

template <PrimitiveFloat T> class [[nodiscard]] ScalarResult final {
  public:
    [[nodiscard]] constexpr bool hasValue() const noexcept {
        return error_ == ScalarEvaluationError::None;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] constexpr const T* value() const& noexcept {
        return hasValue() ? &value_ : nullptr;
    }
    [[nodiscard]] constexpr const T* value() const&& = delete;
    [[nodiscard]] constexpr ScalarEvaluationError error() const noexcept { return error_; }

  private:
    friend ScalarResult<float> evaluateScalar(ScalarPrimitive, std::span<const float>) noexcept;
    friend ScalarResult<double> evaluateScalar(ScalarPrimitive, std::span<const double>) noexcept;

    constexpr ScalarResult(const T value, const ScalarEvaluationError error) noexcept
        : value_(value), error_(error) {}

    T value_ = T{0};
    ScalarEvaluationError error_ = ScalarEvaluationError::None;
};

static_assert(std::is_trivially_copyable_v<ScalarResult<float>>);
static_assert(std::is_trivially_copyable_v<ScalarResult<double>>);

[[nodiscard]] std::span<const ScalarPrimitiveSignature> scalarPrimitiveSignatures() noexcept;
[[nodiscard]] const ScalarPrimitiveSignature*
scalarPrimitiveSignature(ScalarPrimitive primitive) noexcept;

} // namespace bloom::core::primitives

#endif // BLOOM_CORE_SCALAR_PRIMITIVES_HPP
