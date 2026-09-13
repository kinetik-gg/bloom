#pragma once

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/validation.hpp>

#include <cstdint>
#include <span>
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

struct Vec2Keyframe {
    KeyframeId id;
    core::RationalTime time;
    Vec2d value;
    KeyframeInterpolation outgoingInterpolation = KeyframeInterpolation::Linear;

    friend bool operator==(const Vec2Keyframe&, const Vec2Keyframe&) = default;
};

struct ScalarAnimationCurve {
    AnimationCurveId id;
    std::vector<ScalarKeyframe> keyframes;

    friend bool operator==(const ScalarAnimationCurve&, const ScalarAnimationCurve&) = default;
};

struct Vec2AnimationCurve {
    AnimationCurveId id;
    std::vector<Vec2Keyframe> keyframes;

    friend bool operator==(const Vec2AnimationCurve&, const Vec2AnimationCurve&) = default;
};

// The third curve value kind (task S5): straight/unassociated authoring RGBA, exactly the encoding
// core::Color4d already carries for a constant solid or text color. A color key's value must
// satisfy core::Color4d::isValid() -- finite RGB and alpha in [0, 1] -- which is the SAME domain
// the constant source and the evaluator already enforce for an authoring color, so animating a
// color cannot reach a value a constant one could not.
struct Color4Keyframe {
    KeyframeId id;
    core::RationalTime time;
    core::Color4d value;
    KeyframeInterpolation outgoingInterpolation = KeyframeInterpolation::Linear;

    friend bool operator==(const Color4Keyframe&, const Color4Keyframe&) = default;
};

struct Color4AnimationCurve {
    AnimationCurveId id;
    std::vector<Color4Keyframe> keyframes;

    friend bool operator==(const Color4AnimationCurve&, const Color4AnimationCurve&) = default;
};

using AnimationCurveRecord =
    std::variant<ScalarAnimationCurve, Vec2AnimationCurve, Color4AnimationCurve>;

[[nodiscard]] AnimationCurveId animationCurveId(const AnimationCurveRecord& record) noexcept;

class AnimationCurveStore final {
  public:
    [[nodiscard]] std::span<const AnimationCurveRecord> records() const noexcept {
        return records_;
    }
    [[nodiscard]] const AnimationCurveRecord* find(AnimationCurveId id) const noexcept;
    [[nodiscard]] const ScalarAnimationCurve* findScalar(AnimationCurveId id) const noexcept;
    [[nodiscard]] const Vec2AnimationCurve* findVec2(AnimationCurveId id) const noexcept;
    [[nodiscard]] const Color4AnimationCurve* findColor4(AnimationCurveId id) const noexcept;

    [[nodiscard]] bool insert(AnimationCurveRecord record);
    [[nodiscard]] bool erase(AnimationCurveId id);

    [[nodiscard]] bool insertKeyframe(AnimationCurveId curveId, ScalarKeyframe keyframe);
    [[nodiscard]] bool insertKeyframe(AnimationCurveId curveId, Vec2Keyframe keyframe);
    [[nodiscard]] bool insertKeyframe(AnimationCurveId curveId, Color4Keyframe keyframe);
    [[nodiscard]] bool updateKeyframe(AnimationCurveId curveId, ScalarKeyframe keyframe);
    [[nodiscard]] bool updateKeyframe(AnimationCurveId curveId, Vec2Keyframe keyframe);
    [[nodiscard]] bool updateKeyframe(AnimationCurveId curveId, Color4Keyframe keyframe);
    [[nodiscard]] bool eraseKeyframe(AnimationCurveId curveId, KeyframeId keyframeId);

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
