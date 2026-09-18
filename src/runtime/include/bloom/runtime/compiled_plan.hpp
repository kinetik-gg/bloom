#pragma once

#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/render/embedded_fonts.hpp>
#include <bloom/runtime/compiled_curves.hpp>
#include <bloom/runtime/compiled_value_graph.hpp>
#include <bloom/runtime/operation_index.hpp>

#include <array>
#include <bloom/document/asset.hpp>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime {

// A compiled plan never crosses a process or persistence boundary: it is compiled in-process from a
// document snapshot and retained only by frames of that same process. What DOES cross such a
// boundary is ProcessFrameIdentity, and the two numbers it carries for this are
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
// new alternative appearing. TEXT-2 bumps this number to 5 because CompiledText now carries
// font-reference and box-layout grammar. Both numbers below also enter ProcessFrameIdentity and
// therefore every cached/exported frame digest (src/output/process_frame_semantic_identity.cpp),
// which is why the identity goldens were re-derived in the same change.
// COMP-SRC advances 5 -> 6: plans now own a table of nested composition plans,
// indexed by source operations with composition-time mapping operands.
inline constexpr std::uint32_t kCompiledCompositionPlanSemanticsVersion = 7;
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

struct CompiledVec3Parameter final {
    document::ParameterId id;
    std::variant<document::Vec3d, Vec3CurveIndex, ValueOutputIndex> source;

    friend bool operator==(const CompiledVec3Parameter&, const CompiledVec3Parameter&) = default;
};

struct CompiledColorParameter final {
    document::ParameterId id;
    std::variant<core::Color4d, Color4CurveIndex, ValueOutputIndex> source;

    friend bool operator==(const CompiledColorParameter&, const CompiledColorParameter&) = default;
};

// Content rectangles use pixel-edge coordinates in full-resolution local space.
// Bounds are evaluated from the plan's source operands and transformed input bounds at frame time.
struct ContentBounds final {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    [[nodiscard]] bool empty() const noexcept { return right <= left || bottom <= top; }
    [[nodiscard]] document::Vec2d centre() const noexcept {
        return {(left + right) / 2.0, (top + bottom) / 2.0};
    }
    friend bool operator==(const ContentBounds&, const ContentBounds&) = default;
};
struct EvaluatedOperationBounds final {
    ContentBounds local;
    ContentBounds output;
    document::LayerId layerId;
    std::array<document::Vec2d, 4> polygon{};
    document::Vec2d anchor;
    friend bool operator==(const EvaluatedOperationBounds&,
                           const EvaluatedOperationBounds&) = default;
};

// A lowered solid source. Its colour is a typed operand rather than a resolved constant (task S5):
// the solid colour schema is animatable now, so the value is either a constant or an index into the
// plan's Color4 curve table -- exactly the shape a Layer Output transform operand already had.
struct CompiledSolid {
    document::NodeId sourceNodeId;
    CompiledColorParameter color;
    CompiledScalarParameter width;
    CompiledScalarParameter height;

    friend bool operator==(const CompiledSolid&, const CompiledSolid&) = default;
};

// A lowered text source. Size and colour became typed operands in task S5, when both schemas
// became animatable. Content still never INTERPOLATES -- a String has no midpoint and no command in
// the surface can put it on a curve -- but since task DRIVE-1 it can VARY, because a driver binding
// can hand it a new String every frame; the two are different questions, and only the first is
// about curves. Each value carries its own parameter identity so a diagnostic can name the exact
// parameter that failed, exactly as CompiledSolid does.
struct CompiledTextLayout {
    CompiledTextLayout() = default;
    CompiledTextLayout(document::ParameterId alignmentParameterId,
                       const std::int64_t alignmentValue, CompiledScalarParameter lineHeightValue,
                       CompiledScalarParameter letterSpacingValue,
                       std::optional<ValueOutputIndex> drivenAlignmentValue = {})
        : alignmentId(alignmentParameterId), alignment(alignmentValue), lineHeight(lineHeightValue),
          letterSpacing(letterSpacingValue), drivenAlignment(drivenAlignmentValue) {}

    document::ParameterId alignmentId;
    std::int64_t alignment = 0;
    CompiledScalarParameter lineHeight;
    CompiledScalarParameter letterSpacing;
    // Task DRIVE-1. The value-graph output this alignment is driven by, when it is driven at all.
    // See CompiledText::drivenContent for why a kind that cannot interpolate carries its driver
    // beside its authored value rather than as a third alternative of a typed operand.
    std::optional<ValueOutputIndex> drivenAlignment{};
    document::ParameterId boxId;
    document::Vec2d box{};
    document::ParameterId wrapId;
    bool wrap = false;
    document::ParameterId verticalAlignmentId;
    std::int64_t verticalAlignment = 0;
    document::ParameterId anchorModeId;
    std::int64_t anchorMode = 0;
    document::ParameterId overflowId;
    std::int64_t overflow = 0;
    friend bool operator==(const CompiledTextLayout&, const CompiledTextLayout&) = default;
};

class CompiledCompositionPlan;

struct CompiledCompositionTimeMapping final {
    CompiledScalarParameter offset;
    CompiledScalarParameter scale;
    std::int64_t loopMode = 0;
    friend bool operator==(const CompiledCompositionTimeMapping&,
                           const CompiledCompositionTimeMapping&) = default;
};

struct CompiledCompositionSource final {
    document::NodeId sourceNodeId;
    std::size_t nestedPlanIndex = 0;
    CompiledCompositionTimeMapping timeMapping;
    friend bool operator==(const CompiledCompositionSource&,
                           const CompiledCompositionSource&) = default;
};

struct CompiledImageSource {
    document::NodeId sourceNodeId;
    std::optional<document::AssetRecord> asset;
    std::int64_t startFrame = 0;
    std::int64_t loopMode = 0;
    std::int64_t colorSpace = 0;
    std::string inputColorSpaceId;
    bool premultiply = true;
    friend bool operator==(const CompiledImageSource&, const CompiledImageSource&) = default;
};

struct CompiledVideoSource {
    document::NodeId sourceNodeId;
    std::optional<document::AssetRecord> asset;
    std::int64_t startFrame = 0;
    std::int64_t loopMode = 0;
    std::int64_t colorSpace = 0;
    std::string inputColorSpaceId;
    friend bool operator==(const CompiledVideoSource&, const CompiledVideoSource&) = default;
};

struct CompiledAudioSource final {
    document::NodeId sourceNodeId;
    document::AssetId assetId;
    std::int64_t startFrame = 0;
    CompiledScalarParameter level;

    friend bool operator==(const CompiledAudioSource&, const CompiledAudioSource&) = default;
};

struct CompiledAudioLayer final {
    document::NodeId layerOutputNodeId;
    document::LayerId layerId;
    std::size_t sourceIndex = 0;
    bool enabled = true;
    bool solo = false;
    core::RationalTime inPoint{};
    core::RationalTime outPoint{};

    friend bool operator==(const CompiledAudioLayer&, const CompiledAudioLayer&) = default;
};

// Audio is intentionally a sibling of the image operation chain. It carries no device, decoded
// samples, or filesystem state; the UI/audio boundary resolves the asset identity to a buffer.
struct CompiledCompositionAudioLayer final {
    CompiledCompositionSource source;
    bool enabled = true;
    bool solo = false;
    core::RationalTime inPoint{}, outPoint{};
    friend bool operator==(const CompiledCompositionAudioLayer&,
                           const CompiledCompositionAudioLayer&) = default;
};
struct CompositionAudioMix final {
    document::NodeId outputNodeId;
    std::vector<CompiledAudioSource> sources;
    std::vector<CompiledAudioLayer> layers;
    std::vector<CompiledCompositionAudioLayer> nestedLayers{};

    friend bool operator==(const CompositionAudioMix&, const CompositionAudioMix&) = default;
};

struct CompiledShape {
    document::NodeId sourceNodeId;
    document::ShapeKind kind;
    CompiledVec2Parameter size;
    double cornerRadius = 0;
    std::int64_t points = 5;
    double innerRatio = 0.5;
    document::Vec2d lineStart, lineEnd;
    document::PathValue path;
    bool fillEnabled = true;
    CompiledColorParameter fillColor;
    bool strokeEnabled = false;
    CompiledColorParameter strokeColor;
    CompiledScalarParameter strokeWidth;
    document::ShapeStrokeAlign strokeAlign = document::ShapeStrokeAlign::Center;
    document::ShapeStrokeJoin strokeJoin = document::ShapeStrokeJoin::Miter;
    document::ShapeStrokeCap strokeCap = document::ShapeStrokeCap::Butt;
    document::ShapeFillRule fillRule = document::ShapeFillRule::NonZero;
    friend bool operator==(const CompiledShape&, const CompiledShape&) = default;
};

struct CompiledText {
    CompiledText() = default;
    CompiledText(document::NodeId source, document::ParameterId contentParameter,
                 std::string authoredContent, CompiledScalarParameter authoredSize,
                 CompiledColorParameter authoredColor, CompiledTextLayout authoredLayout,
                 std::optional<ValueOutputIndex> contentDriver = {},
                 render::EmbeddedFace authoredFace = render::EmbeddedFace::DejaVuSans)
        : sourceNodeId(source), contentParameterId(contentParameter),
          content(std::move(authoredContent)), size(authoredSize), color(authoredColor),
          layout(authoredLayout), drivenContent(contentDriver), face(authoredFace) {}

    document::NodeId sourceNodeId;
    document::ParameterId contentParameterId;
    std::string content;
    CompiledScalarParameter size;
    CompiledColorParameter color;
    CompiledTextLayout layout;
    // Task DRIVE-1. Present exactly when the content parameter carries a driver binding; `content`
    // is then the empty authored fallback the parameter store no longer holds a constant for, and
    // the evaluator reads the String this output produces instead.
    //
    // This is an OPTIONAL BESIDE the constant rather than a third alternative of a typed
    // CompiledStringParameter, and deliberately so. A String, an Integer and a Boolean have no
    // curve table -- none of the three interpolates -- so a typed operand for them would carry
    // exactly these two alternatives and nothing more, while changing `content` from a std::string
    // into one WOULD be "a field's meaning changing" in the sense the semantics-version comment at
    // the top of this file describes, and would therefore move
    // kCompiledCompositionPlanSemanticsVersion and with it every process-frame semantic identity
    // digest (src/output/process_frame_semantic_identity.cpp hashes that number). A new optional
    // field appearing is the "new alternative" case that same comment names as NOT a semantics
    // change: an existing plan value still means precisely what it meant, and every pixel and
    // every digest an existing plan produces is unchanged.
    std::optional<ValueOutputIndex> drivenContent{};

    // The render face is a closed, non-animatable choice. It is optional at the document binding
    // boundary for old text-source nodes, which lower to this default and therefore retain the
    // pre-FONT-1 DejaVu Sans pixels exactly.
    render::EmbeddedFace face = render::EmbeddedFace::DejaVuSans;
    document::AssetId fontAssetId;
    render::TextFont font = render::EmbeddedFace::DejaVuSans;

    friend bool operator==(const CompiledText&, const CompiledText&) = default;
};

// A lowered Layer Output boundary. The six parameters appear in the registered authoring order --
// position, anchor, scale, rotation, opacity, blend mode -- and each one carries its own parameter
// identity so a diagnostic can name the exact parameter that failed. The first five are animatable,
// so each is either a resolved constant or an index into the plan's curve tables.
//
// The blend mode is non-animatable -- the schema says so, and there is no meaningful value between
// Multiply and Screen for a curve to interpolate -- so it has no curve alternative. It can still be
// DRIVEN (task DRIVE-1): an Integer node handing a layer its mode per frame is a jump between two
// named modes, which is exactly what a driver expresses and a curve cannot. It lives HERE, on the
// layer boundary that owns it, rather than on the stack entry that consumes it -- the stack entry
// is the ordering of layers, and the mode is a property of the layer.
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
    // Absent means full composition duration.
    std::optional<core::RationalTime> outPoint{};
    // Position places centre(local bounds) + anchor at the authored parent-space point.
    // Task DRIVE-1. The value-graph output the blend mode is driven by, when it is driven. The
    // Integer it produces is mapped through core::blendModeFromStoredValue(), the same closed
    // enumeration an authored one goes through, so a driven mode and an authored mode are the same
    // set of modes. `blendMode` beside it stays the authored constant, or the registry default when
    // the parameter holds no constant at all because it is driven. See CompiledText::drivenContent
    // for why the driver sits beside the constant rather than inside a typed operand.
    std::optional<ValueOutputIndex> drivenBlendMode{};
    // Plan v4: authored transform dependency, evaluated before this boundary.
    std::optional<OperationIndex> parent{};
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

using CompiledOperation =
    std::variant<CompiledSolid, CompiledText, CompiledImageSource, CompiledVideoSource,
                 CompiledLayerOutput, CompiledMerge, CompiledCompositionOutput, CompiledShape,
                 CompiledCompositionSource>;

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
    std::vector<CompiledVec3Curve> vec3Curves{};
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
    CompositionAudioMix audioMix{};
    std::vector<std::shared_ptr<const CompiledCompositionPlan>> nestedPlans{};
    core::RationalTime duration{};

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

    [[nodiscard]] std::span<const std::shared_ptr<const CompiledCompositionPlan>>
    nestedPlans() const noexcept {
        return nestedPlans_;
    }
    [[nodiscard]] core::RationalTime duration() const noexcept { return duration_; }
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
    [[nodiscard]] std::span<const CompiledVec3Curve> vec3Curves() const& noexcept {
        return vec3Curves_;
    }
    [[nodiscard]] std::span<const CompiledVec3Curve> vec3Curves() const&& = delete;
    [[nodiscard]] std::span<const CompiledColor4Curve> color4Curves() const& noexcept {
        return color4Curves_;
    }
    [[nodiscard]] std::span<const CompiledColor4Curve> color4Curves() const&& = delete;
    [[nodiscard]] std::span<const CompiledValueOperation> valueOperations() const& noexcept {
        return valueOperations_;
    }
    [[nodiscard]] std::span<const CompiledValueOperation> valueOperations() const&& = delete;
    [[nodiscard]] std::size_t valueOutputCount() const noexcept { return valueOutputCount_; }
    [[nodiscard]] const CompositionAudioMix& audioMix() const& noexcept { return audioMix_; }
    [[nodiscard]] const CompositionAudioMix& audioMix() const&& = delete;
    [[nodiscard]] std::uint32_t planSemanticsVersion() const noexcept {
        return planSemanticsVersion_;
    }
    [[nodiscard]] std::uint32_t animationSamplingSemanticsVersion() const noexcept {
        return animationSamplingSemanticsVersion_;
    }

    // This is intentionally an allocating deep copy for tests and tooling that need a mutable
    // candidate definition. It never exposes aliases into the published plan.
    [[nodiscard]] CompiledCompositionPlanDefinition copyDefinition() const;

    // Memoization controls and derived dependence tables are not semantic identity inputs.
    friend bool operator==(const CompiledCompositionPlan&, const CompiledCompositionPlan&);

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
    std::vector<CompiledVec3Curve> vec3Curves_;
    std::vector<CompiledColor4Curve> color4Curves_;
    std::vector<CompiledValueOperation> valueOperations_;
    std::size_t valueOutputCount_ = 0;
    std::uint32_t planSemanticsVersion_ = kCompiledCompositionPlanSemanticsVersion;
    std::uint32_t animationSamplingSemanticsVersion_ = kAnimationSamplingSemanticsVersion;
    CompositionAudioMix audioMix_{};
    std::vector<std::shared_ptr<const CompiledCompositionPlan>> nestedPlans_;
    core::RationalTime duration_{};
};

} // namespace bloom::runtime
