#pragma once

#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace bloom::runtime {

// Deliberately NOT bumped when CompiledText was added (task S3). This number exists so an evaluator
// can refuse a plan built under different semantics, and a compiled plan never crosses a process or
// persistence boundary: it is compiled in-process from a document snapshot and retained only by
// frames of that same process, so no version-1 plan can reach a build that has CompiledText, and no
// plan carrying a CompiledText can reach a build that does not. What DOES cross such a boundary is
// ProcessFrameIdentity, and the two numbers it carries for this are
// kCpuCompositionEvaluatorSemanticsVersion and render::kCpuImagePrimitiveSemanticsVersion -- both
// bumped by the text path, because both describe pixels a cached or exported frame may already
// hold. Bump this one when the plan's own grammar changes in a way an existing plan value could
// misrepresent (a field's meaning changing, not a new alternative appearing).
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

// A lowered text source. Content, size, and color are all resolved constants, not parameter
// sources: the text schema declares none of the three animatable, and the command surface has no
// way to put any of them on a curve (CreateAnimationForParameter accepts only the animatable
// transform and opacity schemas, and SetKeyframeAtTime has no string or Color4d overload). The
// parameter
// identities travel with them so a diagnostic can name the exact parameter that failed, exactly as
// CompiledSolid does.
struct CompiledText {
    document::NodeId sourceNodeId;
    document::ParameterId contentParameterId;
    std::string content;
    document::ParameterId sizeParameterId;
    double size = document::kDefaultTextSizePixels;
    document::ParameterId colorParameterId;
    core::Color4d color;

    friend bool operator==(const CompiledText&, const CompiledText&) = default;
};

// A lowered Layer Output boundary. The six parameters appear in the registered authoring order --
// position, anchor, scale, rotation, opacity, blend mode -- and each one carries its own parameter
// identity so a diagnostic can name the exact parameter that failed. The first five are animatable,
// so each is either a resolved constant or an index into the plan's curve tables.
//
// The blend mode is a resolved constant, like CompiledText's three values and for the same reason:
// the schema declares it non-animatable and no command in the surface can put it on a curve. It
// lives HERE, on the layer boundary that owns it, rather than on the stack entry that consumes it --
// the stack entry is the ordering of layers, and the mode is a property of the layer.
struct CompiledLayerOutput {
    document::NodeId sourceNodeId;
    document::LayerId layerId;
    OperationIndex input;
    CompiledVec2Parameter position;
    CompiledVec2Parameter anchor;
    CompiledVec2Parameter scale;
    CompiledScalarParameter rotation;
    CompiledScalarParameter opacity;
    document::ParameterId blendModeParameterId;
    core::BlendMode blendMode = core::kDefaultBlendMode;

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

using CompiledOperation = std::variant<CompiledSolid, CompiledText, CompiledLayerOutput,
                                       CompiledLayerStack, CompiledCompositionOutput>;

// Mutable construction storage is deliberately a distinct type. Publishing a plan copies or moves
// this complete definition into private storage, so retaining or changing the definition cannot
// change evaluation or identity semantics after publication.
struct CompiledCompositionPlanDefinition final {
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

    friend bool operator==(const CompiledCompositionPlanDefinition&,
                           const CompiledCompositionPlanDefinition&) = default;
};

// Immutable after construction. Frames may retain this object through shared ownership without a
// per-frame plan copy because no public API can mutate the published fields or vector storage.
class CompiledCompositionPlan final {
  public:
    explicit CompiledCompositionPlan(CompiledCompositionPlanDefinition definition) noexcept;

    CompiledCompositionPlan(const CompiledCompositionPlan&) = delete;
    CompiledCompositionPlan& operator=(const CompiledCompositionPlan&) = delete;
    CompiledCompositionPlan(CompiledCompositionPlan&&) = delete;
    CompiledCompositionPlan& operator=(CompiledCompositionPlan&&) = delete;
    ~CompiledCompositionPlan() = default;

    [[nodiscard]] document::Revision sourceRevision() const noexcept { return sourceRevision_; }
    [[nodiscard]] document::ProjectId projectId() const noexcept { return projectId_; }
    [[nodiscard]] document::CompositionId compositionId() const noexcept { return compositionId_; }
    [[nodiscard]] const document::CompositionFormat& format() const& noexcept { return format_; }
    [[nodiscard]] const document::CompositionFormat& format() const&& = delete;
    [[nodiscard]] std::span<const CompiledOperation> operations() const& noexcept {
        return operations_;
    }
    [[nodiscard]] std::span<const CompiledOperation> operations() const&& = delete;
    [[nodiscard]] OperationIndex output() const noexcept { return output_; }
    [[nodiscard]] std::span<const CompiledScalarCurve> scalarCurves() const& noexcept {
        return scalarCurves_;
    }
    [[nodiscard]] std::span<const CompiledScalarCurve> scalarCurves() const&& = delete;
    [[nodiscard]] std::span<const CompiledVec2Curve> vec2Curves() const& noexcept {
        return vec2Curves_;
    }
    [[nodiscard]] std::span<const CompiledVec2Curve> vec2Curves() const&& = delete;
    [[nodiscard]] std::uint32_t planSemanticsVersion() const noexcept {
        return planSemanticsVersion_;
    }
    [[nodiscard]] std::uint32_t animationSamplingSemanticsVersion() const noexcept {
        return animationSamplingSemanticsVersion_;
    }

    // This is intentionally an allocating deep copy for tests and tooling that need a mutable
    // candidate definition. It never exposes aliases into the published plan.
    [[nodiscard]] CompiledCompositionPlanDefinition copyDefinition() const;

    friend bool operator==(const CompiledCompositionPlan&,
                           const CompiledCompositionPlan&) = default;

  private:
    document::Revision sourceRevision_;
    document::ProjectId projectId_;
    document::CompositionId compositionId_;
    document::CompositionFormat format_;
    std::vector<CompiledOperation> operations_;
    OperationIndex output_;
    std::vector<CompiledScalarCurve> scalarCurves_;
    std::vector<CompiledVec2Curve> vec2Curves_;
    std::uint32_t planSemanticsVersion_ = kCompiledCompositionPlanSemanticsVersion;
    std::uint32_t animationSamplingSemanticsVersion_ = kAnimationSamplingSemanticsVersion;
};

} // namespace bloom::runtime
