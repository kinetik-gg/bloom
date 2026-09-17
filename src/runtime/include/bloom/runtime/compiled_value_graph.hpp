#pragma once

#include <bloom/core/color.hpp>
#include <bloom/core/scalar_primitives.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/value_operations.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/runtime/compiled_curves.hpp>
#include <bloom/runtime/operation_index.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace bloom::runtime {

// The value graph: a second, much smaller compiled program that runs once per frame. Ordinary
// values run before image operations; Layer Bounds readouts and their dependants run in the
// post-image pass.
//
// It is deliberately parallel to the image chain rather than part of it. CompiledOperation is a
// closed variant of Image-producing steps addressed by OperationIndex, and every one of them is
// per-pixel work; these are per-frame numbers. Interleaving them would mean one topological order
// over two kinds of work that never share an address space, and an evaluator arm for a "step" that
// produces no pixels at all. So CompiledOperation and its lowering switch are untouched, and the
// only change to an existing type is one more alternative on each parameter operand.
//
// WHY THE VALUES ARE NOT STATICALLY TYPED HERE. The image chain's operands are typed
// (CompiledScalarParameter, CompiledVec2Parameter, CompiledColorParameter) because there are three
// of them and each is read by one field of one operation. The value graph has seven kinds and
// thirteen kernels, and nearly every kernel is kind-agnostic -- a Switch over Scalar and a Switch
// over Colour are one operation on different storage. A typed operand per kind would be a
// seven-by-thirteen matrix of structs that all say the same thing. So the lowered form carries a
// uniform CompiledValue and the TYPING LIVES WHERE IT BELONGS: the document layer, whose node
// definitions declare every socket's kind and whose validation refuses a mismatched link, and the
// compiler, which inserts an explicit promotion operation rather than ever reinterpreting a value.
// The evaluator still checks what it reads -- a kernel given the wrong alternative reports a scoped
// diagnostic and substitutes the node's documented fallback -- so a malformed plan is diagnosed,
// not trusted.

// One value a value-graph output can hold. The same closed set as document::ParameterValue minus
// RationalTime: a rational is how Bloom names an INSTANT, and a value graph carries quantities. A
// Time node's seconds output is a Scalar for exactly that reason -- it is a number an artist does
// arithmetic on, not a timeline address.
using CompiledValue = std::variant<bool, std::int64_t, double, document::Vec2d, document::Vec3d,
                                   core::Color4d, std::string>;

// Where one value lands. The plan's value outputs are a flat table: every operation claims a
// consecutive run of entries in it, so a Separate RGBA's four channels and a Time node's two units
// are each addressable on their own without a second level of indexing.
class ValueOutputIndex final {
  public:
    [[nodiscard]] static constexpr ValueOutputIndex fromRaw(const std::size_t value) noexcept {
        return ValueOutputIndex(value);
    }

    [[nodiscard]] constexpr std::size_t value() const noexcept { return value_; }
    friend constexpr auto operator<=>(const ValueOutputIndex&,
                                      const ValueOutputIndex&) noexcept = default;

  private:
    explicit constexpr ValueOutputIndex(const std::size_t value) noexcept : value_(value) {}

    std::size_t value_ = 0;
};

// One operand of a value operation: the authored constant, or an earlier value output.
//
// `id` is the parameter the constant came from, carried for the same reason every image-chain
// operand carries one -- so a diagnostic can name the exact parameter that failed rather than only
// the node. It is invalid for the one operand with no parameter behind it, a Reroute's
// pass-through.
// The three curve alternatives are task FIX1, item G: a literal Scalar, Vector 2 or Colour node may
// carry a curve of its own, so its authored value is sampled per frame exactly as a layer
// parameter's is -- same curve kinds, same sampler, same semantics version -- and a downstream
// driven parameter reads the sampled number rather than a constant. A literal of a kind with no
// curve (Vector 3, Integer, Boolean, String) and every generic OPERAND stay constant-or-output.
struct CompiledValueOperand final {
    document::ParameterId id;
    std::variant<CompiledValue, ValueOutputIndex, ScalarCurveIndex, Vec2CurveIndex, Vec3CurveIndex,
                 Color4CurveIndex>
        source;

    friend bool operator==(const CompiledValueOperand&, const CompiledValueOperand&) = default;
};

// A literal Value node, a Reroute, and nothing else: one operand straight through to one output.
// The two are the same lowered operation because they are the same lowered operation -- a literal
// is a pass-through of an authored constant, and a Reroute is a pass-through of someone else's
// value.
struct CompiledValuePassthrough final {
    CompiledValueOperand value;

    friend bool operator==(const CompiledValuePassthrough&,
                           const CompiledValuePassthrough&) = default;
};

// Zero inputs, two outputs: the frame being rendered, in seconds and as a frame number. Filled from
// the SAME request time the plan's animation curves are sampled at, so a Time node and a curve in
// one plan can never disagree about when "now" is.
struct CompiledValueTime final {
    friend bool operator==(const CompiledValueTime&, const CompiledValueTime&) = default;
};

// The frozen 24-operation scalar vocabulary. `operands` is exactly as long as the chosen
// operation's own arity -- the node declares five sockets, and the compiler lowers only the ones
// the operation reads, so the kernel is never handed an operand it would silently fold in.
struct CompiledValueScalarMath final {
    core::primitives::ScalarPrimitive operation = core::primitives::ScalarPrimitive::Add;
    bool clampResult = false;
    std::vector<CompiledValueOperand> operands;

    friend bool operator==(const CompiledValueScalarMath&,
                           const CompiledValueScalarMath&) = default;
};

// Componentwise vector arithmetic plus Scale, Normalize and Cross. The componentwise operations
// route through the scalar kernel one component at a time; only the three that reduce or
// renormalize across components reach the vector tranche.
struct CompiledValueVectorMath final {
    document::VectorOperation operation = document::VectorOperation::Add;
    std::uint8_t components = 2;
    CompiledValueOperand left;
    CompiledValueOperand right;
    CompiledValueOperand factor;

    friend bool operator==(const CompiledValueVectorMath&,
                           const CompiledValueVectorMath&) = default;
};

struct CompiledValueVectorReduce final {
    document::VectorReduction reduction = document::VectorReduction::Length;
    std::uint8_t components = 2;
    CompiledValueOperand left;
    CompiledValueOperand right;

    friend bool operator==(const CompiledValueVectorReduce&,
                           const CompiledValueVectorReduce&) = default;
};

struct CompiledValueMapRange final {
    document::RangeInterpolation interpolation = document::RangeInterpolation::Linear;
    bool clampResult = false;
    CompiledValueOperand value;
    CompiledValueOperand fromMinimum;
    CompiledValueOperand fromMaximum;
    CompiledValueOperand toMinimum;
    CompiledValueOperand toMaximum;

    friend bool operator==(const CompiledValueMapRange&, const CompiledValueMapRange&) = default;
};

struct CompiledValueClamp final {
    CompiledValueOperand value;
    CompiledValueOperand minimum;
    CompiledValueOperand maximum;

    friend bool operator==(const CompiledValueClamp&, const CompiledValueClamp&) = default;
};

// `color` selects straight-RGBA channel-independent interpolation over the plain scalar form. Not a
// vector mix of four floats: a Color is not a Vec4, so mixing one is its own operation with no
// gamut or OCIO step implied.
struct CompiledValueMix final {
    bool color = false;
    CompiledValueOperand factor;
    CompiledValueOperand start;
    CompiledValueOperand end;

    friend bool operator==(const CompiledValueMix&, const CompiledValueMix&) = default;
};

struct CompiledValueCompare final {
    document::CompareOperation operation = document::CompareOperation::Greater;
    CompiledValueOperand left;
    CompiledValueOperand right;
    CompiledValueOperand epsilon;

    friend bool operator==(const CompiledValueCompare&, const CompiledValueCompare&) = default;
};

// Both branches are lowered, and only the selected one is READ. The unselected branch is not
// evaluated for its own sake -- but if it names an earlier value output, that output was already
// computed by the pass, because a value graph is evaluated in topological order rather than pulled
// on demand. Cheap enough to be the right trade at this size, and it keeps the pass a single linear
// sweep with no re-entrancy.
struct CompiledValueSwitch final {
    CompiledValueOperand condition;
    CompiledValueOperand ifFalse;
    CompiledValueOperand ifTrue;

    friend bool operator==(const CompiledValueSwitch&, const CompiledValueSwitch&) = default;
};

// One vector or colour in, its components out, in declaration order.
struct CompiledValueSeparate final {
    CompiledValueOperand value;
    std::uint8_t componentCount = 2;
    bool color = false;

    friend bool operator==(const CompiledValueSeparate&, const CompiledValueSeparate&) = default;
};

struct CompiledValueCombine final {
    std::vector<CompiledValueOperand> components;
    bool color = false;

    friend bool operator==(const CompiledValueCombine&, const CompiledValueCombine&) = default;
};

// Deterministic: the same seed is the same value in every process and every frame. A Random node
// varies over time only when something wires a changing number into its seed.
struct CompiledValueRandom final {
    CompiledValueOperand seed;
    CompiledValueOperand minimum;
    CompiledValueOperand maximum;

    friend bool operator==(const CompiledValueRandom&, const CompiledValueRandom&) = default;
};

// Which widening a promotion performs. Every promotion the connect-time whitelist accepts becomes
// one of these, compiled as its OWN operation rather than applied silently where the value is read:
// a widening that shows up in a plan dump is a widening that can be diagnosed.
enum class ValuePromotion : std::uint8_t {
    IntegerToScalar,
    BooleanToInteger,
    BooleanToScalar,
    ScalarToVector2,
    ScalarToVector3,
};

struct CompiledValuePromotion final {
    ValuePromotion promotion = ValuePromotion::IntegerToScalar;
    CompiledValueOperand value;

    friend bool operator==(const CompiledValuePromotion&, const CompiledValuePromotion&) = default;
};

// Task UTIL-1's whole library, as one kernel. `operation` names WHICH conversion, string
// operation, numeric operation or readout this is; `operands` are the node's socket-backed inputs
// in its descriptor's order, and `selectors` its inline selectors in theirs. Both lists are exactly
// as long as the descriptor says, so the kernel indexes them positionally rather than searching by
// name once per frame.
//
// One variant alternative rather than sixty is the same trade CompiledValueSwitch already makes
// across seven socket kinds: these operations differ in what they COMPUTE, not in how they are
// addressed, and sixty structs holding the same two vectors would be sixty places for the operand
// order to drift from the table that produced it.
struct CompiledValueUtility final {
    document::ValueUtilityKernel operation = document::ValueUtilityKernel::ScalarToString;
    std::vector<CompiledValueOperand> operands;
    std::vector<std::int64_t> selectors;

    friend bool operator==(const CompiledValueUtility&, const CompiledValueUtility&) = default;
};

// Image bounds are produced by the image pass, so this readout is the one value kernel whose
// source is an image operation address rather than a value operand. It expands to four Vec2
// outputs: size, origin, authored anchor, and evaluated center.
struct CompiledBoundsReadout final {
    OperationIndex operationIndex;

    friend bool operator==(const CompiledBoundsReadout&, const CompiledBoundsReadout&) = default;
};

using CompiledValueKernel =
    std::variant<CompiledValuePassthrough, CompiledValueTime, CompiledValueScalarMath,
                 CompiledValueVectorMath, CompiledValueVectorReduce, CompiledValueMapRange,
                 CompiledValueClamp, CompiledValueMix, CompiledValueCompare, CompiledValueSwitch,
                 CompiledValueSeparate, CompiledValueCombine, CompiledValueRandom,
                 CompiledValuePromotion, CompiledValueUtility, CompiledBoundsReadout>;

// Every operand one kernel reads, in declaration order. Written once, here beside the kernels, so a
// caller that has to walk them -- the evaluator's preflight, which has to know which animation
// curves the value graph references before it can call an unreferenced one a malformed plan -- does
// not carry its own copy of the kernel list that would silently go stale when a kernel is added.
template <typename Visit> void forEachValueOperand(const CompiledValueKernel& kernel, Visit visit) {
    std::visit(
        [&visit](const auto& step) {
            using Step = std::decay_t<decltype(step)>;
            if constexpr (std::is_same_v<Step, CompiledValueTime> ||
                          std::is_same_v<Step, CompiledBoundsReadout>) {
                // The one kernel with no operands: its value is the frame being rendered.
            } else if constexpr (std::is_same_v<Step, CompiledValuePassthrough> ||
                                 std::is_same_v<Step, CompiledValueSeparate> ||
                                 std::is_same_v<Step, CompiledValuePromotion>) {
                visit(step.value);
            } else if constexpr (std::is_same_v<Step, CompiledValueScalarMath> ||
                                 std::is_same_v<Step, CompiledValueUtility>) {
                for (const auto& operand : step.operands)
                    visit(operand);
            } else if constexpr (std::is_same_v<Step, CompiledValueCombine>) {
                for (const auto& operand : step.components)
                    visit(operand);
            } else if constexpr (std::is_same_v<Step, CompiledValueVectorMath>) {
                visit(step.left);
                visit(step.right);
                visit(step.factor);
            } else if constexpr (std::is_same_v<Step, CompiledValueVectorReduce>) {
                visit(step.left);
                visit(step.right);
            } else if constexpr (std::is_same_v<Step, CompiledValueMapRange>) {
                visit(step.value);
                visit(step.fromMinimum);
                visit(step.fromMaximum);
                visit(step.toMinimum);
                visit(step.toMaximum);
            } else if constexpr (std::is_same_v<Step, CompiledValueClamp>) {
                visit(step.value);
                visit(step.minimum);
                visit(step.maximum);
            } else if constexpr (std::is_same_v<Step, CompiledValueMix>) {
                visit(step.factor);
                visit(step.start);
                visit(step.end);
            } else if constexpr (std::is_same_v<Step, CompiledValueCompare>) {
                visit(step.left);
                visit(step.right);
                visit(step.epsilon);
            } else if constexpr (std::is_same_v<Step, CompiledValueSwitch>) {
                visit(step.condition);
                visit(step.ifFalse);
                visit(step.ifTrue);
            } else {
                static_assert(std::is_same_v<Step, CompiledValueRandom>,
                              "every value kernel must enumerate its operands here");
                visit(step.seed);
                visit(step.minimum);
                visit(step.maximum);
            }
        },
        kernel);
}

// One step of the value graph. `firstOutput` and `outputCount` are the run of entries in the plan's
// flat value-output table this step fills; a promotion the compiler synthesized carries no document
// node, so `sourceNodeId` is invalid for exactly those.
struct CompiledValueOperation final {
    document::NodeId sourceNodeId;
    ValueOutputIndex firstOutput;
    std::uint8_t outputCount = 1;
    CompiledValueKernel kernel;
    // A bounds readout and every value operation transitively fed by it run after the image pass.
    // Existing aggregate initializers remain valid because this additive flag has a default.
    bool requiresPostImage = false;

    friend bool operator==(const CompiledValueOperation&, const CompiledValueOperation&) = default;
};

} // namespace bloom::runtime
