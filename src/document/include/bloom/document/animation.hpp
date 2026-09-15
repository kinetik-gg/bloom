#pragma once

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/validation.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::document {

// The outgoing interpolation of one key's segment.
//
// EaseInOut is a cubic Bezier ease with FIXED symmetric handles at (1/3, 0) and (2/3, 1). Those
// x handles are exactly what makes the ease exact and allocation-free: a cubic Bezier whose x
// control points are 0, 1/3, 2/3, 1 has x(s) == s identically, so the curve parameter IS the
// interval factor and no root finding, iteration, or libm call is needed -- the eased factor is the
// closed-form polynomial 3t^2 - 2t^3 of the exact rational interval factor. See
// docs/architecture/animation-and-time.md, "Sampling Semantics Version 1".
enum class KeyframeInterpolation : std::uint8_t {
    Hold,
    Linear,
    EaseInOut,
};

struct ScalarKeyframe {
    KeyframeId id;
    core::RationalTime time;
    double value = 0.0;
    KeyframeInterpolation outgoingInterpolation = KeyframeInterpolation::Linear;

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

// Compatibility projections retained for pre-component callers. They are not the source of truth
// for component-aware curves; new records use the `components` arrays below. Keeping these plain
// value records lets the pre-KEY-2 timeline and older all-components commands continue to build
// while the UI wiring migrates to KeyframeComponent selections.
struct Vec2Keyframe {
    KeyframeId id;
    core::RationalTime time;
    Vec2d value;
    KeyframeInterpolation outgoingInterpolation = KeyframeInterpolation::Linear;

    friend bool operator==(const Vec2Keyframe&, const Vec2Keyframe&) = default;
};

struct Vec3Keyframe {
    KeyframeId id;
    core::RationalTime time;
    Vec3d value;
    KeyframeInterpolation outgoingInterpolation = KeyframeInterpolation::Linear;

    friend bool operator==(const Vec3Keyframe&, const Vec3Keyframe&) = default;
};

struct Color4Keyframe {
    KeyframeId id;
    core::RationalTime time;
    core::Color4d value;
    KeyframeInterpolation outgoingInterpolation = KeyframeInterpolation::Linear;

    friend bool operator==(const Color4Keyframe&, const Color4Keyframe&) = default;
};

struct ScalarAnimationCurve {
    AnimationCurveId id;
    std::vector<ScalarKeyframe> keyframes;

    friend bool operator==(const ScalarAnimationCurve&, const ScalarAnimationCurve&) = default;
};

struct Vec2AnimationCurve {
    AnimationCurveId id;
    std::vector<Vec2Keyframe> keyframes{};
    std::array<ComponentAnimationCurve, 2> components{};

    Vec2AnimationCurve() = default;
    Vec2AnimationCurve(AnimationCurveId curveId, std::vector<Vec2Keyframe> legacyKeyframes)
        : id(curveId), keyframes(std::move(legacyKeyframes)) {}
    Vec2AnimationCurve(AnimationCurveId curveId,
                       std::array<ComponentAnimationCurve, 2> componentCurves)
        : id(curveId), components(std::move(componentCurves)) {}
    Vec2AnimationCurve(AnimationCurveId curveId, std::vector<Vec2Keyframe> legacyKeyframes,
                       std::array<ComponentAnimationCurve, 2> componentCurves)
        : id(curveId), keyframes(std::move(legacyKeyframes)),
          components(std::move(componentCurves)) {}

    [[nodiscard]] const ComponentAnimationCurve* component(AnimationComponent which) const noexcept;
    [[nodiscard]] ComponentAnimationCurve* component(AnimationComponent which) noexcept;

    friend bool operator==(const Vec2AnimationCurve&, const Vec2AnimationCurve&) = default;
};

struct Vec3AnimationCurve {
    AnimationCurveId id;
    std::vector<Vec3Keyframe> keyframes{};
    std::array<ComponentAnimationCurve, 3> components{};

    Vec3AnimationCurve() = default;
    Vec3AnimationCurve(AnimationCurveId curveId, std::vector<Vec3Keyframe> legacyKeyframes)
        : id(curveId), keyframes(std::move(legacyKeyframes)) {}
    Vec3AnimationCurve(AnimationCurveId curveId,
                       std::array<ComponentAnimationCurve, 3> componentCurves)
        : id(curveId), components(std::move(componentCurves)) {}
    Vec3AnimationCurve(AnimationCurveId curveId, std::vector<Vec3Keyframe> legacyKeyframes,
                       std::array<ComponentAnimationCurve, 3> componentCurves)
        : id(curveId), keyframes(std::move(legacyKeyframes)),
          components(std::move(componentCurves)) {}

    [[nodiscard]] const ComponentAnimationCurve* component(AnimationComponent which) const noexcept;
    [[nodiscard]] ComponentAnimationCurve* component(AnimationComponent which) noexcept;

    friend bool operator==(const Vec3AnimationCurve&, const Vec3AnimationCurve&) = default;
};

struct Color4AnimationCurve {
    AnimationCurveId id;
    std::vector<Color4Keyframe> keyframes{};
    std::array<ComponentAnimationCurve, 4> components{};

    Color4AnimationCurve() = default;
    Color4AnimationCurve(AnimationCurveId curveId, std::vector<Color4Keyframe> legacyKeyframes)
        : id(curveId), keyframes(std::move(legacyKeyframes)) {}
    Color4AnimationCurve(AnimationCurveId curveId,
                         std::array<ComponentAnimationCurve, 4> componentCurves)
        : id(curveId), components(std::move(componentCurves)) {}
    Color4AnimationCurve(AnimationCurveId curveId, std::vector<Color4Keyframe> legacyKeyframes,
                         std::array<ComponentAnimationCurve, 4> componentCurves)
        : id(curveId), keyframes(std::move(legacyKeyframes)),
          components(std::move(componentCurves)) {}

    [[nodiscard]] const ComponentAnimationCurve* component(AnimationComponent which) const noexcept;
    [[nodiscard]] ComponentAnimationCurve* component(AnimationComponent which) noexcept;

    friend bool operator==(const Color4AnimationCurve&, const Color4AnimationCurve&) = default;
};

using AnimationCurveRecord = std::variant<ScalarAnimationCurve, Vec2AnimationCurve,
                                          Vec3AnimationCurve, Color4AnimationCurve>;

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
    [[nodiscard]] const ComponentAnimationCurve*
    findComponent(AnimationCurveId id, AnimationComponent component) const noexcept;

    [[nodiscard]] bool insert(AnimationCurveRecord record);
    [[nodiscard]] bool erase(AnimationCurveId id);

    [[nodiscard]] bool insertKeyframe(AnimationCurveId curveId, ScalarKeyframe keyframe);
    [[nodiscard]] bool insertKeyframe(AnimationCurveId curveId, Vec2Keyframe keyframe);
    [[nodiscard]] bool insertKeyframe(AnimationCurveId curveId, Vec3Keyframe keyframe);
    [[nodiscard]] bool insertKeyframe(AnimationCurveId curveId, Color4Keyframe keyframe);
    [[nodiscard]] bool insertKeyframe(AnimationCurveId curveId, AnimationComponent component,
                                      ScalarKeyframe keyframe);
    [[nodiscard]] bool updateKeyframe(AnimationCurveId curveId, ScalarKeyframe keyframe);
    [[nodiscard]] bool updateKeyframe(AnimationCurveId curveId, Vec2Keyframe keyframe);
    [[nodiscard]] bool updateKeyframe(AnimationCurveId curveId, Vec3Keyframe keyframe);
    [[nodiscard]] bool updateKeyframe(AnimationCurveId curveId, Color4Keyframe keyframe);
    [[nodiscard]] bool updateKeyframe(AnimationCurveId curveId, AnimationComponent component,
                                      ScalarKeyframe keyframe);
    [[nodiscard]] bool eraseKeyframe(AnimationCurveId curveId, KeyframeId keyframeId);
    [[nodiscard]] bool eraseKeyframe(AnimationCurveId curveId, AnimationComponent component,
                                     KeyframeId keyframeId);
    // Refreshes the pre-component whole-value projection used by legacy callers. Component-aware
    // records remain authoritative; the projection is only a compatibility view.
    [[nodiscard]] bool synchronizeCompatibilityProjection(AnimationCurveId curveId);

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
