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
inline constexpr std::string_view kTextParameterSchemaKey = "bloom.text.content";
inline constexpr std::string_view kTextSizeParameterSchemaKey = "bloom.text.size";
inline constexpr std::string_view kTextColorParameterSchemaKey = "bloom.text.color";
inline constexpr std::string_view kPositionParameterSchemaKey = "bloom.transform.position";
// Layer-space anchor, in full-resolution composition pixels, measured from the LAYER CENTRE -- so
// the default Vec2d{} is exactly "the layer centre" without the schema needing to know any
// composition format. Scale/rotation turn about this point and it is the one point a layer's
// position parameter places, which is why the two share an origin: position puts the layer centre
// at its value, so anchor {0, 0} is that same point. See docs/architecture/layer-graph-model.md,
// "Layer Transform".
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

// The animatable schema set, split by the curve value kind each member requires. These three
// predicates are the single authority every layer asks -- document validation, the animation
// commands, and the snapshot compiler's override gate -- so the set cannot be widened in one place
// and stay narrow in another. A schema key that satisfies neither predicate is constant-only, which
// is every source parameter (solid colour, text content/size/colour) and every unregistered key.
[[nodiscard]] constexpr bool isVec2AnimatableSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kPositionParameterSchemaKey || schemaKey == kAnchorParameterSchemaKey ||
           schemaKey == kScaleParameterSchemaKey;
}

[[nodiscard]] constexpr bool
isScalarAnimatableSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kOpacityParameterSchemaKey || schemaKey == kRotationParameterSchemaKey;
}

// Whether a scalar value under this schema is confined to [0, 1]. Opacity is the only one: rotation
// degrees must be free to wind past a full turn in either direction, so the unit domain belongs to
// the schema rather than to "scalar values" as a class.
[[nodiscard]] constexpr bool hasUnitDomainSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kOpacityParameterSchemaKey;
}

using ParameterValue =
    std::variant<bool, std::int64_t, double, Vec2d, core::Color4d, std::string, core::RationalTime>;

struct ConstantValueSource {
    ParameterValue value;

    friend bool operator==(const ConstantValueSource&, const ConstantValueSource&) = default;
};

struct AnimationCurveSource {
    AnimationCurveId curveId;

    friend bool operator==(const AnimationCurveSource&, const AnimationCurveSource&) = default;
};

struct DriverBindingSource {
    DriverBindingId driverId;

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
