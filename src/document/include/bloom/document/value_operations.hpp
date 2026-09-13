#pragma once

#include <bloom/core/scalar_primitives.hpp>
#include <bloom/core/value_primitives.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace bloom::document {

// The closed operation vocabularies the value-graph node library selects from, each with a durable
// stored integer mapping.
//
// These live here, beside the parameter schemas that validate them, for exactly the reason
// core::BlendMode's comment gives for living in bloom_core: the stored integer IS the durable
// authoring value, so one enumeration has to serve the document schema that validates it, the
// compiler that switches on it, and every surface that offers it. A document stores the NUMBER, not
// a name, so appending an operation is additive and renumbering one silently re-interprets every
// saved document.
//
// A Math node's operation is the exception that needs no enumeration of its own: it IS
// core::primitives::ScalarPrimitive, whose 24 members are already frozen with their own semantics
// version, so the mapping below names that vocabulary rather than copying it.

// Every scalar operation a Math node offers, in the order every surface offers them: the binary
// arithmetic first, then the combining forms, then the unary shaping operations, then the rounding
// family, then the interval operations. This is the frozen ScalarPrimitive set verbatim --
// Invalid excluded, because it names the absence of an operation rather than one.
inline constexpr std::array<core::primitives::ScalarPrimitive, 24> kScalarOperations{
    core::primitives::ScalarPrimitive::Add,         core::primitives::ScalarPrimitive::Subtract,
    core::primitives::ScalarPrimitive::Multiply,    core::primitives::ScalarPrimitive::Divide,
    core::primitives::ScalarPrimitive::MultiplyAdd, core::primitives::ScalarPrimitive::Minimum,
    core::primitives::ScalarPrimitive::Maximum,     core::primitives::ScalarPrimitive::Clamp,
    core::primitives::ScalarPrimitive::Remap,       core::primitives::ScalarPrimitive::Mix,
    core::primitives::ScalarPrimitive::Absolute,    core::primitives::ScalarPrimitive::Negate,
    core::primitives::ScalarPrimitive::Sign,        core::primitives::ScalarPrimitive::Reciprocal,
    core::primitives::ScalarPrimitive::SquareRoot,  core::primitives::ScalarPrimitive::Floor,
    core::primitives::ScalarPrimitive::Ceiling,     core::primitives::ScalarPrimitive::Round,
    core::primitives::ScalarPrimitive::Truncate,    core::primitives::ScalarPrimitive::Fraction,
    core::primitives::ScalarPrimitive::Modulo,      core::primitives::ScalarPrimitive::Step,
    core::primitives::ScalarPrimitive::Smoothstep,  core::primitives::ScalarPrimitive::Smootherstep,
};

inline constexpr auto kDefaultScalarOperation = core::primitives::ScalarPrimitive::Add;

[[nodiscard]] constexpr std::int64_t
scalarOperationStoredValue(const core::primitives::ScalarPrimitive operation) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint8_t>(operation));
}

[[nodiscard]] constexpr std::optional<core::primitives::ScalarPrimitive>
scalarOperationFromStoredValue(const std::int64_t value) noexcept {
    for (const auto operation : kScalarOperations) {
        if (scalarOperationStoredValue(operation) == value) {
            return operation;
        }
    }
    return std::nullopt;
}

// How many scalar operands a Math node's operation actually reads. The node declares three input
// sockets and three operands, and the unused trailing ones are hidden on the card and ignored by
// the kernel rather than defaulted to something the operation would silently fold in.
[[nodiscard]] constexpr std::uint8_t
scalarOperationOperandCount(const core::primitives::ScalarPrimitive operation) noexcept {
    const auto* signature = core::primitives::scalarPrimitiveSignature(operation);
    return signature == nullptr ? 0 : signature->inputCount;
}

// The widest operand list any Math operation reads (Remap's five). The node declares exactly this
// many operand sockets so one definition serves every operation in the vocabulary.
inline constexpr std::uint8_t kMaximumScalarOperationOperands = 5;

// Vector Math: the operations whose result is a VECTOR of the operand's own width. The reductions
// (length, dot product, distance) are not here -- their result is a scalar, and a socket's kind is
// fixed by its definition, so they are separate node types rather than one node whose output socket
// silently retypes itself.
enum class VectorOperation : std::uint8_t {
    Add = 0,
    Subtract = 1,
    Multiply = 2,
    Divide = 3,
    // The vector scaled by the node's own scalar operand.
    Scale = 4,
    Normalize = 5,
    // Three components only: a two-component cross product is not a vector.
    CrossProduct = 6,
};

inline constexpr std::array<VectorOperation, 7> kVectorOperations{
    VectorOperation::Add,          VectorOperation::Subtract, VectorOperation::Multiply,
    VectorOperation::Divide,       VectorOperation::Scale,    VectorOperation::Normalize,
    VectorOperation::CrossProduct,
};

inline constexpr VectorOperation kDefaultVectorOperation = VectorOperation::Add;

[[nodiscard]] constexpr std::int64_t
vectorOperationStoredValue(const VectorOperation operation) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint8_t>(operation));
}

[[nodiscard]] constexpr std::optional<VectorOperation>
vectorOperationFromStoredValue(const std::int64_t value) noexcept {
    for (const auto operation : kVectorOperations) {
        if (vectorOperationStoredValue(operation) == value) {
            return operation;
        }
    }
    return std::nullopt;
}

// Which operations are defined at a given vector width. Cross product is the only width-sensitive
// one, and the document refuses a three-component operation on a two-component node rather than
// substituting something else.
[[nodiscard]] constexpr bool isVectorOperationValidAt(const VectorOperation operation,
                                                      const std::uint8_t components) noexcept {
    if (components < core::primitives::kMinimumVectorComponents ||
        components > core::primitives::kMaximumVectorComponents) {
        return false;
    }
    return operation != VectorOperation::CrossProduct ||
           components == core::primitives::kMaximumVectorComponents;
}

// The vector reductions -- the operations whose result is one scalar.
enum class VectorReduction : std::uint8_t {
    Length = 0,
    DotProduct = 1,
    Distance = 2,
};

inline constexpr std::array<VectorReduction, 3> kVectorReductions{
    VectorReduction::Length, VectorReduction::DotProduct, VectorReduction::Distance};

inline constexpr VectorReduction kDefaultVectorReduction = VectorReduction::Length;

[[nodiscard]] constexpr std::int64_t
vectorReductionStoredValue(const VectorReduction reduction) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint8_t>(reduction));
}

[[nodiscard]] constexpr std::optional<VectorReduction>
vectorReductionFromStoredValue(const std::int64_t value) noexcept {
    for (const auto reduction : kVectorReductions) {
        if (vectorReductionStoredValue(reduction) == value) {
            return reduction;
        }
    }
    return std::nullopt;
}

[[nodiscard]] constexpr core::primitives::VectorPrimitive
vectorReductionPrimitive(const VectorReduction reduction) noexcept {
    switch (reduction) {
    case VectorReduction::Length:
        return core::primitives::VectorPrimitive::Length;
    case VectorReduction::DotProduct:
        return core::primitives::VectorPrimitive::DotProduct;
    case VectorReduction::Distance:
        return core::primitives::VectorPrimitive::Distance;
    }
    return core::primitives::VectorPrimitive::Invalid;
}

// Map Range's interpolation. Stepped is deliberately absent: it would need a kernel that does not
// exist, and an operation with no kernel is a promise the evaluator cannot keep.
enum class RangeInterpolation : std::uint8_t {
    Linear = 0,
    Smoothstep = 1,
    Smootherstep = 2,
};

inline constexpr std::array<RangeInterpolation, 3> kRangeInterpolations{
    RangeInterpolation::Linear, RangeInterpolation::Smoothstep, RangeInterpolation::Smootherstep};

inline constexpr RangeInterpolation kDefaultRangeInterpolation = RangeInterpolation::Linear;

[[nodiscard]] constexpr std::int64_t
rangeInterpolationStoredValue(const RangeInterpolation interpolation) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint8_t>(interpolation));
}

[[nodiscard]] constexpr std::optional<RangeInterpolation>
rangeInterpolationFromStoredValue(const std::int64_t value) noexcept {
    for (const auto interpolation : kRangeInterpolations) {
        if (rangeInterpolationStoredValue(interpolation) == value) {
            return interpolation;
        }
    }
    return std::nullopt;
}

// Compare's predicate. Equal and NotEqual read the node's own epsilon operand; the four ordering
// predicates do not, because an ordering has no tolerance to spend.
enum class CompareOperation : std::uint8_t {
    Equal = 0,
    NotEqual = 1,
    Less = 2,
    LessOrEqual = 3,
    Greater = 4,
    GreaterOrEqual = 5,
};

inline constexpr std::array<CompareOperation, 6> kCompareOperations{
    CompareOperation::Equal,       CompareOperation::NotEqual, CompareOperation::Less,
    CompareOperation::LessOrEqual, CompareOperation::Greater,  CompareOperation::GreaterOrEqual};

inline constexpr CompareOperation kDefaultCompareOperation = CompareOperation::Greater;

// Blender's own default equality tolerance, kept rather than invented so a comparison authored in
// Bloom behaves the way an artist coming from a geometry-nodes graph expects.
inline constexpr double kDefaultCompareEpsilon = 0.0001;

[[nodiscard]] constexpr std::int64_t
compareOperationStoredValue(const CompareOperation operation) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint8_t>(operation));
}

[[nodiscard]] constexpr std::optional<CompareOperation>
compareOperationFromStoredValue(const std::int64_t value) noexcept {
    for (const auto operation : kCompareOperations) {
        if (compareOperationStoredValue(operation) == value) {
            return operation;
        }
    }
    return std::nullopt;
}

// Whether this predicate reads the epsilon operand at all.
[[nodiscard]] constexpr bool
compareOperationUsesEpsilon(const CompareOperation operation) noexcept {
    return operation == CompareOperation::Equal || operation == CompareOperation::NotEqual;
}

} // namespace bloom::document
