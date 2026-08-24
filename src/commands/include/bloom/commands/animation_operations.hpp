#pragma once

#include <bloom/commands/operation.hpp>
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

class SetKeyframeAtTime final : public Operation {
  public:
    SetKeyframeAtTime(document::CompositionId compositionId, document::AnimationCurveId curveId,
                      core::RationalTime time, double value)
        : compositionId_(compositionId), curveId_(curveId), time_(time), value_(value) {}
    SetKeyframeAtTime(document::CompositionId compositionId, document::AnimationCurveId curveId,
                      core::RationalTime time, document::Vec2d value)
        : compositionId_(compositionId), curveId_(curveId), time_(time), value_(value) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::AnimationCurveId curveId_;
    core::RationalTime time_;
    std::variant<double, document::Vec2d> value_;
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

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::ParameterId parameterId_;
    std::variant<double, document::Vec2d> value_;
};

} // namespace bloom::commands
