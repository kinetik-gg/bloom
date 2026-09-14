#pragma once

#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/validation.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::document {

inline constexpr std::string_view kSolidColorParameterSchemaKey = "bloom.solid.color";
inline constexpr std::string_view kSolidWidthParameterSchemaKey = "bloom.solid.width";
inline constexpr std::string_view kSolidHeightParameterSchemaKey = "bloom.solid.height";
inline constexpr std::string_view kTextParameterSchemaKey = "bloom.text.content";
inline constexpr std::string_view kTextSizeParameterSchemaKey = "bloom.text.size";
// Alignment: 0 = Left, 1 = Center, 2 = Right. Line height is a positive em multiplier;
// letter spacing is a finite number of full-resolution pixels between adjacent glyphs.
inline constexpr std::string_view kTextAlignmentParameterSchemaKey = "bloom.text.alignment";
inline constexpr std::string_view kTextLineHeightParameterSchemaKey = "bloom.text.line-height";
inline constexpr std::string_view kTextLetterSpacingParameterSchemaKey =
    "bloom.text.letter-spacing";
inline constexpr std::string_view kTextColorParameterSchemaKey = "bloom.text.color";
inline constexpr std::string_view kPositionParameterSchemaKey = "bloom.transform.position";
// Offset from the centre of the source's local content bounds, in full-resolution pixels.
// Position places that anchor in composition space; scale and rotation act about it.
// New Layer v4 follows this contract. Persisted Layer v3 retains frame-relative placement.
inline constexpr std::string_view kAnchorParameterSchemaKey = "bloom.transform.anchor";
// Unitless per-axis scale factor. 1 is unscaled; a negative factor mirrors the axis; 0 collapses
// the layer to nothing, which evaluation renders as an empty layer rather than an error.
inline constexpr std::string_view kScaleParameterSchemaKey = "bloom.transform.scale";
// Clockwise screen rotation in DEGREES (Bloom's y axis points down, so a positive value turns the
// layer clockwise, matching every timeline-based compositor). Unbounded: 450 and 90 evaluate
// identically but are distinct authored values so a rotation curve can wind past a full turn.
inline constexpr std::string_view kRotationParameterSchemaKey = "bloom.transform.rotation";
inline constexpr std::string_view kOpacityParameterSchemaKey = "bloom.layer.opacity";
// How the layer combines with what is beneath it in the stack. The value kind is a small integer
// carrying core::BlendMode's own durable mapping (Normal = 0; see bloom/core/blend_mode.hpp and
// docs/architecture/color-management.md, "Blend modes"), not a name: a stored name would make every
// saved document depend on a spelling, and the enumeration is closed and owned by Bloom. NOT
// animatable -- a blend mode is a discrete choice with no meaningful value between two modes, so
// neither isScalarAnimatableSchemaKey() nor isVec2AnimatableSchemaKey() accepts it and no curve can
// be created over it.
inline constexpr std::string_view kBlendModeParameterSchemaKey = "bloom.layer.blend-mode";

// ---------------------------------------------------------------------------------------------
// Value-graph parameter schemas (task S7).
//
// Every one of these is a value a node AUTHORS inline and can equally receive from a driver, which
// is why each is a parameter schema rather than a port-only concept: the inline widget writes the
// parameter's constant, and linking a value-graph output into the matching socket replaces that
// constant with a driver binding. The two are the same storage, never two parallel ones.
//
// The LITERAL value schemas whose kind already has a curve -- Scalar, Vector 2 and Colour -- are
// animatable (task FIX1, item G): a Scalar node the artist keys 0 to 1 over ten frames, driving a
// layer's opacity, is the whole of what "primitives should be able to be animated too" asks for,
// and it reuses the existing curve kinds, commands and sampling rather than inventing a second
// authoring path. Vector 3, Integer, Boolean and String literals stay constant-or-driven, because
// each needs a curve KIND that does not exist yet.
//
// The generic OPERAND schemas below stay constant-or-driven whatever their kind. An operand is a
// value a node reads, and the artist already shapes it with a curve upstream -- wire an animated
// Scalar node into the socket -- so giving it its own curve as well would be a second authoring
// path to the same picture.
// ---------------------------------------------------------------------------------------------

// The literal Value nodes. One schema per authored kind, because the kind IS the meaning here:
// "this node carries a Scalar" is the whole of what a Scalar node's parameter says.
inline constexpr std::string_view kIntegerValueParameterSchemaKey = "bloom.value.integer";
inline constexpr std::string_view kScalarValueParameterSchemaKey = "bloom.value.scalar";
inline constexpr std::string_view kVector2ValueParameterSchemaKey = "bloom.value.vector2";
inline constexpr std::string_view kVector3ValueParameterSchemaKey = "bloom.value.vector3";
inline constexpr std::string_view kStringValueParameterSchemaKey = "bloom.value.string";
inline constexpr std::string_view kColorValueParameterSchemaKey = "bloom.value.color";
inline constexpr std::string_view kBooleanValueParameterSchemaKey = "bloom.value.boolean";

// Generic operands, shared by every node in the library that reads a value of that kind. One schema
// per KIND rather than one per node-and-role: a Math node's "a" and a Mix node's "a" mean exactly
// the same thing -- an unconstrained finite scalar -- and two schema keys saying that would be two
// spellings of one domain. The node's own role names which operand it is; the schema names what a
// value of that kind may be.
inline constexpr std::string_view kScalarOperandParameterSchemaKey = "bloom.operand.scalar";
inline constexpr std::string_view kIntegerOperandParameterSchemaKey = "bloom.operand.integer";
inline constexpr std::string_view kBooleanOperandParameterSchemaKey = "bloom.operand.boolean";
inline constexpr std::string_view kVector2OperandParameterSchemaKey = "bloom.operand.vector2";
inline constexpr std::string_view kVector3OperandParameterSchemaKey = "bloom.operand.vector3";
inline constexpr std::string_view kColorOperandParameterSchemaKey = "bloom.operand.color";
inline constexpr std::string_view kStringOperandParameterSchemaKey = "bloom.operand.string";

// The inline selectors. Each is an Integer under its own closed mapping (value_operations.hpp), for
// the same reason the blend mode is: a stored name would make every saved document depend on a
// spelling. None of them is socket-linkable -- the selector is what decides which kernel the plan
// compiles, so it has to be known at compile time, not delivered per frame.
inline constexpr std::string_view kScalarOperationParameterSchemaKey = "bloom.math.operation";
inline constexpr std::string_view kVectorOperationParameterSchemaKey =
    "bloom.vector-math.operation";
inline constexpr std::string_view kVectorReductionParameterSchemaKey =
    "bloom.vector-reduce.operation";
inline constexpr std::string_view kRangeInterpolationParameterSchemaKey =
    "bloom.map-range.interpolation";
inline constexpr std::string_view kCompareOperationParameterSchemaKey = "bloom.compare.operation";
// A Math or Map Range node's result-clamping toggle. Boolean, and deliberately NOT linkable for the
// same reason as the selectors above: it changes the shape of the compiled operation.
inline constexpr std::string_view kClampResultParameterSchemaKey = "bloom.math.clamp-result";
// Compare's equality tolerance. Its own schema rather than the generic scalar operand because the
// domain differs: a negative tolerance is not a tolerance.
inline constexpr std::string_view kCompareEpsilonParameterSchemaKey = "bloom.compare.epsilon";

// The initial solid schema owns straight/unassociated RGBA authoring values in this encoding.
// Evaluation converts them to the canonical premultiplied image representation. The text color
// schema (kTextColorParameterSchemaKey) authors in exactly this same encoding with exactly the same
// straight-alpha meaning -- a text color is a solid color that glyph coverage then scales -- so it
// reuses this one constant rather than declaring a second, identical encoding name.
inline constexpr std::string_view kSolidColorEncoding = "bloom.reference.linear-srgb";

// Text size bounds, in pixels per em, owned by the text size parameter schema. The lower bound is
// exclusive (a zero or negative em size has no meaning); the upper bound is inclusive and is the
// same value render::kMaximumTextPixelSize imposes on the rasterizer, so a document the schema
// accepts is always a document the reference rasterizer can draw. src/runtime, the one module that
// sees both headers, static_asserts the two equal rather than leaving them to drift -- see
// src/runtime/cpu_composition_evaluator.cpp.
inline constexpr double kMaximumTextSizePixels = 4096.0;
// The size a newly authored text layer starts at.
inline constexpr double kDefaultTextSizePixels = 72.0;

struct Vec2d {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const Vec2d&, const Vec2d&) = default;
};

// The third authoring vector width (task S7). A genuinely new authoring type rather than a reuse of
// Vec2d or Color4d: a Color is not a Vec4 under
// docs/architecture/evaluation-primitives.md's Type Binding rule, and by the same reasoning a
// three-component vector is not a two-component one with a spare field. It has no animation curve
// kind yet -- no schema declares a Vec3d parameter animatable -- so it is constant-or-driven only.
struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    friend bool operator==(const Vec3d&, const Vec3d&) = default;
};

// The Layer Output transform defaults, in one place so the node definition registry, the layer
// creation commands, and Project I/O's version-1 node upgrade cannot drift apart. Together they are
// the identity transform: no anchor offset, no scaling, no rotation.
inline constexpr Vec2d kDefaultAnchor{};
inline constexpr Vec2d kDefaultScale{1.0, 1.0};
inline constexpr double kDefaultRotationDegrees = 0.0;
// The blend-mode default, in the same one place and for the same reason: Normal, stored as the
// integer core::BlendMode's mapping gives it, so a newly authored layer and an upgraded version-1
// or version-2 Layer Output agree exactly.
inline constexpr std::int64_t kDefaultBlendModeValue =
    core::blendModeStoredValue(core::kDefaultBlendMode);

// The animatable schema set, split by the curve value kind each member requires. These predicates
// are the single authority every layer asks -- document validation, the animation commands, and the
// snapshot compiler's override gate -- so the set cannot be widened in one place and stay narrow in
// another. A schema key that satisfies none of them is constant-only, which after task S5 is
// exactly the text CONTENT schema (a String has no interpolation) and every unregistered key.
[[nodiscard]] constexpr bool isVec2AnimatableSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kPositionParameterSchemaKey || schemaKey == kAnchorParameterSchemaKey ||
           schemaKey == kScaleParameterSchemaKey || schemaKey == kVector2ValueParameterSchemaKey;
}

[[nodiscard]] constexpr bool
isScalarAnimatableSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kOpacityParameterSchemaKey || schemaKey == kRotationParameterSchemaKey ||
           schemaKey == kTextSizeParameterSchemaKey ||
           schemaKey == kScalarValueParameterSchemaKey ||
           schemaKey == kSolidWidthParameterSchemaKey ||
           schemaKey == kSolidHeightParameterSchemaKey ||
           schemaKey == kTextLineHeightParameterSchemaKey ||
           schemaKey == kTextLetterSpacingParameterSchemaKey;
}

// The Color4d-valued animatable schemas (task S5): a solid's colour and a text layer's colour.
// Both author straight RGBA in kSolidColorEncoding, so one curve kind serves both.
//
// Task FIX1, item G: the value LIBRARY's own literals join all three sets. A Scalar node, a Vector
// 2 node and a Colour node hold exactly the kinds these curves already carry, so animating one is
// the same gesture, the same command, and the same sampling the layer parameters already have --
// and a driven parameter downstream of one reads the sampled value per frame. Vector 3, Integer,
// Boolean and String stay constant-or-driven: each would need a curve KIND that does not exist (a
// Vec3 curve, or a Hold-only integer/boolean curve), which is a document-format change with its own
// schema ladder step rather than a widening of this set.
[[nodiscard]] constexpr bool
isColor4AnimatableSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kSolidColorParameterSchemaKey ||
           schemaKey == kTextColorParameterSchemaKey || schemaKey == kColorValueParameterSchemaKey;
}

[[nodiscard]] constexpr bool isAnimatableSchemaKey(const std::string_view schemaKey) noexcept {
    return isVec2AnimatableSchemaKey(schemaKey) || isScalarAnimatableSchemaKey(schemaKey) ||
           isColor4AnimatableSchemaKey(schemaKey);
}

// Whether a scalar value under this schema is confined to [0, 1]. Opacity is the only one: rotation
// degrees must be free to wind past a full turn in either direction, so the unit domain belongs to
// the schema rather than to "scalar values" as a class.
[[nodiscard]] constexpr bool hasUnitDomainSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kOpacityParameterSchemaKey;
}

// Whether a scalar value under this schema is confined to the text size domain (0, kMaximumText-
// SizePixels]. Same reasoning as the unit domain above: the bound belongs to the schema, not to
// "scalar" as a class, and task S5 made text size animatable so a KEY has to satisfy it too.
[[nodiscard]] constexpr bool hasTextSizeDomainSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kTextSizeParameterSchemaKey;
}

// The ONE scalar-domain gate every layer asks (document validation, the animation commands, the
// evaluator's per-key check, the session's write paths), so a constant and a keyframe under the
// same schema can never be admitted on different terms. `value` must already be finite; finiteness
// is checked separately by each caller because the diagnostic it produces differs.
[[nodiscard]] constexpr bool isScalarWithinSchemaDomain(const std::string_view schemaKey,
                                                        const double value) noexcept {
    if (schemaKey == kSolidWidthParameterSchemaKey || schemaKey == kSolidHeightParameterSchemaKey)
        return value >= 1.0;
    if (schemaKey == kTextLineHeightParameterSchemaKey)
        return value > 0.0;
    if (hasUnitDomainSchemaKey(schemaKey)) {
        return value >= 0.0 && value <= 1.0;
    }
    if (hasTextSizeDomainSchemaKey(schemaKey)) {
        return value > 0.0 && value <= kMaximumTextSizePixels;
    }
    return true;
}

using ParameterValue = std::variant<bool, std::int64_t, double, Vec2d, Vec3d, core::Color4d,
                                    std::string, core::RationalTime>;

struct ConstantValueSource {
    ParameterValue value;

    friend bool operator==(const ConstantValueSource&, const ConstantValueSource&) = default;
};

struct AnimationCurveSource {
    AnimationCurveId curveId;

    friend bool operator==(const AnimationCurveSource&, const AnimationCurveSource&) = default;
};

// A parameter driven by a value-graph node's output (task S7). Durable, and structurally identical
// to an ordinary OutputPortRef -- it just lands in parameter-address space instead of on an input
// port, which is the whole of what a driver is.
//
// Deliberately inline rather than an id into a separate driver table: a table would add a document
// collection, an encoding, and an id space whose only content is this pair, and a dangling id would
// be a new failure mode the pair cannot have. DriverBindingId still exists as an allocated id space
// (ids.hpp) because a saved document's high-water marks are durable and this slice is not the place
// to change the allocator's shape; no live record uses it.
//
// Cycles are not a new concept either: CanonicalGraph::validate() feeds these bindings into the
// same adjacency the edge set builds, so a driver that closes a loop is refused by the one existing
// same-time cycle check rather than by a second rule that could disagree with it.
struct DriverBindingSource {
    NodeId sourceNodeId;
    std::string outputPort;

    friend bool operator==(const DriverBindingSource&, const DriverBindingSource&) = default;
};

using ParameterSource =
    std::variant<ConstantValueSource, AnimationCurveSource, DriverBindingSource>;

struct ParameterRecord {
    ParameterId id;
    std::string schemaKey;
    ParameterSource source;

    friend bool operator==(const ParameterRecord&, const ParameterRecord&) = default;
};

struct ParameterBinding {
    std::string role;
    ParameterId parameterId;

    friend bool operator==(const ParameterBinding&, const ParameterBinding&) = default;
};

class ParameterStore final {
  public:
    [[nodiscard]] std::span<const ParameterRecord> records() const noexcept { return records_; }
    [[nodiscard]] const ParameterRecord* find(ParameterId id) const noexcept;

    [[nodiscard]] bool insert(ParameterRecord record);
    [[nodiscard]] bool erase(ParameterId id);
    [[nodiscard]] bool setSource(ParameterId id, ParameterSource source);

    [[nodiscard]] ValidationResult validate() const;

  private:
    std::vector<ParameterRecord> records_;
};

} // namespace bloom::document
