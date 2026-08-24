#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/validation.hpp>

#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace bloom::document {

enum class KeyframeInterpolation : std::uint8_t {
    Hold,
    Linear,
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

using AnimationCurveRecord = std::variant<ScalarAnimationCurve, Vec2AnimationCurve>;

[[nodiscard]] AnimationCurveId animationCurveId(const AnimationCurveRecord& record) noexcept;

class AnimationCurveStore final {
  public:
    [[nodiscard]] std::span<const AnimationCurveRecord> records() const noexcept {
        return records_;
    }
    [[nodiscard]] const AnimationCurveRecord* find(AnimationCurveId id) const noexcept;
    [[nodiscard]] const ScalarAnimationCurve* findScalar(AnimationCurveId id) const noexcept;
    [[nodiscard]] const Vec2AnimationCurve* findVec2(AnimationCurveId id) const noexcept;

    [[nodiscard]] bool insert(AnimationCurveRecord record);
    [[nodiscard]] bool erase(AnimationCurveId id);

    [[nodiscard]] bool insertKeyframe(AnimationCurveId curveId, ScalarKeyframe keyframe);
    [[nodiscard]] bool insertKeyframe(AnimationCurveId curveId, Vec2Keyframe keyframe);
    [[nodiscard]] bool updateKeyframe(AnimationCurveId curveId, ScalarKeyframe keyframe);
    [[nodiscard]] bool updateKeyframe(AnimationCurveId curveId, Vec2Keyframe keyframe);
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
