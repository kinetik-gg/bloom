#pragma once

#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/runtime/compiled_curves.hpp>
#include <bloom/runtime/compiled_value_graph.hpp>

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
//
// Task S5 bumped it 1 -> 2 for exactly that reason: CompiledSolid::color, CompiledText::size, and
// CompiledText::color stopped being resolved constants and became CompiledColorParameter/
// CompiledScalarParameter operands, because those three schemas are now animatable. A version-1
// plan's `color` field was a Color4d; a version-2 plan's is a parameter that may index a curve
// table, so the same field position means something different -- a field's meaning changing, not a
// new alternative appearing. Both numbers below also enter ProcessFrameIdentity and therefore every
// cached/exported frame digest (src/output/process_frame_semantic_identity.cpp), which is why the
// identity goldens were re-derived in the same change.
inline constexpr std::uint32_t kCompiledCompositionPlanSemanticsVersion = 3;
// Task S5 bumped this 1 -> 2: KeyframeInterpolation gained EaseInOut, so sampling can now produce a
// value no version-1 sampler could, and the Color4 curve table added a third sampled value kind.
inline constexpr std::uint32_t kAnimationSamplingSemanticsVersion = 2;

// Each of the three typed operands gained ONE more alternative in task S7: a value-graph output,
// for a parameter a driver binding supplies per frame. A new alternative appearing is deliberately
// not a plan-semantics change -- no existing plan value means anything different than it did, and
// the field at this position is still "where this parameter's value comes from" -- which is the
// same reasoning the semantics-version comment at the top of this file already states.
struct CompiledScalarParameter final {
    document::ParameterId id;
    std::variant<double, ScalarCurveIndex, ValueOutputIndex> source;

    friend bool operator==(const CompiledScalarParameter&,
                           const CompiledScalarParameter&) = default;
};

struct CompiledVec2Parameter final {
    document::ParameterId id;
    std::variant<document::Vec2d, Vec2CurveIndex, ValueOutputIndex> source;

    friend bool operator==(const CompiledVec2Parameter&, const CompiledVec2Parameter&) = default;
};

struct CompiledColorParameter final {
    document::ParameterId id;
    std::variant<core::Color4d, Color4CurveIndex, ValueOutputIndex> source;

    friend bool operator==(const CompiledColorParameter&, const CompiledColorParameter&) = default;
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

// A lowered solid source. Its colour is a typed operand rather than a resolved constant (task S5):
// the solid colour schema is animatable now, so the value is either a constant or an index into the
// plan's Color4 curve table -- exactly the shape a Layer Output transform operand already had.
struct CompiledSolid {
    document::NodeId sourceNodeId;
    CompiledColorParameter color;

    friend bool operator==(const CompiledSolid&, const CompiledSolid&) = default;
};

// A lowered text source. Content stays a resolved constant because a String has no interpolation
// and no command in the surface can put it on a curve; size and colour became typed operands in
// task S5, when both schemas became animatable. Each carries its own parameter identity so a
// diagnostic can name the exact parameter that failed, exactly as CompiledSolid does.
struct CompiledText {
    document::NodeId sourceNodeId;
    document::ParameterId contentParameterId;
    std::string content;
    CompiledScalarParameter size;
    CompiledColorParameter color;

    friend bool operator==(const CompiledText&, const CompiledText&) = default;
};

// A lowered Layer Output boundary. The six parameters appear in the registered authoring order --
// position, anchor, scale, rotation, opacity, blend mode -- and each one carries its own parameter
// identity so a diagnostic can name the exact parameter that failed. The first five are animatable,
// so each is either a resolved constant or an index into the plan's curve tables.
//
// The blend mode is a resolved constant, like CompiledText's three values and for the same reason:
// the schema declares it non-animatable and no command in the surface can put it on a curve. It
// lives HERE, on the layer boundary that owns it, rather than on the stack entry that consumes it
// -- the stack entry is the ordering of layers, and the mode is a property of the layer.
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

    core::RationalTime inPoint{};
    // Absent means full duration; old/default plans retain identical behavior.
    std::optional<core::RationalTime> outPoint{};
    friend bool operator==(const CompiledLayerOutput&, const CompiledLayerOutput&) = default;
};

// Plan v3: an invalid layerId denotes a plain image with Normal blending.
struct CompiledMergeInput {
    document::LayerSlotId slotId;
    document::LayerId layerId;
    OperationIndex input;

    friend bool operator==(const CompiledMergeInput&, const CompiledMergeInput&) = default;
};

struct CompiledMerge {
    document::NodeId sourceNodeId;
    std::vector<CompiledMergeInput> entries;

    friend bool operator==(const CompiledMerge&, const CompiledMerge&) = default;
};

struct CompiledCompositionOutput {
    document::NodeId sourceNodeId;
    OperationIndex input;

    friend bool operator==(const CompiledCompositionOutput&,
                           const CompiledCompositionOutput&) = default;
};

using CompiledOperation = std::variant<CompiledSolid, CompiledText, CompiledLayerOutput,
                                       CompiledMerge, CompiledCompositionOutput>;

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
    std::vector<CompiledColor4Curve> color4Curves{};
    // The value graph (task S7), in topological order: every operation's operands name only earlier
    // outputs, so one linear sweep evaluates the whole of it. `valueOutputCount` is the size of the
    // flat output table the operations' runs partition, kept explicitly so the evaluator can
    // bounds- check a ValueOutputIndex without summing the operations first.
    std::vector<CompiledValueOperation> valueOperations{};
    std::size_t valueOutputCount = 0;
    std::uint32_t planSemanticsVersion = kCompiledCompositionPlanSemanticsVersion;
    std::uint32_t animationSamplingSemanticsVersion = kAnimationSamplingSemanticsVersion;

    bool bypassOperationCache = false;

    friend bool operator==(const CompiledCompositionPlanDefinition&,
                           const CompiledCompositionPlanDefinition&) = default;
};

// Immutable after construction. Frames may retain this object through shared ownership without a
// per-frame plan copy because no public API can mutate the published fields or vector storage.
class CompiledCompositionPlan final {
  public:
    explicit CompiledCompositionPlan(CompiledCompositionPlanDefinition definition);

    CompiledCompositionPlan(const CompiledCompositionPlan&) = delete;
    CompiledCompositionPlan& operator=(const CompiledCompositionPlan&) = delete;
    CompiledCompositionPlan(CompiledCompositionPlan&&) = delete;
    CompiledCompositionPlan& operator=(CompiledCompositionPlan&&) = delete;
    ~CompiledCompositionPlan() = default;

    [[nodiscard]] bool bypassOperationCache() const noexcept { return bypassOperationCache_; }
    [[nodiscard]] bool operationTimeDependent(OperationIndex index) const noexcept {
        return index.value() >= operationTimeDependent_.size() ||
               operationTimeDependent_[index.value()] != 0;
    }
    [[nodiscard]] std::span<const std::uint8_t> valueTimeDependence() const noexcept {
        return valueTimeDependent_;
    }
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
    [[nodiscard]] std::span<const CompiledColor4Curve> color4Curves() const& noexcept {
        return color4Curves_;
    }
    [[nodiscard]] std::span<const CompiledColor4Curve> color4Curves() const&& = delete;
    [[nodiscard]] std::span<const CompiledValueOperation> valueOperations() const& noexcept {
        return valueOperations_;
    }
    [[nodiscard]] std::span<const CompiledValueOperation> valueOperations() const&& = delete;
    [[nodiscard]] std::size_t valueOutputCount() const noexcept { return valueOutputCount_; }
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
    bool bypassOperationCache_ = false;
    void analyzeTimeDependence();
    std::vector<std::uint8_t> operationTimeDependent_;
    std::vector<std::uint8_t> valueTimeDependent_;
    document::Revision sourceRevision_;
    document::ProjectId projectId_;
    document::CompositionId compositionId_;
    document::CompositionFormat format_;
    std::vector<CompiledOperation> operations_;
    OperationIndex output_;
    std::vector<CompiledScalarCurve> scalarCurves_;
    std::vector<CompiledVec2Curve> vec2Curves_;
    std::vector<CompiledColor4Curve> color4Curves_;
    std::vector<CompiledValueOperation> valueOperations_;
    std::size_t valueOutputCount_ = 0;
    std::uint32_t planSemanticsVersion_ = kCompiledCompositionPlanSemanticsVersion;
    std::uint32_t animationSamplingSemanticsVersion_ = kAnimationSamplingSemanticsVersion;
};

} // namespace bloom::runtime
