#pragma once

#include <bloom/commands/operation.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>

#include <string_view>
#include <variant>

namespace bloom::commands {

inline constexpr std::string_view kAnimationCurveOutput = "animationCurve";
inline constexpr std::string_view kKeyframeOutput = "keyframe";

class CreateAnimationForParameter final : public Operation {
  public:
    CreateAnimationForParameter(document::CompositionId compositionId,
                                document::ParameterId parameterId, core::RationalTime initialTime)
        : compositionId_(compositionId), parameterId_(parameterId), initialTime_(initialTime) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::ParameterId parameterId_;
    core::RationalTime initialTime_;
};

class InsertScalarKeyframe final : public Operation {
  public:
    InsertScalarKeyframe(document::CompositionId compositionId, document::AnimationCurveId curveId,
                         core::RationalTime time, double value,
                         document::KeyframeInterpolation outgoingInterpolation =
                             document::KeyframeInterpolation::Linear)
        : compositionId_(compositionId), curveId_(curveId), time_(time), value_(value),
          outgoingInterpolation_(outgoingInterpolation) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::AnimationCurveId curveId_;
    core::RationalTime time_;
    double value_ = 0.0;
    document::KeyframeInterpolation outgoingInterpolation_ =
        document::KeyframeInterpolation::Linear;
};

class InsertVec2Keyframe final : public Operation {
  public:
    InsertVec2Keyframe(document::CompositionId compositionId, document::AnimationCurveId curveId,
                       core::RationalTime time, document::Vec2d value,
                       document::KeyframeInterpolation outgoingInterpolation =
                           document::KeyframeInterpolation::Linear)
        : compositionId_(compositionId), curveId_(curveId), time_(time), value_(value),
          outgoingInterpolation_(outgoingInterpolation) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::AnimationCurveId curveId_;
    core::RationalTime time_;
    document::Vec2d value_;
    document::KeyframeInterpolation outgoingInterpolation_ =
        document::KeyframeInterpolation::Linear;
};

class InsertColor4Keyframe final : public Operation {
  public:
    InsertColor4Keyframe(document::CompositionId compositionId, document::AnimationCurveId curveId,
                         core::RationalTime time, core::Color4d value,
                         document::KeyframeInterpolation outgoingInterpolation =
                             document::KeyframeInterpolation::Linear)
        : compositionId_(compositionId), curveId_(curveId), time_(time), value_(value),
          outgoingInterpolation_(outgoingInterpolation) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::AnimationCurveId curveId_;
    core::RationalTime time_;
    core::Color4d value_;
    document::KeyframeInterpolation outgoingInterpolation_ =
        document::KeyframeInterpolation::Linear;
};

class UpdateScalarKeyframe final : public Operation {
  public:
    UpdateScalarKeyframe(document::CompositionId compositionId, document::AnimationCurveId curveId,
                         document::KeyframeId keyframeId, core::RationalTime time, double value,
                         document::KeyframeInterpolation outgoingInterpolation =
                             document::KeyframeInterpolation::Linear)
        : compositionId_(compositionId), curveId_(curveId), keyframeId_(keyframeId), time_(time),
          value_(value), outgoingInterpolation_(outgoingInterpolation) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::AnimationCurveId curveId_;
    document::KeyframeId keyframeId_;
    core::RationalTime time_;
    double value_ = 0.0;
    document::KeyframeInterpolation outgoingInterpolation_ =
        document::KeyframeInterpolation::Linear;
};

class UpdateVec2Keyframe final : public Operation {
  public:
    UpdateVec2Keyframe(document::CompositionId compositionId, document::AnimationCurveId curveId,
                       document::KeyframeId keyframeId, core::RationalTime time,
                       document::Vec2d value,
                       document::KeyframeInterpolation outgoingInterpolation =
                           document::KeyframeInterpolation::Linear)
        : compositionId_(compositionId), curveId_(curveId), keyframeId_(keyframeId), time_(time),
          value_(value), outgoingInterpolation_(outgoingInterpolation) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::AnimationCurveId curveId_;
    document::KeyframeId keyframeId_;
    core::RationalTime time_;
    document::Vec2d value_;
    document::KeyframeInterpolation outgoingInterpolation_ =
        document::KeyframeInterpolation::Linear;
};

class UpdateColor4Keyframe final : public Operation {
  public:
    UpdateColor4Keyframe(document::CompositionId compositionId, document::AnimationCurveId curveId,
                         document::KeyframeId keyframeId, core::RationalTime time,
                         core::Color4d value,
                         document::KeyframeInterpolation outgoingInterpolation =
                             document::KeyframeInterpolation::Linear)
        : compositionId_(compositionId), curveId_(curveId), keyframeId_(keyframeId), time_(time),
          value_(value), outgoingInterpolation_(outgoingInterpolation) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::AnimationCurveId curveId_;
    document::KeyframeId keyframeId_;
    core::RationalTime time_;
    core::Color4d value_;
    document::KeyframeInterpolation outgoingInterpolation_ =
        document::KeyframeInterpolation::Linear;
};

// Re-points one existing key's OUTGOING interpolation, preserving its KeyframeId, exact time, and
// value bit-for-bit (task S5, item 2). It is deliberately its own operation rather than a flag on
// UpdateKeyframe: the gesture that changes an ease is a menu pick on a key that is not moving, and
// routing it through UpdateKeyframe would make a caller restate a time and a value it has no reason
// to touch. Setting the interpolation the key already carries is a committing no-change.
//
// The FINAL key of a curve is always canonical Linear (docs/architecture/animation-and-time.md), so
// asking for anything else there is refused rather than silently normalized -- the menu disables
// the choice, and this is the command-layer guarantee behind that.
class SetKeyframeInterpolation final : public Operation {
  public:
    SetKeyframeInterpolation(document::CompositionId compositionId,
                             document::AnimationCurveId curveId, document::KeyframeId keyframeId,
                             document::KeyframeInterpolation interpolation)
        : compositionId_(compositionId), curveId_(curveId), keyframeId_(keyframeId),
          interpolation_(interpolation) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::AnimationCurveId curveId_;
    document::KeyframeId keyframeId_;
    document::KeyframeInterpolation interpolation_ = document::KeyframeInterpolation::Linear;
};

class SetKeyframeAtTime final : public Operation {
  public:
    SetKeyframeAtTime(document::CompositionId compositionId, document::AnimationCurveId curveId,
                      core::RationalTime time, double value)
        : compositionId_(compositionId), curveId_(curveId), time_(time), value_(value) {}
    SetKeyframeAtTime(document::CompositionId compositionId, document::AnimationCurveId curveId,
                      core::RationalTime time, document::Vec2d value)
        : compositionId_(compositionId), curveId_(curveId), time_(time), value_(value) {}
    SetKeyframeAtTime(document::CompositionId compositionId, document::AnimationCurveId curveId,
                      core::RationalTime time, core::Color4d value)
        : compositionId_(compositionId), curveId_(curveId), time_(time), value_(value) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::AnimationCurveId curveId_;
    core::RationalTime time_;
    std::variant<double, document::Vec2d, core::Color4d> value_;
};

// SetKeyframeAtTime's PARAMETER-keyed sibling (task S5, item 0). It resolves the parameter's own
// AnimationCurveSource at APPLY time and then does exactly what SetKeyframeAtTime does, which is
// the one thing a curve-keyed operation cannot do: sit in the same transaction as
// CreateAnimationForParameter, whose curve id does not exist until that earlier operation runs
// against the same draft. That is precisely the AE keyframe gesture -- "make this parameter
// animated AND put a key at the current time" is one undo step, not two.
//
// Every admission rule is the curve-keyed operation's, reached through the same helper: a
// constant-sourced or driven parameter is refused (there is no curve to key), an occupied exact
// time updates that key and preserves its KeyframeId, and a value equal to the existing one is a
// committing no-change.
class SetKeyframeAtTimeForParameter final : public Operation {
  public:
    SetKeyframeAtTimeForParameter(document::CompositionId compositionId,
                                  document::ParameterId parameterId, core::RationalTime time,
                                  double value)
        : compositionId_(compositionId), parameterId_(parameterId), time_(time), value_(value) {}
    SetKeyframeAtTimeForParameter(document::CompositionId compositionId,
                                  document::ParameterId parameterId, core::RationalTime time,
                                  document::Vec2d value)
        : compositionId_(compositionId), parameterId_(parameterId), time_(time), value_(value) {}
    SetKeyframeAtTimeForParameter(document::CompositionId compositionId,
                                  document::ParameterId parameterId, core::RationalTime time,
                                  core::Color4d value)
        : compositionId_(compositionId), parameterId_(parameterId), time_(time), value_(value) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::ParameterId parameterId_;
    core::RationalTime time_;
    std::variant<double, document::Vec2d, core::Color4d> value_;
};

class DeleteKeyframe final : public Operation {
  public:
    DeleteKeyframe(document::CompositionId compositionId, document::AnimationCurveId curveId,
                   document::KeyframeId keyframeId)
        : compositionId_(compositionId), curveId_(curveId), keyframeId_(keyframeId) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::AnimationCurveId curveId_;
    document::KeyframeId keyframeId_;
};

class ConvertAnimationToConstant final : public Operation {
  public:
    ConvertAnimationToConstant(document::CompositionId compositionId,
                               document::ParameterId parameterId, double value)
        : compositionId_(compositionId), parameterId_(parameterId), value_(value) {}
    ConvertAnimationToConstant(document::CompositionId compositionId,
                               document::ParameterId parameterId, document::Vec2d value)
        : compositionId_(compositionId), parameterId_(parameterId), value_(value) {}
    ConvertAnimationToConstant(document::CompositionId compositionId,
                               document::ParameterId parameterId, core::Color4d value)
        : compositionId_(compositionId), parameterId_(parameterId), value_(value) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::ParameterId parameterId_;
    std::variant<double, document::Vec2d, core::Color4d> value_;
};

} // namespace bloom::commands
