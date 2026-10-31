#pragma once

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/validation.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::document {

// The outgoing interpolation of one key's segment.
//
// EaseInOut is a cubic Bezier ease whose control points come from the two keys' own handles
// (below). Their DEFAULTS are the symmetric (1/3, 0) and (2/3, 1) that the mode carried before
// handles existed, and those defaults are what make the ease exact and allocation-free: a cubic
// Bezier whose x control points are 0, 1/3, 2/3, 1 has x(s) == s identically, so the curve
// parameter IS the interval factor and no root finding, iteration, or libm call is needed -- the
// eased factor is the closed-form polynomial 3t^2 - 2t^3 of the exact rational interval factor. A
// key whose handles are both bitwise default therefore samples exactly as it always did. See
// docs/architecture/animation-and-time.md, "Ease In-Out" and "Ease handles".
//
// The artist-facing names follow Blender: Hold is "Constant" and EaseInOut is "Bezier". Their
// enumerator names and values are part of the persisted vocabulary and never move.
//
// Document 1.30 appends Blender's easing equations (Robert Penner's, in Blender's own forms,
// including its exact-endpoint Expo and its blended Elastic). Each reads the segment's LEFT key's
// KeyframeEasingParameters below: which end eases, Back's overshoot, and Elastic's amplitude and
// period. See docs/architecture/animation-and-time.md, "Easing equations".
enum class KeyframeInterpolation : std::uint8_t {
    Hold,
    Linear,
    EaseInOut,
    Sine,
    Quad,
    Cubic,
    Quart,
    Quint,
    Expo,
    Circ,
    Back,
    Bounce,
    Elastic,
};

[[nodiscard]] constexpr bool isValidKeyframeInterpolation(KeyframeInterpolation mode) noexcept {
    return static_cast<std::uint8_t>(mode) <=
           static_cast<std::uint8_t>(KeyframeInterpolation::Elastic);
}

// True for the ten easing equations, which read KeyframeEasingParameters; false for Hold, Linear
// and the handle-driven Bezier.
[[nodiscard]] constexpr bool isEasingInterpolation(KeyframeInterpolation mode) noexcept {
    return mode >= KeyframeInterpolation::Sine && mode <= KeyframeInterpolation::Elastic;
}

// Which end of the segment an easing equation applies to. Auto is Blender's default: Out for the
// "dynamic" equations (Back, Bounce, Elastic) and In for every other one.
enum class KeyframeEasing : std::uint8_t {
    Auto,
    In,
    Out,
    InOut,
};

[[nodiscard]] constexpr bool isValidKeyframeEasingMode(KeyframeEasing easing) noexcept {
    return static_cast<std::uint8_t>(easing) <= static_cast<std::uint8_t>(KeyframeEasing::InOut);
}

// Blender's defaults for a new key (BKE initialize_bezt). Back's overshoot is a factor of the
// segment's change; Elastic's amplitude is in the curve's own value units (a Path key's progress
// runs 0 -> 1). Elastic's period is in SECONDS, because Bloom key times are rational seconds and
// a composition's frame rate can change under its keys: the default is Blender's 4.1 frames at
// Blender's default 24 fps. A period of exactly zero is Blender's automatic period (0.3 of the
// segment, 0.45 for In-Out).
inline constexpr double kDefaultKeyframeBack = 1.70158;
inline constexpr double kDefaultKeyframeAmplitude = 0.8;
inline constexpr double kDefaultKeyframePeriod = 4.1 / 24.0;

struct KeyframeEasingParameters {
    KeyframeEasing easing = KeyframeEasing::Auto;
    double back = kDefaultKeyframeBack;
    double amplitude = kDefaultKeyframeAmplitude;
    double period = kDefaultKeyframePeriod;

    friend bool operator==(const KeyframeEasingParameters&,
                           const KeyframeEasingParameters&) = default;
};

// Bitwise, like the handle test: the writer omits default parameters, and a merely-equal value
// (a negative zero period) must still be written so it reopens with the same bits.
[[nodiscard]] constexpr bool
isDefaultKeyframeEasing(const KeyframeEasingParameters& parameters) noexcept {
    return parameters.easing == KeyframeEasing::Auto &&
           std::bit_cast<std::uint64_t>(parameters.back) ==
               std::bit_cast<std::uint64_t>(kDefaultKeyframeBack) &&
           std::bit_cast<std::uint64_t>(parameters.amplitude) ==
               std::bit_cast<std::uint64_t>(kDefaultKeyframeAmplitude) &&
           std::bit_cast<std::uint64_t>(parameters.period) ==
               std::bit_cast<std::uint64_t>(kDefaultKeyframePeriod);
}

// Blender's own domain: the mode is known, every number finite, and the amplitude non-negative.
// Back and period may be any finite value (Blender leaves both unbounded).
[[nodiscard]] bool isValidKeyframeEasing(const KeyframeEasingParameters& parameters) noexcept;

// The stable lowercase tokens shared by the project format, Python and MCP: "hold", "linear",
// "ease-in-out" (Bezier), then "sine" ... "elastic"; and "auto", "in", "out", "in-out". An unknown
// enumerator yields an empty view, an unknown token nullopt.
[[nodiscard]] std::string_view keyframeInterpolationToken(KeyframeInterpolation mode) noexcept;
[[nodiscard]] std::optional<KeyframeInterpolation>
keyframeInterpolationFromToken(std::string_view token) noexcept;
[[nodiscard]] std::string_view keyframeEasingToken(KeyframeEasing easing) noexcept;
[[nodiscard]] std::optional<KeyframeEasing>
keyframeEasingFromToken(std::string_view token) noexcept;

// The default ease handle. `time` is a fraction of the segment's duration measured from the key
// the handle belongs to; `value` is an offset from that key's own value. The defaults (1/3, 0)
// are exactly the fixed handles the pre-handle EaseInOut carried, which is why a document written
// before ease handles existed samples bit-for-bit as it did: the sampler recognizes them and takes
// the old closed-form Smoothstep path verbatim.
inline constexpr double kDefaultKeyframeHandleTime = 1.0 / 3.0;
inline constexpr double kDefaultKeyframeHandleValue = 0.0;

struct KeyframeHandle {
    double time = kDefaultKeyframeHandleTime;
    double value = kDefaultKeyframeHandleValue;

    friend bool operator==(const KeyframeHandle&, const KeyframeHandle&) = default;
};

// "Default" is a BITWISE test rather than an arithmetic one. The sampler branches on it to decide
// whether it may take the old exact path, so a handle that merely compares equal (a negative zero
// offset, say) must not be able to claim a path that would produce different bits.
[[nodiscard]] constexpr bool isDefaultKeyframeHandle(const KeyframeHandle& handle) noexcept {
    return std::bit_cast<std::uint64_t>(handle.time) ==
               std::bit_cast<std::uint64_t>(kDefaultKeyframeHandleTime) &&
           std::bit_cast<std::uint64_t>(handle.value) ==
               std::bit_cast<std::uint64_t>(kDefaultKeyframeHandleValue);
}

[[nodiscard]] constexpr bool isDefaultKeyframeHandleTime(const double time) noexcept {
    return std::bit_cast<std::uint64_t>(time) ==
           std::bit_cast<std::uint64_t>(kDefaultKeyframeHandleTime);
}

// A handle is representable when its value offset is finite and its time fraction stays inside its
// own segment. A time outside [0, 1] would let one segment's control point reach past a neighbour
// key, which is not a shape the monotone x(s) inversion can answer.
[[nodiscard]] bool isValidKeyframeHandle(const KeyframeHandle& handle) noexcept;

struct ScalarKeyframe {
    KeyframeId id;
    core::RationalTime time;
    double value = 0.0;
    KeyframeInterpolation outgoingInterpolation = KeyframeInterpolation::Linear;
    // Ease handles for the segments on either side of this key. The trailing position and the
    // defaults keep every existing aggregate initializer valid. A handle offsets ONE scalar axis,
    // which is exactly what a key is: scalar curves and the component curves of a vector or colour
    // parameter hold ScalarKeyframes, and nothing else holds a key at all.
    KeyframeHandle outgoingHandle{};
    KeyframeHandle incomingHandle{};
    // The easing equation's parameters for this key's OUTGOING segment (document 1.30). Kept on
    // every key, like Blender's BezTriple, so switching a key between modes never loses them.
    KeyframeEasingParameters easing{};

    friend bool operator==(const ScalarKeyframe&, const ScalarKeyframe&) = default;
};

// A component is a scalar curve inside a vector or colour parameter. The enum names are grouped
// by the value kind that owns them; the numeric values are intentionally not used as array
// indices because X and Red are different semantic names even though both are component zero.
enum class AnimationComponent : std::uint8_t {
    X,
    Y,
    Z,
    Red,
    Green,
    Blue,
    Alpha,
};
using ParameterComponent = AnimationComponent;
using KeyframeComponent = AnimationComponent;

struct ComponentAnimationCurve {
    std::vector<ScalarKeyframe> keyframes;

    friend bool operator==(const ComponentAnimationCurve&,
                           const ComponentAnimationCurve&) = default;
};

struct ScalarAnimationCurve {
    AnimationCurveId id;
    std::vector<ScalarKeyframe> keyframes;

    friend bool operator==(const ScalarAnimationCurve&, const ScalarAnimationCurve&) = default;
};

// A vector or colour curve IS its components: one independent scalar curve per axis, addressed by
// the AnimationComponent that names it. There is no whole-value key record and no derived
// whole-value list -- "the parameter's keys" is the union of the component key sets, computed by
// whoever asks, and every writer therefore has exactly one place to keep correct.
struct Vec2AnimationCurve {
    AnimationCurveId id;
    std::array<ComponentAnimationCurve, 2> components{};

    [[nodiscard]] const ComponentAnimationCurve* component(AnimationComponent which) const noexcept;
    [[nodiscard]] ComponentAnimationCurve* component(AnimationComponent which) noexcept;

    friend bool operator==(const Vec2AnimationCurve&, const Vec2AnimationCurve&) = default;
};

struct Vec3AnimationCurve {
    AnimationCurveId id;
    std::array<ComponentAnimationCurve, 3> components{};

    [[nodiscard]] const ComponentAnimationCurve* component(AnimationComponent which) const noexcept;
    [[nodiscard]] ComponentAnimationCurve* component(AnimationComponent which) noexcept;

    friend bool operator==(const Vec3AnimationCurve&, const Vec3AnimationCurve&) = default;
};

struct Color4AnimationCurve {
    AnimationCurveId id;
    std::array<ComponentAnimationCurve, 4> components{};

    [[nodiscard]] const ComponentAnimationCurve* component(AnimationComponent which) const noexcept;
    [[nodiscard]] ComponentAnimationCurve* component(AnimationComponent which) noexcept;

    friend bool operator==(const Color4AnimationCurve&, const Color4AnimationCurve&) = default;
};

// ADR 0028: a whole-value Path key. Paths cannot be split per component, so a Path curve holds
// its own key kind. The ease handles' time fractions shape the segment exactly as a scalar key's
// do; their value offsets are fractions of the segment's normalized 0 -> 1 progress.
struct PathKeyframe {
    KeyframeId id;
    core::RationalTime time;
    PathValue value;
    KeyframeInterpolation outgoingInterpolation = KeyframeInterpolation::Linear;
    KeyframeHandle outgoingHandle{};
    KeyframeHandle incomingHandle{};
    KeyframeEasingParameters easing{};

    friend bool operator==(const PathKeyframe&, const PathKeyframe&) = default;
};

struct PathAnimationCurve {
    AnimationCurveId id;
    std::vector<PathKeyframe> keyframes;

    friend bool operator==(const PathAnimationCurve&, const PathAnimationCurve&) = default;
};

using AnimationCurveRecord = std::variant<ScalarAnimationCurve, Vec2AnimationCurve,
                                          Vec3AnimationCurve, Color4AnimationCurve,
                                          PathAnimationCurve>;

// Whole-value curves (scalar and Path) hold their keys directly in `keyframes`; the vector and
// colour curves hold them per component.
template <typename Curve>
inline constexpr bool kWholeValueAnimationCurve = std::is_same_v<Curve, ScalarAnimationCurve> ||
                                                  std::is_same_v<Curve, PathAnimationCurve>;

[[nodiscard]] AnimationCurveId animationCurveId(const AnimationCurveRecord& record) noexcept;

class AnimationCurveStore final {
  public:
    [[nodiscard]] std::span<const AnimationCurveRecord> records() const noexcept {
        return records_;
    }
    [[nodiscard]] const AnimationCurveRecord* find(AnimationCurveId id) const noexcept;
    [[nodiscard]] const ScalarAnimationCurve* findScalar(AnimationCurveId id) const noexcept;
    [[nodiscard]] const Vec2AnimationCurve* findVec2(AnimationCurveId id) const noexcept;
    [[nodiscard]] const Vec3AnimationCurve* findVec3(AnimationCurveId id) const noexcept;
    [[nodiscard]] const Color4AnimationCurve* findColor4(AnimationCurveId id) const noexcept;
    [[nodiscard]] const PathAnimationCurve* findPath(AnimationCurveId id) const noexcept;
    [[nodiscard]] const ComponentAnimationCurve*
    findComponent(AnimationCurveId id, AnimationComponent component) const noexcept;

    [[nodiscard]] bool insert(AnimationCurveRecord record);
    [[nodiscard]] bool erase(AnimationCurveId id);

    [[nodiscard]] bool insertKeyframe(AnimationCurveId curveId, ScalarKeyframe keyframe);
    [[nodiscard]] bool insertKeyframe(AnimationCurveId curveId, AnimationComponent component,
                                      ScalarKeyframe keyframe);
    [[nodiscard]] bool updateKeyframe(AnimationCurveId curveId, ScalarKeyframe keyframe);
    // ADR 0028 Path curves: whole-value keys.
    [[nodiscard]] bool insertKeyframe(AnimationCurveId curveId, PathKeyframe keyframe);
    [[nodiscard]] bool updateKeyframe(AnimationCurveId curveId, PathKeyframe keyframe);
    [[nodiscard]] bool updateKeyframe(AnimationCurveId curveId, AnimationComponent component,
                                      ScalarKeyframe keyframe);
    // Erases one key by id. A scalar curve is searched directly; a vector or colour curve is
    // searched component by component, because a KeyframeId belongs to exactly one component.
    [[nodiscard]] bool eraseKeyframe(AnimationCurveId curveId, KeyframeId keyframeId);
    [[nodiscard]] bool eraseKeyframe(AnimationCurveId curveId, AnimationComponent component,
                                     KeyframeId keyframeId);

    [[nodiscard]] ValidationResult validate() const;

  private:
    [[nodiscard]] AnimationCurveRecord* findMutable(AnimationCurveId id) noexcept;
    [[nodiscard]] bool containsKeyframe(KeyframeId id) const noexcept;

    std::vector<AnimationCurveRecord> records_;
};

[[nodiscard]] ValidationResult
validateAnimationCurveReferences(const ParameterStore& parameters,
                                 const AnimationCurveStore& animationCurves);

} // namespace bloom::document
