#ifndef BLOOM_CORE_VALUE_PRIMITIVES_HPP
#define BLOOM_CORE_VALUE_PRIMITIVES_HPP

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::core::primitives {

// The vector tranche that sits BESIDE the frozen 24-operation ScalarPrimitive vocabulary
// (scalar_primitives.hpp), deliberately small. Componentwise vector arithmetic is not listed here:
// Add, Subtract, Multiply, Divide, Minimum, Maximum and every other per-component operation routes
// through evaluateScalar() one component at a time, so a Vector Math node and a Math node cannot
// disagree about what dividing by zero or exceeding the finite range means. What IS here is exactly
// the set that has no scalar spelling at all -- the operations that reduce or renormalize ACROSS
// components and therefore cannot be expressed as N independent scalar evaluations.
//
// Same hard contract as the scalar tranche: allocation-free, noexcept, no NaN or infinity ever
// leaves a kernel. A domain failure is reported, never substituted; choosing a fallback is the
// caller's decision because only the caller knows what the value is for (see
// docs/architecture/evaluation-primitives.md).
enum class VectorPrimitive : std::uint8_t {
    // |a|
    Length,
    // a / |a|
    Normalize,
    // a . b
    DotProduct,
    // |a - b|
    Distance,
    // a x b -- three components only; a two-component cross product is not a vector.
    CrossProduct,
    Invalid = 0xFF,
};

inline constexpr std::uint32_t kVectorPrimitiveSemanticsVersion = 1;

// The number of components a vector operand may carry. Two and three are the only authored vector
// widths in Bloom's socket vocabulary, and a Color is NOT one of them: per
// docs/architecture/evaluation-primitives.md's Type Binding rule, a Color is not a Vec4 and never
// reaches these kernels.
inline constexpr std::uint8_t kMinimumVectorComponents = 2;
inline constexpr std::uint8_t kMaximumVectorComponents = 3;

struct VectorPrimitiveSignature final {
    VectorPrimitive primitive;
    std::string_view id;
    // How many vector operands the flat input span carries, in order.
    std::uint8_t vectorOperandCount;
    // Whether the result is one scalar (Length, DotProduct, Distance) rather than a vector of the
    // operand width.
    bool producesScalar;
    // Whether the operation is defined only at three components (CrossProduct).
    bool requiresThreeComponents;

    friend constexpr bool operator==(const VectorPrimitiveSignature&,
                                     const VectorPrimitiveSignature&) noexcept = default;
};

enum class VectorEvaluationError : std::uint8_t {
    None,
    UnknownPrimitive,
    InvalidArity,
    UnsupportedComponentCount,
    NonFiniteInput,
    UnsupportedFloatingPointEnvironment,
    // A zero-length vector has no direction, so Normalize has no answer for it.
    DegenerateVector,
    NonFiniteResult,
};

class [[nodiscard]] VectorResult final {
  public:
    [[nodiscard]] constexpr bool hasValue() const noexcept {
        return error_ == VectorEvaluationError::None;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return hasValue(); }
    // One value wide for a scalar-producing operation, `components` wide otherwise; empty on
    // failure.
    [[nodiscard]] constexpr std::span<const double> components() const& noexcept {
        return {values_.data(), count_};
    }
    [[nodiscard]] constexpr std::span<const double> components() const&& = delete;
    [[nodiscard]] constexpr VectorEvaluationError error() const noexcept { return error_; }

  private:
    friend VectorResult evaluateVector(VectorPrimitive, std::uint8_t,
                                       std::span<const double>) noexcept;

    constexpr VectorResult(const std::array<double, 3> values, const std::uint8_t count,
                           const VectorEvaluationError error) noexcept
        : values_(values), count_(count), error_(error) {}

    std::array<double, 3> values_{};
    std::uint8_t count_ = 0;
    VectorEvaluationError error_ = VectorEvaluationError::None;
};

// `inputs` is the flat operand layout: operand A's `components` values, then operand B's
// `components` values for a two-operand signature. `components` must be 2 or 3.
//
// Failures are reported in this order: unknown primitive, component count, arity, input
// finiteness, floating-point environment, operation domain, then result finiteness -- the same
// order evaluateScalar() documents, so the two tranches report a malformed call identically.
[[nodiscard]] VectorResult evaluateVector(VectorPrimitive primitive, std::uint8_t components,
                                          std::span<const double> inputs) noexcept;

[[nodiscard]] std::span<const VectorPrimitiveSignature> vectorPrimitiveSignatures() noexcept;
[[nodiscard]] const VectorPrimitiveSignature*
vectorPrimitiveSignature(VectorPrimitive primitive) noexcept;

// Deterministic seeded value hash: the same seed yields the same double in [0, 1) on every
// platform, every process and every frame. There is deliberately no entropy source -- a render has
// to be reproducible, and a cached or exported frame must agree with the frame that produced it --
// so a Random node is a pure function of its seed and varies over time only because something
// wired a changing value (a Time node's frame number) into that seed.
[[nodiscard]] double hashUnitValue(std::int64_t seed) noexcept;

inline constexpr std::uint32_t kValueHashSemanticsVersion = 1;

} // namespace bloom::core::primitives

#endif // BLOOM_CORE_VALUE_PRIMITIVES_HPP
