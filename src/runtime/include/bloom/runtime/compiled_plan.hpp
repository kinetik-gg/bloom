#pragma once

#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/render/embedded_fonts.hpp>
#include <bloom/render/image_coverage.hpp>
#include <bloom/runtime/channel_expression.hpp>
#include <bloom/runtime/compiled_curves.hpp>
#include <bloom/runtime/compiled_value_graph.hpp>
#include <bloom/runtime/composition_time.hpp>
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
// COLOR-3 freezes the reusable image-effect input/kernel grammar as plan semantics 8.
// ADR 0024 advances 8 -> 9: operand sampling and media selection are relative to an operation's
// time clock, and Layer Output ranges are in placement-clock time.
inline constexpr std::uint32_t kCompiledCompositionPlanSemanticsVersion = 10;
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
// Linear and radial generators share the Solid source's explicit extent and pixel-window path.
// Their colours interpolate as straight scene-linear RGBA, then premultiply once for storage.
struct CompiledGradient final {
    bool radial = false;
    CompiledColorParameter endColor;
    CompiledVec2Parameter start;
    CompiledVec2Parameter end;
    document::ParameterId repeatId;
    std::int64_t repeat = 0; // 0 clamp, 1 repeat, 2 mirror
    friend bool operator==(const CompiledGradient&, const CompiledGradient&) = default;
};

struct CompiledSolid {
    CompiledSolid() = default;
    CompiledSolid(document::NodeId nodeId, CompiledColorParameter authoredColor,
                  CompiledScalarParameter authoredWidth, CompiledScalarParameter authoredHeight,
                  std::optional<CompiledGradient> authoredGradient = {})
        : sourceNodeId(nodeId), color(std::move(authoredColor)), width(std::move(authoredWidth)),
          height(std::move(authoredHeight)), gradient(std::move(authoredGradient)) {}
    document::NodeId sourceNodeId;
    CompiledColorParameter color;
    CompiledScalarParameter width;
    CompiledScalarParameter height;
    std::optional<CompiledGradient> gradient;

    friend bool operator==(const CompiledSolid&, const CompiledSolid&) = default;
};

// Checker colours are straight authored values; cell and canvas dimensions are in authored
// pixels. The reference and resident paths share a 4x4 sample grid at output resolution.
struct CompiledChecker final {
    document::NodeId sourceNodeId;
    CompiledColorParameter colorA;
    CompiledColorParameter colorB;
    std::uint32_t cellWidth = 64;
    std::uint32_t cellHeight = 64;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    friend bool operator==(const CompiledChecker&, const CompiledChecker&) = default;
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

// Image effects publish finite premultiplied RGBA32F. Pointwise colour kernels preserve windows
// and alpha while operating on straight RGB; neighborhood kernels declare their own associated-
// RGBA and window/halo policy. Canvas and spatial kernels can change image windows. Identity and
// bypass share the input storage exactly.
struct IdentityImageKernel final {
    friend bool operator==(const IdentityImageKernel&, const IdentityImageKernel&) = default;
};
struct CstKernel final {
    std::string fromId;
    std::string toId;
    friend bool operator==(const CstKernel&, const CstKernel&) = default;
};
struct FileTransformKernel final {
    document::AssetId lutAssetId;
    std::int64_t interpolation = 0;
    std::int64_t direction = 0;
    std::string processSpaceId;
    std::optional<document::AssetRecord> asset;
    friend bool operator==(const FileTransformKernel&, const FileTransformKernel&) = default;
};
// Scene-linear, straight-RGB affine effects. The sampled parameter is absent for Invert.
// Every mode preserves alpha and all image windows; no display-referred clamp is implied.
enum class PointwiseImageEffect : std::uint8_t { Exposure, Offset, Multiply, Invert };
struct PointwiseImageKernel final {
    PointwiseImageEffect effect = PointwiseImageEffect::Invert;
    std::optional<CompiledScalarParameter> amount;
    friend bool operator==(const PointwiseImageKernel&, const PointwiseImageKernel&) = default;
};
enum class ImageCombineMode : std::uint8_t { Mix, SetAlpha, MultiplyAlpha };
struct ImageCombineKernel final {
    ImageCombineMode mode = ImageCombineMode::Mix;
    std::optional<CompiledScalarParameter> factor;
    render::ImageCoverageChannel channel = render::ImageCoverageChannel::Alpha;
    std::optional<ValueOutputIndex> drivenChannel = std::nullopt;
    // #361: the constant matte selector's invert half (render::MatteSelection). A driven selector
    // carries its own invert in the sampled stored value, so this stays false beside a driver.
    // Appended with a false default: no existing plan value changes meaning.
    bool invert = false;
    friend bool operator==(const ImageCombineKernel&, const ImageCombineKernel&) = default;
};
enum class CanvasImageEffect : std::uint8_t { Crop, Pad };
struct CanvasImageKernel final {
    CanvasImageEffect effect = CanvasImageEffect::Crop;
    std::array<CompiledScalarParameter, 4> margins;
    friend bool operator==(const CanvasImageKernel&, const CanvasImageKernel&) = default;
};
// Exact permutations of the stored pixel lattice, with no colour/alpha arithmetic. Odd turns
// exchange data-window extents at its top-left; the composition display canvas/PAR are retained.
struct DiscreteImageKernel final {
    bool horizontal = false;
    bool vertical = false;
    std::uint8_t quarterTurns = 0; // clockwise, canonical 0..3
    [[nodiscard]] bool identity() const noexcept {
        return !horizontal && !vertical && quarterTurns == 0;
    }
    friend bool operator==(const DiscreteImageKernel&, const DiscreteImageKernel&) = default;
};
// Constant output geometry. Resize stretches; Fit uses a physical-aspect-preserving fit/fill.
// Bilinear is the initial explicitly versioned filter (0), shared with Layer Output.
struct ResizeFitKernel final {
    std::int64_t width = 1920;
    std::int64_t height = 1080;
    std::int64_t pixelAspectNumerator = 1;
    std::int64_t pixelAspectDenominator = 1;
    std::int64_t filter = 0;
    // 0 = Resize (stretch), 1 = Fit (contain), 2 = Fill (cover).
    std::int64_t policy = 0;
    friend bool operator==(const ResizeFitKernel&, const ResizeFitKernel&) = default;
};
// Neighborhood filters operate on associated RGBA, including coverage. Radius is finite full-
// resolution pixels in [0,256]; Gaussian sigma is radius/3 and support is ceil(radius).
enum class SeparableBlurKind : std::uint8_t { Gaussian, Box };
enum class BlurEdgeMode : std::uint8_t { Transparent, Clamp };
struct SeparableBlurKernel final {
    SeparableBlurKind kind = SeparableBlurKind::Gaussian;
    CompiledScalarParameter radius;
    BlurEdgeMode edge = BlurEdgeMode::Transparent;
    friend bool operator==(const SeparableBlurKernel&, const SeparableBlurKernel&) = default;
};
// ADR 0028 layer masks. A path parameter is a constant or an inline Path curve.
struct CompiledPathParameter final {
    document::ParameterId id;
    std::variant<document::PathValue, CompiledPathCurve> source;
    friend bool operator==(const CompiledPathParameter&, const CompiledPathParameter&) = default;
};
struct CompiledLayerMask final {
    document::LayerMaskId id;
    document::LayerMaskMode mode = document::LayerMaskMode::Add;
    bool inverted = false;
    CompiledPathParameter path;
    CompiledScalarParameter feather;
    CompiledScalarParameter expansion;
    CompiledScalarParameter opacity;
    friend bool operator==(const CompiledLayerMask&, const CompiledLayerMask&) = default;
};
// Multiplies the input, in premultiplied space, by the masks' accumulated coverage over the input's
// own pixel space (the layer's content space). The data window is unchanged. Every math detail is
// in docs/decisions/0028-layer-masks-and-track-mattes.md section 2.
struct LayerMaskKernel final {
    std::vector<CompiledLayerMask> masks;
    friend bool operator==(const LayerMaskKernel&, const LayerMaskKernel&) = default;
};
using ImageEffectKernel = std::variant<IdentityImageKernel, CstKernel, FileTransformKernel,
                                       PointwiseImageKernel, ImageCombineKernel, CanvasImageKernel,
                                       DiscreteImageKernel, ResizeFitKernel, SeparableBlurKernel, LayerMaskKernel>;
struct CompiledImageEffect final {
    document::NodeId sourceNodeId;
    OperationIndex input;
    ImageEffectKernel kernel;
    bool bypass = false;
    bool look = false;
    // Second image/matte for flat RGBA combines. The base input owns the output data/display
    // windows; the second input is sampled by global pixel coordinate and clipped to those windows.
    std::optional<OperationIndex> secondaryInput{};
    // ADR 0028: Image Mix's optional Mask (coverage in the alpha channel of the mask transport),
    // sampled by global pixel coordinate; its per-pixel factor is factor * mask.
    std::optional<OperationIndex> maskInput{};
    // ADR 0028: this effect applies a track matte to the Layer Output at `input`; a Merge reads that
    // layer's blend mode and clock through it (see mergeEntryLayerOutput).
    bool trackMatte = false;
    friend bool operator==(const CompiledImageEffect&, const CompiledImageEffect&) = default;
};

struct CompiledImageSource {
    document::NodeId sourceNodeId;
    std::optional<document::AssetRecord> asset;
    std::int64_t startFrame = 0;
    std::int64_t loopMode = 0;
    std::int64_t colorSpace = 0;
    std::string inputColorSpaceId;
    bool premultiply = true;
    // ADR 0026: the OpenEXR part (empty: first) and colour layer (empty: root) of the image output.
    std::string part{};
    std::string layer{};
    friend bool operator==(const CompiledImageSource&, const CompiledImageSource&) = default;
};

// ADR 0026: a Shuffle's image output. The channel expression is resolved against the probed
// sources; the four slots name resolved channels or the constants "0" and "1". Colour mode converts
// the RGB triple from the red slot's source input colour space; Raw places the values unchanged.
// `associated` says the mapped RGB is already premultiplied by the mapped alpha.
struct CompiledChannelShuffle {
    document::NodeId sourceNodeId;
    CompiledChannelExpression channels;
    std::array<std::string, 4> mapping{};
    std::int64_t mode = 0;
    bool associated = true;
    friend bool operator==(const CompiledChannelShuffle&, const CompiledChannelShuffle&) = default;
};

// #364: a Cryptomatte matte. The selected IDs' coverage from the rank pairs of `layer` in the
// channel expression, as the premultiplied white Mask (ADR 0028) in the anchor source's frame. The
// node's Image output with an input image is a MultiplyAlpha effect reading this operation.
struct CompiledCryptomatte {
    document::NodeId sourceNodeId;
    CompiledChannelExpression channels;
    std::string layer{};
    std::vector<std::string> mattes{};
    // #364 Viewer-only plans: the ID-colour preview (every object in its own colour, the selected
    // ones brightened) instead of the matte. Never set by an ordinary composition plan.
    bool preview = false;
    friend bool operator==(const CompiledCryptomatte&, const CompiledCryptomatte&) = default;
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

// ADR 0024: a Layer Output's own clock and its upstream's content clock. Present only for a layer
// whose timing is not identity; the extent is already resolved (a Source extent names no node
// here). The Time Remap operand is sampled on the layer clock.
struct CompiledLayerTime final {
    core::RationalTime startTime{};
    document::LayerSpeed speed{};
    std::optional<CompiledScalarParameter> timeRemap{};
    std::optional<core::RationalTime> extentDuration{};
    document::FrameRate extentFrameRate = document::FrameRate::framesPerSecond24();
    document::LayerTimeMode beforeMode = document::LayerTimeMode::None;
    document::LayerTimeMode afterMode = document::LayerTimeMode::None;
    friend bool operator==(const CompiledLayerTime&, const CompiledLayerTime&) = default;
};

struct CompiledAudioSource final {
    document::NodeId sourceNodeId;
    document::AssetId assetId;
    std::int64_t startFrame = 0;
    CompiledScalarParameter level;
    // #354: a video source's stored loop mode (0 Hold, 1 Loop, 2 PingPong) and its picture extent,
    // so embedded audio wraps exactly as its picture does. An audio source has neither (Hold): the
    // stream plays once.
    std::int64_t loopMode = 0;
    std::optional<TimeExtent> loopExtent{};

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
    // ADR 0024: the layer's own time mapping; audio follows exactly the picture mapping.
    std::optional<CompiledLayerTime> time{};

    friend bool operator==(const CompiledAudioLayer&, const CompiledAudioLayer&) = default;
};

// Audio is intentionally a sibling of the image operation chain. It carries no device, decoded
// samples, or filesystem state; the UI/audio boundary resolves the asset identity to a buffer.
struct CompiledCompositionAudioLayer final {
    CompiledCompositionSource source;
    bool enabled = true;
    bool solo = false;
    core::RationalTime inPoint{}, outPoint{};
    std::optional<CompiledLayerTime> time{};
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
    // Plan v9 (ADR 0024): absent for identity timing.
    std::optional<CompiledLayerTime> time{};
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
    // ADR 0026 §7: the channel set connected to the output's `channels` input, written beside RGBA
    // by layered OpenEXR, and the comma-separated layer names to write (empty: every layer).
    // Neither affects the process image.
    std::optional<CompiledChannelExpression> channelLayers{};
    std::string layers{};

    friend bool operator==(const CompiledCompositionOutput&,
                           const CompiledCompositionOutput&) = default;
};

using CompiledOperation =
    std::variant<CompiledSolid, CompiledChecker, CompiledText, CompiledImageSource,
                 CompiledVideoSource, CompiledLayerOutput, CompiledMerge, CompiledCompositionOutput,
                 CompiledShape, CompiledCompositionSource, CompiledImageEffect,
                 CompiledChannelShuffle, CompiledCryptomatte>;

// ADR 0024 time clocks. Clock 0 is the plan's own clock (the request time). A Layer clock maps its
// parent clock through a layer's start time and speed; a Content clock (whose parent is that Layer
// clock) applies the layer's Time Remap and out-of-extent modes, and may be absent. Every
// operation evaluates its operands on operationClock(i) and is consumed on placementClock(i); the
// two differ only for a retimed Layer Output (layer clock vs. the clock it is placed in). A plan
// with no retimed layer has no clocks and is exactly the pre-v9 operation list.
enum class CompiledClockKind : std::uint8_t { Plan, Layer, Content };
struct CompiledTimeClock final {
    CompiledClockKind kind = CompiledClockKind::Plan;
    std::uint32_t parent = 0;
    // The (placement-clock copy of the) Layer Output whose CompiledLayerTime defines this clock.
    OperationIndex layer = OperationIndex::fromRaw(0);
    friend bool operator==(const CompiledTimeClock&, const CompiledTimeClock&) = default;
};

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
    // ADR 0024. Empty means every operation is on clock 0.
    std::vector<CompiledTimeClock> clocks{};
    std::vector<std::uint32_t> operationClocks{};
    std::vector<std::uint32_t> placementClocks{};

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
    // ADR 0024 clocks; clock 0 is implicit and always present.
    [[nodiscard]] std::span<const CompiledTimeClock> clocks() const& noexcept { return clocks_; }
    [[nodiscard]] std::span<const CompiledTimeClock> clocks() const&& = delete;
    [[nodiscard]] std::size_t clockCount() const noexcept {
        return clocks_.empty() ? 1 : clocks_.size();
    }
    [[nodiscard]] std::uint32_t operationClock(const OperationIndex index) const noexcept {
        return index.value() < operationClocks_.size() ? operationClocks_[index.value()] : 0;
    }
    [[nodiscard]] std::uint32_t placementClock(const OperationIndex index) const noexcept {
        return index.value() < placementClocks_.size() ? placementClocks_[index.value()] : 0;
    }
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
    std::vector<CompiledTimeClock> clocks_;
    std::vector<std::uint32_t> operationClocks_;
    std::vector<std::uint32_t> placementClocks_;
};

// ADR 0028: the operation a Merge entry takes its layer blend mode and clock from -- the entry's
// own input, or the Layer Output behind a track-matte effect.
[[nodiscard]] inline OperationIndex mergeEntryLayerIndex(const CompiledCompositionPlan& plan,
                                                         const OperationIndex input) noexcept {
    if (input.value() < plan.operations().size())
        if (const auto* effect = std::get_if<CompiledImageEffect>(&plan.operations()[input.value()]);
            effect != nullptr && effect->trackMatte)
            return effect->input;
    return input;
}

} // namespace bloom::runtime
