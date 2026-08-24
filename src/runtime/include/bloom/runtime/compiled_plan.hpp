#pragma once

#include <bloom/core/color.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace bloom::runtime {

inline constexpr std::uint32_t kCompiledCompositionPlanSemanticsVersion = 1;
inline constexpr std::uint32_t kAnimationSamplingSemanticsVersion = 1;

enum class CompiledKeyframeInterpolation : std::uint8_t {
    Hold,
    Linear,
};

struct CompiledScalarKeyframe final {
    document::KeyframeId id;
    core::RationalTime time;
    double value = 0.0;
    CompiledKeyframeInterpolation outgoingInterpolation = CompiledKeyframeInterpolation::Linear;

    friend bool operator==(const CompiledScalarKeyframe&, const CompiledScalarKeyframe&) = default;
};

struct CompiledVec2Keyframe final {
    document::KeyframeId id;
    core::RationalTime time;
    document::Vec2d value;
    CompiledKeyframeInterpolation outgoingInterpolation = CompiledKeyframeInterpolation::Linear;

    friend bool operator==(const CompiledVec2Keyframe&, const CompiledVec2Keyframe&) = default;
};

struct CompiledScalarCurve final {
    document::AnimationCurveId id;
    std::vector<CompiledScalarKeyframe> keyframes;

    friend bool operator==(const CompiledScalarCurve&, const CompiledScalarCurve&) = default;
};

struct CompiledVec2Curve final {
    document::AnimationCurveId id;
    std::vector<CompiledVec2Keyframe> keyframes;

    friend bool operator==(const CompiledVec2Curve&, const CompiledVec2Curve&) = default;
};

class ScalarCurveIndex final {
  public:
    [[nodiscard]] static constexpr ScalarCurveIndex fromRaw(const std::size_t value) noexcept {
        return ScalarCurveIndex(value);
    }

    [[nodiscard]] constexpr std::size_t value() const noexcept { return value_; }
    friend constexpr auto operator<=>(const ScalarCurveIndex&,
                                      const ScalarCurveIndex&) noexcept = default;

  private:
    explicit constexpr ScalarCurveIndex(const std::size_t value) noexcept : value_(value) {}

    std::size_t value_ = 0;
};

class Vec2CurveIndex final {
  public:
    [[nodiscard]] static constexpr Vec2CurveIndex fromRaw(const std::size_t value) noexcept {
        return Vec2CurveIndex(value);
    }

    [[nodiscard]] constexpr std::size_t value() const noexcept { return value_; }
    friend constexpr auto operator<=>(const Vec2CurveIndex&,
                                      const Vec2CurveIndex&) noexcept = default;

  private:
    explicit constexpr Vec2CurveIndex(const std::size_t value) noexcept : value_(value) {}

    std::size_t value_ = 0;
};

struct CompiledScalarParameter final {
    document::ParameterId id;
    std::variant<double, ScalarCurveIndex> source;

    friend bool operator==(const CompiledScalarParameter&,
                           const CompiledScalarParameter&) = default;
};

struct CompiledVec2Parameter final {
    document::ParameterId id;
    std::variant<document::Vec2d, Vec2CurveIndex> source;

    friend bool operator==(const CompiledVec2Parameter&, const CompiledVec2Parameter&) = default;
};

class OperationIndex final {
  public:
    [[nodiscard]] static constexpr OperationIndex fromRaw(const std::size_t value) noexcept {
        return OperationIndex(value);
    }

    [[nodiscard]] constexpr std::size_t value() const noexcept { return value_; }
    friend constexpr auto operator<=>(const OperationIndex&,
                                      const OperationIndex&) noexcept = default;

  private:
    explicit constexpr OperationIndex(const std::size_t value) noexcept : value_(value) {}

    std::size_t value_ = 0;
};

struct CompiledSolid {
    document::NodeId sourceNodeId;
    document::ParameterId colorParameterId;
    core::Color4d color;

    friend bool operator==(const CompiledSolid&, const CompiledSolid&) = default;
};

struct CompiledLayerOutput {
    document::NodeId sourceNodeId;
    document::LayerId layerId;
    OperationIndex input;
    CompiledVec2Parameter position;
    CompiledScalarParameter opacity;

    friend bool operator==(const CompiledLayerOutput&, const CompiledLayerOutput&) = default;
};

struct CompiledLayerStackEntry {
    document::LayerSlotId slotId;
    document::LayerId layerId;
    OperationIndex input;

    friend bool operator==(const CompiledLayerStackEntry&,
                           const CompiledLayerStackEntry&) = default;
};

struct CompiledLayerStack {
    document::NodeId sourceNodeId;
    std::vector<CompiledLayerStackEntry> entries;

    friend bool operator==(const CompiledLayerStack&, const CompiledLayerStack&) = default;
};

struct CompiledCompositionOutput {
    document::NodeId sourceNodeId;
    OperationIndex input;

    friend bool operator==(const CompiledCompositionOutput&,
                           const CompiledCompositionOutput&) = default;
};

using CompiledOperation =
    std::variant<CompiledSolid, CompiledLayerOutput, CompiledLayerStack, CompiledCompositionOutput>;

struct CompiledCompositionPlan {
    document::Revision sourceRevision;
    document::ProjectId projectId;
    document::CompositionId compositionId;
    document::CompositionFormat format;
    std::vector<CompiledOperation> operations;
    OperationIndex output;
    std::vector<CompiledScalarCurve> scalarCurves{};
    std::vector<CompiledVec2Curve> vec2Curves{};
    std::uint32_t planSemanticsVersion = kCompiledCompositionPlanSemanticsVersion;
    std::uint32_t animationSamplingSemanticsVersion = kAnimationSamplingSemanticsVersion;

    friend bool operator==(const CompiledCompositionPlan&,
                           const CompiledCompositionPlan&) = default;
};

} // namespace bloom::runtime
