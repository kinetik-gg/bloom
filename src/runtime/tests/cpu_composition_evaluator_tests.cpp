#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/safe_parse.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image_types.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace core = bloom::core;
namespace document = bloom::document;
namespace render = bloom::render;
namespace runtime = bloom::runtime;

template <typename Plan>
concept HasMutableOperations = requires(Plan& plan, runtime::CompiledOperation operation) {
    plan.operations().push_back(std::move(operation));
};

static_assert(!std::is_copy_constructible_v<runtime::CompiledCompositionPlan>);
static_assert(!std::is_move_constructible_v<runtime::CompiledCompositionPlan>);
static_assert(!HasMutableOperations<runtime::CompiledCompositionPlan>);
static_assert(
    std::is_const_v<std::remove_reference_t<
        decltype(std::declval<const runtime::CompiledCompositionPlan&>().operations().front())>>);

constexpr auto kProjectId = document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = document::CompositionId::fromRaw(2);
constexpr auto kSolidNodeA = document::NodeId::fromRaw(10);
constexpr auto kLayerNodeA = document::NodeId::fromRaw(11);
constexpr auto kSolidNodeB = document::NodeId::fromRaw(12);
constexpr auto kLayerNodeB = document::NodeId::fromRaw(13);
constexpr auto kStackNode = document::NodeId::fromRaw(14);
constexpr auto kOutputNode = document::NodeId::fromRaw(15);
constexpr auto kLayerA = document::LayerId::fromRaw(20);
constexpr auto kLayerB = document::LayerId::fromRaw(21);
constexpr auto kSlotA = document::LayerSlotId::fromRaw(30);
constexpr auto kSlotB = document::LayerSlotId::fromRaw(31);
constexpr auto kColorA = document::ParameterId::fromRaw(40);
constexpr auto kPositionA = document::ParameterId::fromRaw(41);
constexpr auto kOpacityA = document::ParameterId::fromRaw(42);
constexpr auto kColorB = document::ParameterId::fromRaw(43);
constexpr auto kPositionB = document::ParameterId::fromRaw(44);
constexpr auto kOpacityB = document::ParameterId::fromRaw(45);
constexpr auto kOpacityCurve = document::AnimationCurveId::fromRaw(50);
constexpr auto kPositionCurve = document::AnimationCurveId::fromRaw(51);
constexpr auto kTextNode = document::NodeId::fromRaw(16);
// Task DRIVE-1's value nodes: the frame readout, the decimal conversion of it, and the literal
// Integer that hands a layer its blend mode.
constexpr auto kFrameNumberNode = document::NodeId::fromRaw(17);
constexpr auto kIntegerToStringNode = document::NodeId::fromRaw(18);
constexpr auto kBlendModeNode = document::NodeId::fromRaw(19);
constexpr auto kTextContent = document::ParameterId::fromRaw(46);
constexpr auto kTextSize = document::ParameterId::fromRaw(47);
constexpr auto kTextColor = document::ParameterId::fromRaw(48);
constexpr auto kAnchorA = document::ParameterId::fromRaw(49);
constexpr auto kScaleA = document::ParameterId::fromRaw(52);
constexpr auto kRotationA = document::ParameterId::fromRaw(53);
constexpr auto kAnchorB = document::ParameterId::fromRaw(54);
constexpr auto kScaleB = document::ParameterId::fromRaw(55);
constexpr auto kRotationB = document::ParameterId::fromRaw(56);
constexpr auto kBlendModeA = document::ParameterId::fromRaw(57);
constexpr auto kBlendModeB = document::ParameterId::fromRaw(58);
constexpr auto kAnchorCurve = document::AnimationCurveId::fromRaw(55);
constexpr auto kScaleCurve = document::AnimationCurveId::fromRaw(56);
constexpr auto kRotationCurve = document::AnimationCurveId::fromRaw(57);
constexpr auto kColorCurve = document::AnimationCurveId::fromRaw(58);
constexpr auto kTextSizeCurve = document::AnimationCurveId::fromRaw(59);

// The authored transform and blending a Layer Output carries. Defaulted to the identity -- no
// anchor offset, unit scale, no rotation, Normal blending -- so a fixture that cares only about
// position or opacity reads exactly as it did before the transform breadth slice, and a fixture
// that cares about the transform or the blend mode names only the value it is exercising.
struct LayerTransformValues final {
    document::Vec2d position{2.0, 1.0};
    document::Vec2d anchor = document::kDefaultAnchor;
    document::Vec2d scale = document::kDefaultScale;
    double rotation = document::kDefaultRotationDegrees;
    double opacity = 1.0;
    core::BlendMode blendMode = core::kDefaultBlendMode;
};

struct LayerParameterIds final {
    document::ParameterId position;
    document::ParameterId anchor;
    document::ParameterId scale;
    document::ParameterId rotation;
    document::ParameterId opacity;
    document::ParameterId blendMode;
};

inline constexpr LayerParameterIds kLayerParametersA{kPositionA, kAnchorA,  kScaleA,
                                                     kRotationA, kOpacityA, kBlendModeA};
inline constexpr LayerParameterIds kLayerParametersB{kPositionB, kAnchorB,  kScaleB,
                                                     kRotationB, kOpacityB, kBlendModeB};

[[nodiscard]] runtime::CompiledLayerOutput layerOutput(const document::NodeId nodeId,
                                                       const document::LayerId layerId,
                                                       const runtime::OperationIndex input,
                                                       const LayerParameterIds ids,
                                                       const LayerTransformValues values) {
    return runtime::CompiledLayerOutput{
        nodeId,
        layerId,
        input,
        runtime::CompiledVec2Parameter{ids.position, values.position},
        runtime::CompiledVec2Parameter{ids.anchor, values.anchor},
        runtime::CompiledVec2Parameter{ids.scale, values.scale},
        runtime::CompiledScalarParameter{ids.rotation, values.rotation},
        runtime::CompiledScalarParameter{ids.opacity, values.opacity},
        ids.blendMode,
        values.blendMode};
}

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] document::CompositionFormat format(const std::uint32_t width = 4,
                                                 const std::uint32_t height = 2) {
    const auto value = document::CompositionFormat::create(width, height);
    if (!value.has_value()) {
        throw std::logic_error("evaluation test format must be valid");
    }
    return *value;
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
publishPlan(runtime::CompiledCompositionPlanDefinition definition) {
    return std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
oneSolidPlan(const core::Color4d color = {1.0, 0.0, 0.0, 1.0},
             const document::Vec2d position = {2.0, 1.0}, const double opacity = 1.0,
             const document::CompositionFormat compositionFormat = format()) {
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledSolid{
        kSolidNodeA,
        {kColorA, color},
        {bloom::document::ParameterId::fromRaw(kSolidNodeA.value() * 100 + 1000),
         static_cast<double>(compositionFormat.width())},
        {bloom::document::ParameterId::fromRaw(kSolidNodeA.value() * 100 + 1001),
         static_cast<double>(compositionFormat.height())}});
    operations.emplace_back(layerOutput(kLayerNodeA, kLayerA, runtime::OperationIndex::fromRaw(0),
                                        kLayerParametersA,
                                        {.position = position, .opacity = opacity}));
    operations.emplace_back(runtime::CompiledMerge{
        kStackNode, {{kSlotA, kLayerA, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNode, runtime::OperationIndex::fromRaw(2)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{
            document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
            std::move(operations), runtime::OperationIndex::fromRaw(3)});
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
twoSolidPlan(const bool redOnTop = true) {
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledSolid{
        kSolidNodeA,
        {kColorA, core::Color4d{1.0, 0.0, 0.0, 0.5}},
        {bloom::document::ParameterId::fromRaw(kSolidNodeA.value() * 100 + 1000), 4.0},
        {bloom::document::ParameterId::fromRaw(kSolidNodeA.value() * 100 + 1001), 2.0}});
    operations.emplace_back(layerOutput(kLayerNodeA, kLayerA, runtime::OperationIndex::fromRaw(0),
                                        kLayerParametersA, {}));
    operations.emplace_back(runtime::CompiledSolid{
        kSolidNodeB,
        {kColorB, core::Color4d{0.0, 0.0, 1.0, 1.0}},
        {bloom::document::ParameterId::fromRaw(kSolidNodeB.value() * 100 + 1000), 4.0},
        {bloom::document::ParameterId::fromRaw(kSolidNodeB.value() * 100 + 1001), 2.0}});
    operations.emplace_back(layerOutput(kLayerNodeB, kLayerB, runtime::OperationIndex::fromRaw(2),
                                        kLayerParametersB, {}));
    const runtime::CompiledMergeInput red{kSlotA, kLayerA, runtime::OperationIndex::fromRaw(1)};
    const runtime::CompiledMergeInput blue{kSlotB, kLayerB, runtime::OperationIndex::fromRaw(3)};
    operations.emplace_back(runtime::CompiledMerge{kStackNode, redOnTop ? std::vector{red, blue}
                                                                        : std::vector{blue, red}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNode, runtime::OperationIndex::fromRaw(4)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{document::Revision::fromRaw(7), kProjectId,
                                                   kCompositionId, format(), std::move(operations),
                                                   runtime::OperationIndex::fromRaw(5)});
}

// U+2588 FULL BLOCK at 12 px per em in the embedded DejaVu Sans face. Chosen because it makes the
// evaluator's placement and compositing testable with exact, independently stated pixel
// coordinates: the rasterized bitmap is 11 x 15 at text-origin offset (-1, -1), and its interior 9
// x 13 is coverage 255 exactly, so a frame pixel under that interior must equal the premultiplied
// text color bit for bit. The glyph's own coverage bytes are pinned separately by
// src/render/tests/text_raster_test.cpp's byte-exact golden; this fixture's job is WHERE those
// bytes land and what they composite to.
constexpr std::string_view kFullBlock = "\xe2\x96\x88";
constexpr double kFullBlockSize = 12.0;
// The glyph box is centred on the layer's authored position, so its fractional edge columns and
// rows fall outside the fully covered interior that starts here.
constexpr std::int64_t kFullBlockFullCoverageOriginX = 4;
constexpr std::int64_t kFullBlockFullCoverageOriginY = 4;
constexpr std::int64_t kFullBlockFullCoverageWidth = 8;
constexpr std::int64_t kFullBlockFullCoverageHeight = 13;

// A single text layer centered so its Layer Output translation is exactly zero, which keeps the
// bilinear resample an identity and lets the composed frame be compared exactly rather than within
// a tolerance.
[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
oneTextPlan(const core::Color4d color = {0.5, 0.25, 0.75, 1.0},
            const std::string& content = std::string(kFullBlock),
            const double size = kFullBlockSize, const double opacity = 1.0,
            const document::Vec2d position = {8.0, 10.0},
            const document::CompositionFormat compositionFormat = format(16, 20)) {
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledText{kTextNode,
                                                  kTextContent,
                                                  content,
                                                  {kTextSize, size},
                                                  {kTextColor, color},
                                                  {{document::ParameterId::fromRaw(901)},
                                                   0,
                                                   {document::ParameterId::fromRaw(902), 1.0},
                                                   {document::ParameterId::fromRaw(903), 0.0}}});
    operations.emplace_back(layerOutput(kLayerNodeA, kLayerA, runtime::OperationIndex::fromRaw(0),
                                        kLayerParametersA,
                                        {.position = position, .opacity = opacity}));
    operations.emplace_back(runtime::CompiledMerge{
        kStackNode, {{kSlotA, kLayerA, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNode, runtime::OperationIndex::fromRaw(2)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{
            document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
            std::move(operations), runtime::OperationIndex::fromRaw(3)});
}

[[nodiscard]] runtime::EvaluationRequest
requestFor(const runtime::CompiledCompositionPlan& plan, std::size_t budget = 1U << 20U,
           runtime::EvaluationResolution resolution = runtime::CompositionFormatResolution{});

void addLayerBoundsReadout(runtime::CompiledCompositionPlanDefinition& definition,
                           const runtime::OperationIndex operationIndex,
                           const document::NodeId nodeId) {
    definition.valueOperations.push_back({nodeId, runtime::ValueOutputIndex::fromRaw(0), 4,
                                          runtime::CompiledBoundsReadout{operationIndex}, true});
    definition.valueOutputCount = 4;
}

void testLayerBoundsReadout(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    auto solidDefinition =
        oneSolidPlan({1.0, 0.0, 0.0, 1.0}, {960.0, 540.0}, 1.0, document::CompositionFormat{})
            ->copyDefinition();
    auto& layer = std::get<runtime::CompiledLayerOutput>(solidDefinition.operations[1]);
    layer.anchor.source = document::Vec2d{12.0, 18.0};
    addLayerBoundsReadout(solidDefinition, runtime::OperationIndex::fromRaw(1),
                          document::NodeId::fromRaw(900));
    const auto solidPlan = publishPlan(std::move(solidDefinition));
    const auto solid = evaluator.evaluate(solidPlan, requestFor(*solidPlan, 1U << 28U), {});
    expectations.expect(solid.frame() != nullptr,
                        "a 1920x1080 Solid Layer Bounds readout evaluates");
    if (solid.frame() != nullptr) {
        const auto values = solid.frame()->valueOutputs();
        const auto vec2 = [&](const std::size_t index) {
            return index < values.size() ? std::get_if<document::Vec2d>(&values[index]) : nullptr;
        };
        expectations.expect(vec2(0) != nullptr && *vec2(0) == document::Vec2d{1920.0, 1080.0},
                            "a Solid readout reports its full size");
        expectations.expect(vec2(1) != nullptr && *vec2(1) == document::Vec2d{-12.0, -18.0},
                            "a Solid readout reports the authored-anchor origin");
        expectations.expect(vec2(2) != nullptr && *vec2(2) == document::Vec2d{960.0, 540.0},
                            "a Solid readout reports the evaluated anchor");
        expectations.expect(vec2(3) != nullptr && *vec2(3) == document::Vec2d{948.0, 522.0},
                            "a Solid readout reports the evaluated bounds center");
    }

    auto shortTextDefinition = oneTextPlan({1.0, 1.0, 1.0, 1.0}, "A")->copyDefinition();
    addLayerBoundsReadout(shortTextDefinition, runtime::OperationIndex::fromRaw(0),
                          document::NodeId::fromRaw(901));
    auto longTextDefinition = oneTextPlan({1.0, 1.0, 1.0, 1.0}, "AB")->copyDefinition();
    addLayerBoundsReadout(longTextDefinition, runtime::OperationIndex::fromRaw(0),
                          document::NodeId::fromRaw(902));
    const auto shortTextPlan = publishPlan(std::move(shortTextDefinition));
    const auto longTextPlan = publishPlan(std::move(longTextDefinition));
    const auto shortText = evaluator.evaluate(shortTextPlan, requestFor(*shortTextPlan), {});
    const auto longText = evaluator.evaluate(longTextPlan, requestFor(*longTextPlan), {});
    const auto* shortSize =
        shortText.frame() && !shortText.frame()->valueOutputs().empty()
            ? std::get_if<document::Vec2d>(&shortText.frame()->valueOutputs().front())
            : nullptr;
    const auto* longSize =
        longText.frame() && !longText.frame()->valueOutputs().empty()
            ? std::get_if<document::Vec2d>(&longText.frame()->valueOutputs().front())
            : nullptr;
    expectations.expect(shortSize != nullptr && longSize != nullptr && *shortSize != *longSize,
                        "changing Text content changes the Layer Bounds size on the next frame");
}

// Two opaque-format layers whose straight authoring colours premultiply to exactly the values the
// blend-kernel goldens use: the top layer is straight (1, 0.5, 0.25) at half alpha -- premultiplied
// (0.5, 0.25, 0.125, 0.5) -- over an opaque (0.25, 0.5, 0.75) backdrop. Both positions are the
// composition centre, so each layer's transform is the translate-only identity and the resample
// interpolates nothing; every pixel of the published frame is therefore the kernel's exact answer
// rather than a value within a tolerance.
[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
twoSolidBlendPlan(const core::BlendMode topMode, const core::BlendMode bottomMode) {
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledSolid{
        kSolidNodeA,
        {kColorA, core::Color4d{1.0, 0.5, 0.25, 0.5}},
        {bloom::document::ParameterId::fromRaw(kSolidNodeA.value() * 100 + 1000), 4.0},
        {bloom::document::ParameterId::fromRaw(kSolidNodeA.value() * 100 + 1001), 2.0}});
    operations.emplace_back(layerOutput(kLayerNodeA, kLayerA, runtime::OperationIndex::fromRaw(0),
                                        kLayerParametersA, {.blendMode = topMode}));
    operations.emplace_back(runtime::CompiledSolid{
        kSolidNodeB,
        {kColorB, core::Color4d{0.25, 0.5, 0.75, 1.0}},
        {bloom::document::ParameterId::fromRaw(kSolidNodeB.value() * 100 + 1000), 4.0},
        {bloom::document::ParameterId::fromRaw(kSolidNodeB.value() * 100 + 1001), 2.0}});
    operations.emplace_back(layerOutput(kLayerNodeB, kLayerB, runtime::OperationIndex::fromRaw(2),
                                        kLayerParametersB, {.blendMode = bottomMode}));
    operations.emplace_back(
        runtime::CompiledMerge{kStackNode,
                               {{kSlotA, kLayerA, runtime::OperationIndex::fromRaw(1)},
                                {kSlotB, kLayerB, runtime::OperationIndex::fromRaw(3)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNode, runtime::OperationIndex::fromRaw(4)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{document::Revision::fromRaw(7), kProjectId,
                                                   kCompositionId, format(), std::move(operations),
                                                   runtime::OperationIndex::fromRaw(5)});
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan> emptyStackPlan() {
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledMerge{kStackNode, {}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNode, runtime::OperationIndex::fromRaw(0)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{document::Revision::fromRaw(7), kProjectId,
                                                   kCompositionId, format(), std::move(operations),
                                                   runtime::OperationIndex::fromRaw(1)});
}

// A compiled vector or colour curve is one scalar table per component, so a fixture names its
// component tables rather than a whole-value key list.
[[nodiscard]] runtime::CompiledVec2Curve vec2Curve(const document::AnimationCurveId id,
                                                   std::vector<runtime::CompiledScalarKeyframe> x,
                                                   std::vector<runtime::CompiledScalarKeyframe> y) {
    runtime::CompiledVec2Curve curve;
    curve.id = id;
    curve.components[0] = std::move(x);
    curve.components[1] = std::move(y);
    return curve;
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan> animatedLayerPlan() {
    auto definition = oneSolidPlan()->copyDefinition();
    auto& layer = std::get<runtime::CompiledLayerOutput>(definition.operations[1]);
    layer.position.source = runtime::Vec2CurveIndex::fromRaw(0);
    layer.opacity.source = runtime::ScalarCurveIndex::fromRaw(0);
    definition.scalarCurves.push_back(
        {kOpacityCurve,
         {{document::KeyframeId::fromRaw(60), core::RationalTime::fromInteger(0), 1.0,
           runtime::CompiledKeyframeInterpolation::Linear},
          {document::KeyframeId::fromRaw(61), core::RationalTime::fromInteger(1), 0.0,
           runtime::CompiledKeyframeInterpolation::Linear}}});
    definition.vec2Curves.push_back(
        vec2Curve(kPositionCurve,
                  {{document::KeyframeId::fromRaw(62), core::RationalTime::fromInteger(0), 2.0,
                    runtime::CompiledKeyframeInterpolation::Linear},
                   {document::KeyframeId::fromRaw(63), core::RationalTime::fromInteger(1), 3.0,
                    runtime::CompiledKeyframeInterpolation::Linear}},
                  {{document::KeyframeId::fromRaw(64), core::RationalTime::fromInteger(0), 1.0,
                    runtime::CompiledKeyframeInterpolation::Linear},
                   {document::KeyframeId::fromRaw(65), core::RationalTime::fromInteger(1), 1.0,
                    runtime::CompiledKeyframeInterpolation::Linear}}));
    return std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
}

[[nodiscard]] runtime::EvaluationRequest requestFor(const runtime::CompiledCompositionPlan& plan,
                                                    const std::size_t budget,
                                                    runtime::EvaluationResolution resolution) {
    return {.time = core::RationalTime::fromInteger(0),
            .output = plan.output(),
            .resolution = resolution,
            .quality = runtime::EvaluationQuality::Reference,
            .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
            .pixelStorageByteLimit = budget};
}

[[nodiscard]] runtime::ReferenceDisplayPreparationRequest
displayRequest(const std::size_t aggregateBudget = 1U << 20U) {
    return {.intent = runtime::ReferenceDisplayIntent::LinearRec709SceneToSrgb,
            .aggregatePixelStorageByteLimit = aggregateBudget,
            .viewAdjust = {},
            .displayName = {},
            .viewName = {},
            .showLook = true};
}

[[nodiscard]] bool near(const float value, const float expected, const float tolerance = 1.0e-6F) {
    return std::abs(value - expected) <= tolerance;
}

[[nodiscard]] const render::Rgba32f* pixel(const runtime::EvaluationResult& result,
                                           const std::int64_t x, const std::int64_t y,
                                           render::Rgba32f& storage) {
    if (result.frame() == nullptr) {
        return nullptr;
    }
    const auto read = result.frame()->processImage().read(x, y);
    if (!read) {
        return nullptr;
    }
    storage = *read.value();
    return &storage;
}

void testNestedMergeEqualsFlat(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto flat = twoSolidPlan(true);
    auto definition = flat->copyDefinition();
    auto& merge = std::get<runtime::CompiledMerge>(definition.operations[4]);
    const auto top = merge.entries.front();
    merge.entries.erase(merge.entries.begin());
    definition.operations.insert(
        definition.operations.begin() + 5,
        runtime::CompiledMerge{
            document::NodeId::fromRaw(900),
            {top, {document::LayerSlotId::fromRaw(900), {}, runtime::OperationIndex::fromRaw(4)}}});
    std::get<runtime::CompiledCompositionOutput>(definition.operations[6]).input =
        runtime::OperationIndex::fromRaw(5);
    definition.output = runtime::OperationIndex::fromRaw(6);
    const auto nested = publishPlan(std::move(definition));
    const auto expected = evaluator.evaluate(flat, requestFor(*flat), {});
    const auto actual = evaluator.evaluate(nested, requestFor(*nested), {});
    expectations.expect(expected.frame() && actual.frame(), "nested Merge evaluates");
    if (!expected.frame() || !actual.frame())
        return;
    for (std::int64_t y = 0; y < 2; ++y)
        for (std::int64_t x = 0; x < 4; ++x) {
            render::Rgba32f a = render::Rgba32f::transparent(), b = a;
            expectations.expect(pixel(expected, x, y, a) && pixel(actual, x, y, b) && a == b,
                                "two-level Normal Merge is bit-identical to flattened layers");
        }
    auto wrappedDefinition = nested->copyDefinition();
    wrappedDefinition.operations.insert(
        wrappedDefinition.operations.begin() + 6,
        layerOutput(document::NodeId::fromRaw(901), document::LayerId::fromRaw(901),
                    runtime::OperationIndex::fromRaw(5),
                    {document::ParameterId::fromRaw(901), document::ParameterId::fromRaw(902),
                     document::ParameterId::fromRaw(903), document::ParameterId::fromRaw(904),
                     document::ParameterId::fromRaw(905), document::ParameterId::fromRaw(906)},
                    {}));
    wrappedDefinition.operations.insert(
        wrappedDefinition.operations.begin() + 7,
        runtime::CompiledMerge{
            document::NodeId::fromRaw(902),
            {{document::LayerSlotId::fromRaw(902), document::LayerId::fromRaw(901),
              runtime::OperationIndex::fromRaw(6)}}});
    std::get<runtime::CompiledCompositionOutput>(wrappedDefinition.operations[8]).input =
        runtime::OperationIndex::fromRaw(7);
    wrappedDefinition.output = runtime::OperationIndex::fromRaw(8);
    const auto wrapped = publishPlan(std::move(wrappedDefinition));
    const auto wrappedResult = evaluator.evaluate(wrapped, requestFor(*wrapped), {});
    render::Rgba32f originalPixel = render::Rgba32f::transparent(), wrappedPixel = originalPixel;
    expectations.expect(pixel(actual, 1, 1, originalPixel) &&
                            pixel(wrappedResult, 1, 1, wrappedPixel) &&
                            originalPixel == wrappedPixel,
                        "a Merge can feed an identity Layer without changing pixels");
    auto plainDefinition = oneSolidPlan()->copyDefinition();
    std::get<runtime::CompiledMerge>(plainDefinition.operations[2]).entries = {
        {kSlotA, {}, runtime::OperationIndex::fromRaw(0)}};
    plainDefinition.operations.erase(plainDefinition.operations.begin() + 1);
    std::get<runtime::CompiledCompositionOutput>(plainDefinition.operations[2]).input =
        runtime::OperationIndex::fromRaw(1);
    plainDefinition.output = runtime::OperationIndex::fromRaw(2);
    const auto plain = publishPlan(std::move(plainDefinition));
    const auto result = evaluator.evaluate(plain, requestFor(*plain), {});
    render::Rgba32f value = render::Rgba32f::transparent();
    expectations.expect(pixel(result, 1, 1, value) && value.alpha() == 1.0F,
                        "plain source is composited at full opacity");
}

// Task DRIVE-1. A text layer whose WORDS come from the graph, and a layer whose blend MODE does.
// Both are kinds that cannot interpolate -- there is no midpoint between "0" and "7", and none
// between Multiply and Screen -- which is exactly why neither has a curve and why a driver is the
// only thing that can vary them. The two fixtures below are the smallest plans that vary each.

// Frame Number -> Integer To String -> content. Value output 0 is the frame, value output 1 is its
// decimal text, and the text layer's content reads output 1.
[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan> frameNumberTextPlan() {
    auto definition = oneTextPlan()->copyDefinition();
    auto& text = std::get<runtime::CompiledText>(definition.operations.front());
    text.content.clear();
    text.drivenContent = runtime::ValueOutputIndex::fromRaw(1);
    definition.valueOperations.push_back(
        {kFrameNumberNode, runtime::ValueOutputIndex::fromRaw(0), 1,
         runtime::CompiledValueUtility{document::ValueUtilityKernel::FrameNumber, {}, {}, {}}});
    definition.valueOperations.push_back(
        {kIntegerToStringNode, runtime::ValueOutputIndex::fromRaw(1), 1,
         runtime::CompiledValueUtility{document::ValueUtilityKernel::IntegerToString,
                                       {{{}, runtime::ValueOutputIndex::fromRaw(0)},
                                        {{}, runtime::CompiledValue{std::int64_t{0}}},
                                        {{}, runtime::CompiledValue{std::string{}}},
                                        {{}, runtime::CompiledValue{std::string{}}}},
                                       {core::kDefaultRadix},
                                       {}}});
    definition.valueOutputCount = 2;
    return std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
}

// The same two-layer blend fixture, with the TOP layer's mode driven by a literal Integer instead
// of authored. The authored constant beside the driver is deliberately a different mode, so a
// frame that matched it would prove the driver was ignored.
[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
drivenBlendModePlan(const core::BlendMode drivenMode) {
    auto definition =
        twoSolidBlendPlan(core::BlendMode::Normal, core::BlendMode::Normal)->copyDefinition();
    auto& top = std::get<runtime::CompiledLayerOutput>(definition.operations[1]);
    top.drivenBlendMode = runtime::ValueOutputIndex::fromRaw(0);
    definition.valueOperations.push_back(
        {kBlendModeNode, runtime::ValueOutputIndex::fromRaw(0), 1,
         runtime::CompiledValuePassthrough{
             {kBlendModeA, runtime::CompiledValue{core::blendModeStoredValue(drivenMode)}}}});
    definition.valueOutputCount = 1;
    return std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
}

void testDrivenTextContentRendersPerFrame(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto driven = frameNumberTextPlan();
    // 24 fps, so frame 7 is the instant 7/24 -- the exact rational the Frame Number kernel reads,
    // never a rounded seconds value.
    const auto rate = driven->format().frameRate();
    const auto atFrame = [&](const std::int64_t frame) {
        const auto time =
            core::RationalTime::create(frame * static_cast<std::int64_t>(rate.denominator()),
                                       static_cast<std::int64_t>(rate.numerator()));
        if (!time.has_value()) {
            throw std::logic_error("driven text fixture frame time must be valid");
        }
        auto request = requestFor(*driven);
        request.time = *time;
        return evaluator.evaluate(driven, request, {});
    };
    const auto first = atFrame(0);
    const auto eighth = atFrame(7);
    expectations.expect(first.status() == runtime::EvaluationStatus::Evaluated &&
                            eighth.status() == runtime::EvaluationStatus::Evaluated,
                        "a text layer driven by a String-producing chain evaluates at every frame");
    if (first.frame() == nullptr || eighth.frame() == nullptr) {
        return;
    }
    // The golden: each driven frame is compared against the SAME plan with the words it should be
    // showing authored as a constant. Byte-identical, not within a tolerance -- the only difference
    // between the two plans is where the String came from, and where a value came from must never
    // change the pixels it produces.
    const auto zero = oneTextPlan({0.5, 0.25, 0.75, 1.0}, "0");
    const auto seven = oneTextPlan({0.5, 0.25, 0.75, 1.0}, "7");
    const auto authoredZero = evaluator.evaluate(zero, requestFor(*zero), {});
    const auto authoredSeven = evaluator.evaluate(seven, requestFor(*seven), {});
    if (authoredZero.frame() == nullptr || authoredSeven.frame() == nullptr) {
        expectations.expect(false, "the authored-content reference frames evaluate");
        return;
    }
    bool matchesZero = true;
    bool matchesSeven = true;
    bool framesDiffer = false;
    for (std::int64_t y = 0; y < 20; ++y) {
        for (std::int64_t x = 0; x < 16; ++x) {
            render::Rgba32f drivenFirst = render::Rgba32f::transparent();
            render::Rgba32f drivenEighth = drivenFirst;
            render::Rgba32f referenceZero = drivenFirst;
            render::Rgba32f referenceSeven = drivenFirst;
            if (pixel(first, x, y, drivenFirst) == nullptr ||
                pixel(eighth, x, y, drivenEighth) == nullptr ||
                pixel(authoredZero, x, y, referenceZero) == nullptr ||
                pixel(authoredSeven, x, y, referenceSeven) == nullptr) {
                expectations.expect(false, "every pixel of all four frames is readable");
                return;
            }
            matchesZero = matchesZero && drivenFirst == referenceZero;
            matchesSeven = matchesSeven && drivenEighth == referenceSeven;
            framesDiffer = framesDiffer || !(drivenFirst == drivenEighth);
        }
    }
    expectations.expect(matchesZero,
                        "frame 0 of the driven text layer is bit-identical to the same layer with "
                        "\"0\" authored as a constant");
    expectations.expect(matchesSeven,
                        "frame 7 of the driven text layer is bit-identical to the same layer with "
                        "\"7\" authored as a constant");
    expectations.expect(framesDiffer,
                        "and the two frames differ, so the content is re-resolved per frame rather "
                        "than baked once at compile time");

    // The plan says so too: a driven content makes the text operation time-dependent, which is what
    // keeps the operation cache from serving frame 0's words for frame 7.
    expectations.expect(driven->operationTimeDependent(runtime::OperationIndex::fromRaw(0)),
                        "a text operation whose content is driven by the frame is time-dependent");
    expectations.expect(!oneTextPlan()->operationTimeDependent(runtime::OperationIndex::fromRaw(0)),
                        "and an undriven one is not, so nothing else pays for the capability");
}

void testDrivenBlendModeCompositesAsItsResolvedMode(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto driven = drivenBlendModePlan(core::BlendMode::Multiply);
    const auto authored = twoSolidBlendPlan(core::BlendMode::Multiply, core::BlendMode::Normal);
    const auto drivenResult = evaluator.evaluate(driven, requestFor(*driven), {});
    const auto authoredResult = evaluator.evaluate(authored, requestFor(*authored), {});
    expectations.expect(drivenResult.status() == runtime::EvaluationStatus::Evaluated &&
                            authoredResult.status() == runtime::EvaluationStatus::Evaluated,
                        "a layer whose blend mode is driven by an Integer evaluates");
    if (drivenResult.frame() == nullptr || authoredResult.frame() == nullptr) {
        return;
    }
    bool identical = true;
    for (std::int64_t y = 0; y < 2; ++y) {
        for (std::int64_t x = 0; x < 4; ++x) {
            render::Rgba32f left = render::Rgba32f::transparent();
            render::Rgba32f right = left;
            if (pixel(drivenResult, x, y, left) == nullptr ||
                pixel(authoredResult, x, y, right) == nullptr) {
                expectations.expect(false, "every pixel of both blend frames is readable");
                return;
            }
            identical = identical && left == right;
        }
    }
    expectations.expect(identical,
                        "compositing under a driven Multiply is bit-identical to compositing under "
                        "an authored one");
}

void testAbsoluteCenterAndFractionalTranslation(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto centered = oneSolidPlan();
    const auto centeredResult = evaluator.evaluate(centered, requestFor(*centered), {});
    render::Rgba32f sampled = render::Rgba32f::transparent();
    const auto* centerPixel = pixel(centeredResult, 0, 0, sampled);
    expectations.expect(centeredResult.status() == runtime::EvaluationStatus::Evaluated &&
                            centerPixel != nullptr && centerPixel->red() == 1.0F &&
                            centerPixel->alpha() == 1.0F,
                        "absolute composition center is identity for a composition-sized source");

    const auto shifted = oneSolidPlan({1.0, 0.0, 0.0, 1.0}, {2.5, 1.0});
    const auto shiftedResult = evaluator.evaluate(shifted, requestFor(*shifted), {});
    const auto* edgePixel = pixel(shiftedResult, 0, 0, sampled);
    expectations.expect(
        shiftedResult.status() == runtime::EvaluationStatus::Evaluated && edgePixel != nullptr &&
            near(edgePixel->red(), 128.0F / 255.0F) && near(edgePixel->alpha(), 128.0F / 255.0F),
        "fractional center displacement covers the vector edge at output resolution");
    const auto* interiorPixel = pixel(shiftedResult, 1, 0, sampled);
    expectations.expect(interiorPixel != nullptr && interiorPixel->red() == 1.0F &&
                            interiorPixel->alpha() == 1.0F,
                        "fractional translation preserves fully covered interior pixels");
}

// A square white solid layer, centred, so the transform under test is the only thing shaping the
// frame. Every expectation below is derived from the documented model: a 4x4 layer's pixel-area
// centre is (1.5, 1.5), the inverse map is the anchor plus S^-1 R(-rotation) applied to the output
// offset from the pivot, and a bilinear gather between two equal white taps is exactly white.
[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
squareTransformPlan(const LayerTransformValues values) {
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledSolid{
        kSolidNodeA,
        {kColorA, core::Color4d{1.0, 1.0, 1.0, 1.0}},
        {bloom::document::ParameterId::fromRaw(kSolidNodeA.value() * 100 + 1000), 4.0},
        {bloom::document::ParameterId::fromRaw(kSolidNodeA.value() * 100 + 1001), 4.0}});
    operations.emplace_back(layerOutput(kLayerNodeA, kLayerA, runtime::OperationIndex::fromRaw(0),
                                        kLayerParametersA, values));
    operations.emplace_back(runtime::CompiledMerge{
        kStackNode, {{kSlotA, kLayerA, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNode, runtime::OperationIndex::fromRaw(2)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{
            document::Revision::fromRaw(7), kProjectId, kCompositionId, format(4, 4),
            std::move(operations), runtime::OperationIndex::fromRaw(3)});
}

// The layer is centred when its position is the composition format centre, which for a 4x4 frame is
// (2, 2) -- that is what makes the authored translation exactly zero.
inline constexpr document::Vec2d kSquareCentre{2.0, 2.0};

// Renders one 4x4 frame as a row-major opacity mask, so a shape assertion reads as the shape.
[[nodiscard]] std::optional<std::array<float, 16>>
alphaMask(const runtime::EvaluationResult& result) {
    if (result.frame() == nullptr) {
        return std::nullopt;
    }
    std::array<float, 16> mask{};
    for (std::int64_t y = 0; y < 4; ++y) {
        for (std::int64_t x = 0; x < 4; ++x) {
            const auto read = result.frame()->processImage().read(x, y);
            if (!read) {
                return std::nullopt;
            }
            mask[static_cast<std::size_t>(y * 4 + x)] = read.value()->alpha();
        }
    }
    return mask;
}

void testLayerTransformShapesTheFrame(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    constexpr std::array<float, 16> kVerticalBar{0.0F, 1.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F,
                                                 0.0F, 1.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F};
    constexpr std::array<float, 16> kHorizontalBar{0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 1.0F,
                                                   1.0F, 1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    // Position pins centre(local bounds) + anchor, so an anchor at the layer's left edge holds
    // that edge at the composition centre and the narrowed layer falls to its right.
    // Native x in [0,4] maps to [1.75,3.75]: quarter and three-quarter edge coverage.
    constexpr std::array<float, 16> kAnchoredBar{
        0, 64.0F / 255.0F, 1, 191.0F / 255.0F, 0, 64.0F / 255.0F, 1, 191.0F / 255.0F,
        0, 64.0F / 255.0F, 1, 191.0F / 255.0F, 0, 64.0F / 255.0F, 1, 191.0F / 255.0F};
    constexpr std::array<float, 16> kEmpty{};

    // Half scale on one axis only: the layer narrows about its own centre to columns 1 and 2.
    const auto narrowed =
        squareTransformPlan({.position = kSquareCentre, .scale = document::Vec2d{0.5, 1.0}});
    const auto narrowedMask = alphaMask(evaluator.evaluate(narrowed, requestFor(*narrowed), {}));
    expectations.expect(narrowedMask.has_value() && *narrowedMask == kVerticalBar,
                        "a half scale on one axis narrows the layer about its own centre exactly");

    // The same narrowing plus a quarter turn: scale applies first, then rotation, so the vertical
    // bar becomes a horizontal one, and every sample coordinate stays exact.
    const auto turned = squareTransformPlan(
        {.position = kSquareCentre, .scale = document::Vec2d{0.5, 1.0}, .rotation = 90.0});
    const auto turnedMask = alphaMask(evaluator.evaluate(turned, requestFor(*turned), {}));
    expectations.expect(turnedMask.has_value() && *turnedMask == kHorizontalBar,
                        "rotation composes after scale, about the anchor, and stays exact at 90 "
                        "degrees");

    // Moving the anchor to the layer's left edge pins the narrowing there instead of at the centre.
    const auto anchored = squareTransformPlan({.position = kSquareCentre,
                                               .anchor = document::Vec2d{-1.5, 0.0},
                                               .scale = document::Vec2d{0.5, 1.0}});
    const auto anchoredMask = alphaMask(evaluator.evaluate(anchored, requestFor(*anchored), {}));
    expectations.expect(anchoredMask.has_value() && *anchoredMask == kAnchoredBar,
                        "the anchor, not the centre, is the point scale and rotation hold still");

    // A zero scale factor collapses the layer to no area: the Layer Output publishes no image at
    // all and the stack composites nothing, which is an entirely transparent frame rather than a
    // failure.
    const auto collapsed =
        squareTransformPlan({.position = kSquareCentre, .scale = document::Vec2d{0.0, 1.0}});
    const auto collapsedResult = evaluator.evaluate(collapsed, requestFor(*collapsed), {});
    const auto collapsedMask = alphaMask(collapsedResult);
    expectations.expect(collapsedResult.status() == runtime::EvaluationStatus::Evaluated &&
                            collapsedMask.has_value() && *collapsedMask == kEmpty,
                        "a zero scale factor is an empty layer, not an evaluation failure");

    // Carried entirely off the frame, the layer likewise publishes nothing.
    const auto gone = squareTransformPlan({.position = document::Vec2d{1000.0, 2.0}});
    const auto goneResult = evaluator.evaluate(gone, requestFor(*gone), {});
    const auto goneMask = alphaMask(goneResult);
    expectations.expect(goneResult.status() == runtime::EvaluationStatus::Evaluated &&
                            goneMask.has_value() && *goneMask == kEmpty,
                        "a layer carried off the composition contributes nothing");

    // A rotation is not confined to a turn: an authored 450 degrees must evaluate exactly where 90
    // does, and a rotation key is never clamped to any domain.
    const auto wound = squareTransformPlan(
        {.position = kSquareCentre, .scale = document::Vec2d{0.5, 1.0}, .rotation = 450.0});
    const auto woundMask = alphaMask(evaluator.evaluate(wound, requestFor(*wound), {}));
    expectations.expect(woundMask.has_value() && woundMask == turnedMask,
                        "an authored rotation past a full turn evaluates exactly as its reduced "
                        "angle");
}

void testEveryTransformParameterAnimates(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    constexpr std::array<float, 16> kFull{1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F,
                                          1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    constexpr std::array<float, 16> kVerticalBar{0.0F, 1.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F,
                                                 0.0F, 1.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F};
    constexpr std::array<float, 16> kHorizontalBar{0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 1.0F,
                                                   1.0F, 1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    // Native x in [0,4] maps to [1.75,3.75]: quarter and three-quarter edge coverage.
    constexpr std::array<float, 16> kAnchoredBar{
        0, 64.0F / 255.0F, 1, 191.0F / 255.0F, 0, 64.0F / 255.0F, 1, 191.0F / 255.0F,
        0, 64.0F / 255.0F, 1, 191.0F / 255.0F, 0, 64.0F / 255.0F, 1, 191.0F / 255.0F};

    // One plan per animated parameter, each curve running from the value that leaves the frame full
    // to the value that shapes it, so the frame at the first key, the midpoint, and the last key
    // are three distinct, independently predictable pictures.
    const auto maskAt = [&](const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
                            const std::int64_t numerator, const std::int64_t denominator) {
        auto request = requestFor(*plan);
        const auto time = core::RationalTime::create(numerator, denominator);
        if (!time.has_value()) {
            throw std::logic_error("animated transform fixture time must be valid");
        }
        request.time = *time;
        return alphaMask(evaluator.evaluate(plan, request, {}));
    };

    // Scale: (1, 1) at t = 0 to (0.5, 1) at t = 1. The midpoint is (0.75, 1), which inverts to
    // u = 1.5 + (4/3)(x - 1.5): columns 0 and 3 land 2 pixels outside the layer and read exactly
    // transparent, so the midpoint is already the narrowed bar.
    auto scaleDefinition = squareTransformPlan({.position = kSquareCentre})->copyDefinition();
    std::get<runtime::CompiledLayerOutput>(scaleDefinition.operations[1]).scale.source =
        runtime::Vec2CurveIndex::fromRaw(0);
    scaleDefinition.vec2Curves.push_back(
        vec2Curve(kScaleCurve,
                  {{document::KeyframeId::fromRaw(70), core::RationalTime::fromInteger(0), 1.0,
                    runtime::CompiledKeyframeInterpolation::Linear},
                   {document::KeyframeId::fromRaw(71), core::RationalTime::fromInteger(1), 0.5,
                    runtime::CompiledKeyframeInterpolation::Linear}},
                  {{document::KeyframeId::fromRaw(72), core::RationalTime::fromInteger(0), 1.0,
                    runtime::CompiledKeyframeInterpolation::Linear},
                   {document::KeyframeId::fromRaw(73), core::RationalTime::fromInteger(1), 1.0,
                    runtime::CompiledKeyframeInterpolation::Linear}}));
    const auto scalePlan = publishPlan(std::move(scaleDefinition));
    const auto scaleStart = maskAt(scalePlan, 0, 1);
    const auto scaleMiddle = maskAt(scalePlan, 1, 2);
    const auto scaleEnd = maskAt(scalePlan, 1, 1);
    expectations.expect(
        scaleStart.has_value() && *scaleStart == kFull && scaleEnd.has_value() &&
            *scaleEnd == kVerticalBar && scaleMiddle.has_value() && *scaleMiddle != kFull,
        "an animated scale is sampled per request and reshapes the frame over time");

    // Rotation: 0 at t = 0 to 90 at t = 1, with a constant one-axis narrowing, so the bar turns
    // from vertical to horizontal. The midpoint is 45 degrees, which is neither.
    auto rotationDefinition =
        squareTransformPlan({.position = kSquareCentre, .scale = document::Vec2d{0.5, 1.0}})
            ->copyDefinition();
    std::get<runtime::CompiledLayerOutput>(rotationDefinition.operations[1]).rotation.source =
        runtime::ScalarCurveIndex::fromRaw(0);
    rotationDefinition.scalarCurves.push_back(
        {kRotationCurve,
         {{document::KeyframeId::fromRaw(72), core::RationalTime::fromInteger(0), 0.0,
           runtime::CompiledKeyframeInterpolation::Linear},
          {document::KeyframeId::fromRaw(73), core::RationalTime::fromInteger(1), 90.0,
           runtime::CompiledKeyframeInterpolation::Linear}}});
    const auto rotationPlan = publishPlan(std::move(rotationDefinition));
    const auto rotationStart = maskAt(rotationPlan, 0, 1);
    const auto rotationMiddle = maskAt(rotationPlan, 1, 2);
    const auto rotationEnd = maskAt(rotationPlan, 1, 1);
    expectations.expect(
        rotationStart.has_value() && *rotationStart == kVerticalBar && rotationEnd.has_value() &&
            *rotationEnd == kHorizontalBar && rotationMiddle.has_value() &&
            *rotationMiddle != kVerticalBar && *rotationMiddle != kHorizontalBar,
        "an animated rotation is sampled per request and turns the layer over time");

    // Anchor: the centre at t = 0 to the layer's left edge at t = 1, with a constant one-axis
    // narrowing. Position pins centre(local bounds) + anchor, so the bar slides off the middle as
    // the pivot travels, without the authored position itself moving.
    auto anchorDefinition =
        squareTransformPlan({.position = kSquareCentre, .scale = document::Vec2d{0.5, 1.0}})
            ->copyDefinition();
    std::get<runtime::CompiledLayerOutput>(anchorDefinition.operations[1]).anchor.source =
        runtime::Vec2CurveIndex::fromRaw(0);
    anchorDefinition.vec2Curves.push_back(
        vec2Curve(kAnchorCurve,
                  {{document::KeyframeId::fromRaw(74), core::RationalTime::fromInteger(0), 0.0,
                    runtime::CompiledKeyframeInterpolation::Linear},
                   {document::KeyframeId::fromRaw(75), core::RationalTime::fromInteger(1), -1.5,
                    runtime::CompiledKeyframeInterpolation::Linear}},
                  {{document::KeyframeId::fromRaw(76), core::RationalTime::fromInteger(0), 0.0,
                    runtime::CompiledKeyframeInterpolation::Linear},
                   {document::KeyframeId::fromRaw(77), core::RationalTime::fromInteger(1), 0.0,
                    runtime::CompiledKeyframeInterpolation::Linear}}));
    const auto anchorPlan = publishPlan(std::move(anchorDefinition));
    const auto anchorStart = maskAt(anchorPlan, 0, 1);
    const auto anchorMiddle = maskAt(anchorPlan, 1, 2);
    const auto anchorEnd = maskAt(anchorPlan, 1, 1);
    expectations.expect(anchorStart.has_value() && *anchorStart == kVerticalBar &&
                            anchorEnd.has_value() && *anchorEnd == kAnchoredBar &&
                            anchorMiddle.has_value() && *anchorMiddle != kVerticalBar &&
                            *anchorMiddle != kAnchoredBar,
                        "an animated anchor is sampled per request and slides the pivot over time");

    // A rotation key is NOT confined to the unit interval the way an opacity key is: 360 degrees is
    // a perfectly valid key value, and the evaluator must accept it rather than reject it as
    // out-of-domain.
    auto wideDefinition = squareTransformPlan({.position = kSquareCentre})->copyDefinition();
    std::get<runtime::CompiledLayerOutput>(wideDefinition.operations[1]).rotation.source =
        runtime::ScalarCurveIndex::fromRaw(0);
    wideDefinition.scalarCurves.push_back(
        {kRotationCurve,
         {{document::KeyframeId::fromRaw(76), core::RationalTime::fromInteger(0), -720.0,
           runtime::CompiledKeyframeInterpolation::Linear},
          {document::KeyframeId::fromRaw(77), core::RationalTime::fromInteger(1), 1080.0,
           runtime::CompiledKeyframeInterpolation::Linear}}});
    const auto widePlan = publishPlan(std::move(wideDefinition));
    const auto wideResult = evaluator.evaluate(widePlan, requestFor(*widePlan), {});
    expectations.expect(wideResult.status() == runtime::EvaluationStatus::Evaluated,
                        "a rotation key outside the unit interval is accepted, unlike an opacity "
                        "key");
}

// Task S5, item 1: a SOURCE parameter animates too. A solid's colour is a typed operand now, so the
// evaluator samples it at the request time and the pixels follow -- which is the whole reason
// colour animation exists. Before this task a solid's colour was a resolved constant in the plan
// and no request time could change it.
void testAnimatedSolidColorChangesPixelsOverTime(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    auto definition = oneSolidPlan()->copyDefinition();
    std::get<runtime::CompiledSolid>(definition.operations.front()).color.source =
        runtime::Color4CurveIndex::fromRaw(0);
    // Black at t = 0, white at t = 1, with an EASED departure so the midpoint is not the linear
    // one.
    const auto easedChannel = [](const std::uint64_t first, const std::uint64_t second,
                                 const double from, const double to) {
        return std::vector<runtime::CompiledScalarKeyframe>{
            {document::KeyframeId::fromRaw(first), core::RationalTime::fromInteger(0), from,
             runtime::CompiledKeyframeInterpolation::EaseInOut},
            {document::KeyframeId::fromRaw(second), core::RationalTime::fromInteger(1), to,
             runtime::CompiledKeyframeInterpolation::Linear}};
    };
    runtime::CompiledColor4Curve colorCurve;
    colorCurve.id = kColorCurve;
    colorCurve.components[0] = easedChannel(200, 201, 0.0, 1.0);
    colorCurve.components[1] = easedChannel(202, 203, 0.0, 1.0);
    colorCurve.components[2] = easedChannel(204, 205, 0.0, 1.0);
    colorCurve.components[3] = easedChannel(206, 207, 1.0, 1.0);
    definition.color4Curves.push_back(std::move(colorCurve));
    const auto plan =
        std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));

    const auto channelAt = [&](const std::int64_t numerator, const std::int64_t denominator) {
        auto request = requestFor(*plan);
        const auto time = core::RationalTime::create(numerator, denominator);
        if (!time.has_value()) {
            throw std::logic_error("animated colour fixture time must be valid");
        }
        request.time = *time;
        const auto result = evaluator.evaluate(plan, request, {});
        render::Rgba32f storage = render::Rgba32f::transparent();
        const auto* sampled = pixel(result, 0, 0, storage);
        if (result.status() != runtime::EvaluationStatus::Evaluated || sampled == nullptr) {
            throw std::logic_error("animated colour fixture must evaluate");
        }
        return sampled->red();
    };

    const float atStart = channelAt(0, 1);
    const float atFirstThird = channelAt(1, 3);
    const float atMidpoint = channelAt(1, 2);
    const float atEnd = channelAt(1, 1);
    expectations.expect(atStart == 0.0F && atEnd == 1.0F,
                        "an animated solid reproduces its colour keys exactly at their own times");
    expectations.expect(atFirstThird > 0.0F && atFirstThird < atMidpoint && atMidpoint < atEnd,
                        "and the frames between them carry distinct, increasing colour values -- "
                        "the request time really reaches the pixels");
    // The eased factor at the exact first third is 7/27, applied to a 0 -> 1 ramp.
    expectations.expect(atFirstThird == static_cast<float>(7.0 / 27.0),
                        "an eased colour segment lands on the exact eased factor, not the linear "
                        "one");
    expectations.expect(atMidpoint == 0.5F,
                        "and the symmetric handles put the exact midpoint at exactly one half");
}

// Task S5, item 1: a text layer's SIZE animates, which changes how much of the frame the glyph
// covers -- an animated scalar on a source parameter, not a transform one.
void testAnimatedTextSizeChangesCoverage(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    auto definition = oneTextPlan()->copyDefinition();
    std::get<runtime::CompiledText>(definition.operations.front()).size.source =
        runtime::ScalarCurveIndex::fromRaw(definition.scalarCurves.size());
    definition.scalarCurves.push_back(
        {kTextSizeCurve,
         {{document::KeyframeId::fromRaw(210), core::RationalTime::fromInteger(0), 4.0,
           runtime::CompiledKeyframeInterpolation::Linear},
          {document::KeyframeId::fromRaw(211), core::RationalTime::fromInteger(1), 20.0,
           runtime::CompiledKeyframeInterpolation::Linear}}});
    const auto plan =
        std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));

    const auto coveredAt = [&](const std::int64_t numerator, const std::int64_t denominator) {
        auto request = requestFor(*plan);
        const auto time = core::RationalTime::create(numerator, denominator);
        if (!time.has_value()) {
            throw std::logic_error("animated text size fixture time must be valid");
        }
        request.time = *time;
        const auto result = evaluator.evaluate(plan, request, {});
        if (result.status() != runtime::EvaluationStatus::Evaluated || result.frame() == nullptr) {
            throw std::logic_error("animated text size fixture must evaluate");
        }
        const auto window = result.frame()->processImage().descriptor()->dataWindow();
        std::size_t covered = 0;
        for (std::int64_t y = window.originY(); y < window.originY() + window.extent().height();
             ++y) {
            for (std::int64_t x = window.originX(); x < window.originX() + window.extent().width();
                 ++x) {
                render::Rgba32f storage = render::Rgba32f::transparent();
                const auto* sampled = pixel(result, x, y, storage);
                covered += (sampled != nullptr && sampled->alpha() > 0.0F) ? 1U : 0U;
            }
        }
        return covered;
    };

    const auto small = coveredAt(0, 1);
    const auto large = coveredAt(1, 1);
    expectations.expect(
        small > 0 && large > small,
        "an animated text size covers strictly more of the frame at the larger key, "
        "so the size curve really reaches the rasterizer");
}

void testAnimatedParametersAreSampledOncePerRequest(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto plan = animatedLayerPlan();
    auto request = requestFor(*plan);
    const auto halfway = core::RationalTime::create(1, 2);
    if (!halfway.has_value()) {
        expectations.expect(false, "halfway animation time is representable");
        return;
    }
    request.time = *halfway;
    const auto result = evaluator.evaluate(plan, request, {});
    render::Rgba32f sample = render::Rgba32f::transparent();
    const auto* edge = pixel(result, 0, 0, sample);
    expectations.expect(result.status() == runtime::EvaluationStatus::Evaluated &&
                            edge != nullptr && near(edge->red(), (128.0F / 255.0F) * 0.5F) &&
                            near(edge->alpha(), (128.0F / 255.0F) * 0.5F),
                        "exact request time drives typed position and opacity curves");
    expectations.expect(result.frame() != nullptr &&
                            result.frame()->identity().animationSamplingSemanticsVersion ==
                                runtime::kAnimationSamplingSemanticsVersion,
                        "animation sampling semantics participate in the published cache identity");

    auto invalidIndexDefinition = plan->copyDefinition();
    std::get<runtime::CompiledLayerOutput>(invalidIndexDefinition.operations[1]).opacity.source =
        runtime::ScalarCurveIndex::fromRaw(1);
    const auto invalidIndex = publishPlan(std::move(invalidIndexDefinition));
    const auto invalid = evaluator.evaluate(invalidIndex, requestFor(*invalidIndex), {});
    expectations.expect(
        invalid.status() == runtime::EvaluationStatus::Failed && !invalid.diagnostics().empty() &&
            invalid.diagnostics().front().code == runtime::EvaluationDiagnosticCode::InvalidPlan &&
            invalid.diagnostics().front().subject.parameterId == kOpacityA,
        "out-of-range animation references fail preflight with typed parameter identity");

    auto duplicateIdDefinition = plan->copyDefinition();
    duplicateIdDefinition.vec2Curves.front().id = kOpacityCurve;
    const auto duplicateId = publishPlan(std::move(duplicateIdDefinition));
    const auto duplicate = evaluator.evaluate(duplicateId, requestFor(*duplicateId), {});
    expectations.expect(duplicate.status() == runtime::EvaluationStatus::Failed &&
                            !duplicate.diagnostics().empty() &&
                            duplicate.diagnostics().front().code ==
                                runtime::EvaluationDiagnosticCode::InvalidPlan,
                        "curve identities are globally unique across typed plan tables");

    auto duplicateKeyDefinition = plan->copyDefinition();
    duplicateKeyDefinition.vec2Curves.front().components[0].front().id =
        duplicateKeyDefinition.scalarCurves.front().keyframes.front().id;
    const auto duplicateKey = publishPlan(std::move(duplicateKeyDefinition));
    const auto duplicateKeyResult = evaluator.evaluate(duplicateKey, requestFor(*duplicateKey), {});
    expectations.expect(duplicateKeyResult.status() == runtime::EvaluationStatus::Failed &&
                            !duplicateKeyResult.diagnostics().empty() &&
                            duplicateKeyResult.diagnostics().front().subject.animationCurveId ==
                                kPositionCurve &&
                            duplicateKeyResult.diagnostics().front().subject.keyframeId ==
                                duplicateKey->vec2Curves().front().components[0].front().id,
                        "keyframe identities are validated globally before animation sampling");

    auto unusedCurveDefinition = plan->copyDefinition();
    unusedCurveDefinition.scalarCurves.push_back(
        {document::AnimationCurveId::fromRaw(52),
         {{document::KeyframeId::fromRaw(64), core::RationalTime::fromInteger(0), 0.5,
           runtime::CompiledKeyframeInterpolation::Linear}}});
    const auto unusedCurve = publishPlan(std::move(unusedCurveDefinition));
    const auto unused = evaluator.evaluate(unusedCurve, requestFor(*unusedCurve), {});
    expectations.expect(unused.status() == runtime::EvaluationStatus::Failed &&
                            unused.diagnostics().front().subject.animationCurveId ==
                                document::AnimationCurveId::fromRaw(52),
                        "compiled plans reject unreferenced curve-table entries");

    auto sharedCurveDefinition = twoSolidPlan()->copyDefinition();
    sharedCurveDefinition.scalarCurves.push_back(
        {kOpacityCurve,
         {{document::KeyframeId::fromRaw(60), core::RationalTime::fromInteger(0), 1.0,
           runtime::CompiledKeyframeInterpolation::Linear}}});
    std::get<runtime::CompiledLayerOutput>(sharedCurveDefinition.operations[1]).opacity.source =
        runtime::ScalarCurveIndex::fromRaw(0);
    std::get<runtime::CompiledLayerOutput>(sharedCurveDefinition.operations[3]).opacity.source =
        runtime::ScalarCurveIndex::fromRaw(0);
    const auto sharedCurve = publishPlan(std::move(sharedCurveDefinition));
    const auto shared = evaluator.evaluate(sharedCurve, requestFor(*sharedCurve), {});
    expectations.expect(shared.status() == runtime::EvaluationStatus::Failed &&
                            shared.diagnostics().front().subject.animationCurveId == kOpacityCurve,
                        "version-one plans reject curves shared by multiple parameters");

    auto duplicateParameterDefinition = plan->copyDefinition();
    auto& duplicateParameterLayer =
        std::get<runtime::CompiledLayerOutput>(duplicateParameterDefinition.operations[1]);
    duplicateParameterLayer.opacity.id = duplicateParameterLayer.position.id;
    const auto duplicateParameter = publishPlan(std::move(duplicateParameterDefinition));
    const auto duplicateParameterResult =
        evaluator.evaluate(duplicateParameter, requestFor(*duplicateParameter), {});
    expectations.expect(duplicateParameterResult.status() == runtime::EvaluationStatus::Failed &&
                            duplicateParameterResult.diagnostics().front().subject.parameterId ==
                                kPositionA,
                        "compiled plans preserve globally unique parameter identities");

    auto invalidOpacityDefinition = plan->copyDefinition();
    invalidOpacityDefinition.scalarCurves.front().keyframes.front().value = 1.5;
    const auto invalidOpacity = publishPlan(std::move(invalidOpacityDefinition));
    const auto invalidOpacityResult =
        evaluator.evaluate(invalidOpacity, requestFor(*invalidOpacity), {});
    expectations.expect(
        invalidOpacityResult.status() == runtime::EvaluationStatus::Failed &&
            invalidOpacityResult.diagnostics().front().subject.parameterId == kOpacityA &&
            invalidOpacityResult.diagnostics().front().subject.animationCurveId == kOpacityCurve &&
            invalidOpacityResult.diagnostics().front().subject.keyframeId ==
                invalidOpacity->scalarCurves().front().keyframes.front().id,
        "animated opacity domain failures retain parameter, curve, and key identity");
}

void testClippingAndOpacityEndpoints(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto shifted = oneSolidPlan({1.0, 0.0, 0.0, 1.0}, {3.0, 1.0});
    const auto shiftedResult = evaluator.evaluate(shifted, requestFor(*shifted), {});
    render::Rgba32f sample = render::Rgba32f::transparent();
    const auto* clipped = pixel(shiftedResult, 0, 0, sample);
    expectations.expect(clipped != nullptr && *clipped == render::Rgba32f::transparent(),
                        "integer translation clips outside source coverage to exact transparent");
    const auto* covered = pixel(shiftedResult, 1, 0, sample);
    expectations.expect(covered != nullptr && covered->red() == 1.0F && covered->alpha() == 1.0F,
                        "integer translation uses the frozen center-coordinate displacement");

    const auto invisible = oneSolidPlan({3.0, -2.0, 8.0, 1.0}, {2.0, 1.0}, 0.0);
    const auto invisibleResult = evaluator.evaluate(invisible, requestFor(*invisible), {});
    const auto* invisiblePixel = pixel(invisibleResult, 2, 1, sample);
    expectations.expect(invisiblePixel != nullptr &&
                            *invisiblePixel == render::Rgba32f::transparent(),
                        "opacity zero canonicalizes all premultiplied components to transparent");

    const auto transparentHdr = oneSolidPlan(
        {std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(), 4.0, 0.0});
    const auto transparentResult =
        evaluator.evaluate(transparentHdr, requestFor(*transparentHdr), {});
    const auto* transparentPixel = pixel(transparentResult, 0, 0, sample);
    expectations.expect(transparentPixel != nullptr &&
                            *transparentPixel == render::Rgba32f::transparent(),
                        "alpha-zero authoring color canonicalizes before invalid hidden RGB leaks");
}

void testStackOrderingOpacityAndDisplay(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto redTop = twoSolidPlan(true);
    const auto blueTop = twoSolidPlan(false);
    const auto redResult = evaluator.evaluate(redTop, requestFor(*redTop), {});
    const auto blueResult = evaluator.evaluate(blueTop, requestFor(*blueTop), {});

    render::Rgba32f redSample = render::Rgba32f::transparent();
    render::Rgba32f blueSample = render::Rgba32f::transparent();
    const auto* redPixel = pixel(redResult, 1, 1, redSample);
    const auto* bluePixel = pixel(blueResult, 1, 1, blueSample);
    expectations.expect(redPixel != nullptr && near(redPixel->red(), 0.5F) &&
                            near(redPixel->blue(), 0.5F) && redPixel->alpha() == 1.0F,
                        "first stack entry is topmost and folds source-over bottom to top");
    expectations.expect(bluePixel != nullptr && bluePixel->red() == 0.0F &&
                            bluePixel->blue() == 1.0F && bluePixel->alpha() == 1.0F &&
                            *redPixel != *bluePixel,
                        "reordering translucent layers is observably non-commutative");

    const auto halfOpacity = oneSolidPlan({1.0, 0.0, 0.0, 1.0}, {2.0, 1.0}, 0.5);
    const auto halfResult = evaluator.evaluate(halfOpacity, requestFor(*halfOpacity), {});
    render::Rgba32f halfSample = render::Rgba32f::transparent();
    const auto* halfPixel = pixel(halfResult, 0, 0, halfSample);
    expectations.expect(halfPixel != nullptr && halfPixel->red() == 0.5F &&
                            halfPixel->alpha() == 0.5F,
                        "layer opacity multiplies all premultiplied components");
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    const auto halfDisplay = displayPreparer.prepare(halfResult.frame(), displayRequest(), {});
    if (halfDisplay.frame() != nullptr) {
        const auto displayPixels = halfDisplay.frame()->buffer().pixels();
        expectations.expect(!displayPixels.empty() && displayPixels.front().red == 255 &&
                                displayPixels.front().green == 0 &&
                                displayPixels.front().blue == 0 &&
                                displayPixels.front().alpha == 128,
                            "worker display mapping publishes straight packed reference sRGB");
    }
}

// The Merge stage reads each entry's OWN Layer Output blend mode. The expected pixels are the same
// independently derived goldens src/render/tests/cpu_image_primitives_test.cpp pins for this exact
// premultiplied pair, so this case proves the wiring -- that the mode reaches the fold, per layer
// -- rather than re-proving the arithmetic.
void testStackCompositesEachLayerUnderItsOwnBlendMode(Expectations& expectations) {
    using core::BlendMode;
    struct Case final {
        BlendMode mode;
        render::Rgba32f expected;
    };
    const auto premultiplied = [](const float red, const float green, const float blue,
                                  const float alpha) {
        const auto value = render::Rgba32f::fromPremultiplied(red, green, blue, alpha);
        if (!value) {
            throw std::logic_error("blend golden fixture must be a valid process pixel");
        }
        return *value.value();
    };
    const std::array<Case, 8> cases{{
        {BlendMode::Normal, premultiplied(0.625F, 0.5F, 0.5F, 1.0F)},
        {BlendMode::Add, premultiplied(0.75F, 0.75F, 0.875F, 1.0F)},
        {BlendMode::Multiply, premultiplied(0.25F, 0.375F, 0.46875F, 1.0F)},
        {BlendMode::Screen, premultiplied(0.625F, 0.625F, 0.78125F, 1.0F)},
        {BlendMode::Overlay, premultiplied(0.375F, 0.5F, 0.6875F, 1.0F)},
        {BlendMode::Darken, premultiplied(0.25F, 0.5F, 0.5F, 1.0F)},
        {BlendMode::Lighten, premultiplied(0.625F, 0.5F, 0.75F, 1.0F)},
        {BlendMode::Difference, premultiplied(0.5F, 0.25F, 0.625F, 1.0F)},
    }};
    expectations.expect(cases.size() == core::kBlendModes.size(),
                        "the Merge stage is exercised under every implemented blend mode");

    const runtime::CpuCompositionEvaluator evaluator;
    for (const auto& testCase : cases) {
        const auto plan = twoSolidBlendPlan(testCase.mode, BlendMode::Normal);
        const auto result = evaluator.evaluate(plan, requestFor(*plan), {});
        render::Rgba32f storage = render::Rgba32f::transparent();
        const auto* composited = pixel(result, 1, 1, storage);
        expectations.expect(
            result.status() == runtime::EvaluationStatus::Evaluated && composited != nullptr &&
                *composited == testCase.expected,
            "the Merge stage folds the top layer under its own blend mode, exactly");
    }

    // The mode is read per ENTRY, from the Layer Output that entry names -- not once for the stack
    // and not off the first entry. A mode on the BOTTOM layer has nothing beneath it to combine
    // with, so it must leave the frame bit-identical to an all-Normal stack, while the same mode on
    // the top layer must change it.
    const auto allNormal = twoSolidBlendPlan(BlendMode::Normal, BlendMode::Normal);
    const auto bottomDifference = twoSolidBlendPlan(BlendMode::Normal, BlendMode::Difference);
    const auto topDifference = twoSolidBlendPlan(BlendMode::Difference, BlendMode::Normal);
    render::Rgba32f normalStorage = render::Rgba32f::transparent();
    render::Rgba32f bottomStorage = render::Rgba32f::transparent();
    render::Rgba32f topStorage = render::Rgba32f::transparent();
    const auto* normalPixel =
        pixel(evaluator.evaluate(allNormal, requestFor(*allNormal), {}), 1, 1, normalStorage);
    const auto* bottomPixel =
        pixel(evaluator.evaluate(bottomDifference, requestFor(*bottomDifference), {}), 1, 1,
              bottomStorage);
    const auto* topPixel =
        pixel(evaluator.evaluate(topDifference, requestFor(*topDifference), {}), 1, 1, topStorage);
    expectations.expect(normalPixel != nullptr && bottomPixel != nullptr && topPixel != nullptr &&
                            *bottomPixel == *normalPixel && *topPixel != *normalPixel,
                        "each stack entry contributes its own layer's blend mode");
}

void testLayerRangeIsHalfOpen(Expectations& expectations) {
    auto definition = oneSolidPlan()->copyDefinition();
    auto& layer = std::get<runtime::CompiledLayerOutput>(definition.operations[1]);
    layer.inPoint = core::RationalTime::fromInteger(1);
    layer.outPoint = core::RationalTime::fromInteger(2);
    const auto plan = publishPlan(std::move(definition));
    const runtime::CpuCompositionEvaluator evaluator;
    for (const auto second : {0, 1, 2}) {
        auto request = requestFor(*plan);
        request.time = core::RationalTime::fromInteger(second);
        const auto result = evaluator.evaluate(plan, request, {});
        expectations.expect(
            result.frame() &&
                std::ranges::all_of(result.frame()->processImage().pixels(),
                                    [second](const auto& value) {
                                        return second == 1
                                                   ? value != render::Rgba32f::transparent()
                                                   : value == render::Rgba32f::transparent();
                                    }),
            "one reused plan omits a layer before in and at out, and includes it at in");
    }
}

void testEmptyStackIsTransparent(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto plan = emptyStackPlan();
    const auto result = evaluator.evaluate(plan, requestFor(*plan), {});
    const bool processTransparent =
        result.frame() != nullptr &&
        std::ranges::all_of(result.frame()->processImage().pixels(), [](const auto& value) {
            return value == render::Rgba32f::transparent();
        });
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    const auto display = displayPreparer.prepare(result.frame(), displayRequest(), {});
    const bool displayTransparent =
        display.frame() != nullptr &&
        std::ranges::all_of(display.frame()->buffer().pixels(), [](const auto& value) {
            return value.red == 0 && value.green == 0 && value.blue == 0 && value.alpha == 0;
        });
    expectations.expect(
        result.status() == runtime::EvaluationStatus::Evaluated && processTransparent &&
            displayTransparent,
        "an empty layer stack publishes exact transparent process and display pixels");
}

void testProxyAndPeakBudget(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto plan = oneSolidPlan();
    const auto proxyExtentResult = render::ImageExtent::create(2, 2);
    if (!proxyExtentResult) {
        expectations.expect(false, "proxy extent fixture succeeds");
        return;
    }
    const auto proxyRequest =
        requestFor(*plan, 1U << 20U, runtime::ProxyResolution{*proxyExtentResult.value()});
    const auto proxy = evaluator.evaluate(plan, proxyRequest, {});
    const auto* descriptor =
        proxy.frame() == nullptr ? nullptr : proxy.frame()->processImage().descriptor();
    const auto expectedProxyPixelAspect = core::PixelAspectRatio::create(2, 1);
    if (!expectedProxyPixelAspect.has_value()) {
        expectations.expect(false, "proxy pixel-aspect fixture succeeds");
        return;
    }
    expectations.expect(descriptor != nullptr && descriptor->dataWindow().extent().width() == 2 &&
                            descriptor->dataWindow().extent().height() == 2 &&
                            descriptor->pixelAspect() == expectedProxyPixelAspect.value(),
                        "proxy extent derives pixel aspect that preserves display aspect");

    const auto proxyShiftedPlan = oneSolidPlan({1.0, 0.0, 0.0, 1.0}, {3.0, 1.0});
    const auto proxyShifted =
        evaluator.evaluate(proxyShiftedPlan,
                           requestFor(*proxyShiftedPlan, 1U << 20U,
                                      runtime::ProxyResolution{*proxyExtentResult.value()}),
                           {});
    render::Rgba32f proxyEdge = render::Rgba32f::transparent();
    const auto* proxyEdgePixel = pixel(proxyShifted, 0, 0, proxyEdge);
    expectations.expect(proxyEdgePixel != nullptr && near(proxyEdgePixel->red(), 128.0F / 255.0F) &&
                            near(proxyEdgePixel->alpha(), 128.0F / 255.0F),
                        "absolute authoring displacement is scaled independently for a proxy");

    // A 4x2 layer's image is its transformed bounds grown by one pixel on every side for the
    // bilinear support -- 6x4 RGBA32F, 384 bytes. This plan peaks at the layer image and the stack
    // image it composites into, two resident 6x4 images (768 bytes).
    const auto below = evaluator.evaluate(plan, requestFor(*plan, 767), {});
    const auto exact = evaluator.evaluate(plan, requestFor(*plan, 768), {});
    expectations.expect(below.status() == runtime::EvaluationStatus::Failed &&
                            !below.diagnostics().empty() &&
                            below.diagnostics().front().code ==
                                runtime::EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                        "preflight rejects one byte below the exact live-image peak");
    expectations.expect(exact.status() == runtime::EvaluationStatus::Evaluated,
                        "exact live-image peak budget succeeds");

    const auto twoLayers = twoSolidPlan();
    const auto twoBelow = evaluator.evaluate(twoLayers, requestFor(*twoLayers, 1151), {});
    const auto twoExact = evaluator.evaluate(twoLayers, requestFor(*twoLayers, 1152), {});
    expectations.expect(twoBelow.status() == runtime::EvaluationStatus::Failed &&
                            twoExact.status() == runtime::EvaluationStatus::Evaluated,
                        "peak simulation accounts for both live layers while stacking");

    const auto emptyPlan = emptyStackPlan();
    const auto processOnlyBelow = evaluator.evaluate(emptyPlan, requestFor(*emptyPlan, 127), {});
    const auto processOnlyExact = evaluator.evaluate(emptyPlan, requestFor(*emptyPlan, 128), {});
    expectations.expect(processOnlyBelow.status() == runtime::EvaluationStatus::Failed &&
                            processOnlyExact.status() == runtime::EvaluationStatus::Evaluated,
                        "process preflight excludes the later prepared-display allocation");

    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    const auto displayBelow =
        displayPreparer.prepare(processOnlyExact.frame(), displayRequest(159), {});
    const auto displayExact =
        displayPreparer.prepare(processOnlyExact.frame(), displayRequest(160), {});
    expectations.expect(
        displayBelow.status() == runtime::ReferenceDisplayPreparationStatus::Failed &&
            !displayBelow.diagnostics().empty() &&
            displayBelow.diagnostics().front().code ==
                runtime::ReferenceDisplayDiagnosticCode::PixelStorageBudgetExceeded,
        "display preflight counts retained process bytes plus its pending allocation");
    expectations.expect(
        displayExact.status() == runtime::ReferenceDisplayPreparationStatus::Prepared,
        "display preparation accepts the exact aggregate process-plus-display budget");
}

void testIdentityAndPreparedHandoff(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto firstPlan = oneSolidPlan();
    const auto equivalentPlan = publishPlan(firstPlan->copyDefinition());
    const auto divergentPlan = oneSolidPlan({0.0, 1.0, 0.0, 1.0});
    const auto first = evaluator.evaluate(firstPlan, requestFor(*firstPlan), {});
    const auto equivalent = evaluator.evaluate(equivalentPlan, requestFor(*equivalentPlan), {});
    const auto divergent = evaluator.evaluate(divergentPlan, requestFor(*divergentPlan), {});
    expectations.expect(first.frame() != nullptr && equivalent.frame() != nullptr &&
                            first.frame()->identity() == equivalent.frame()->identity(),
                        "cache identity uses exact plan value, not allocation address");
    expectations.expect(first.frame() != nullptr && divergent.frame() != nullptr &&
                            first.frame()->identity() != divergent.frame()->identity(),
                        "same project and revision with different pixels has a different identity");

    auto laterTimeRequest = requestFor(*firstPlan);
    laterTimeRequest.time = core::RationalTime::fromInteger(1);
    const auto laterTime = evaluator.evaluate(firstPlan, laterTimeRequest, {});
    expectations.expect(first.frame() != nullptr && laterTime.frame() != nullptr &&
                            first.frame()->identity() != laterTime.frame()->identity(),
                        "exact rational time remains in conservative cache identity");

    const auto largerBudget = evaluator.evaluate(firstPlan, requestFor(*firstPlan, 1U << 21U), {});
    expectations.expect(first.frame() != nullptr && largerBudget.frame() != nullptr &&
                            first.frame()->identity() == largerBudget.frame()->identity(),
                        "execution memory budget is deliberately excluded from pixel identity");

    const auto identityProxyExtent = render::ImageExtent::create(2, 1);
    if (first.frame() != nullptr && identityProxyExtent) {
        auto changedResolution = first.frame()->identity();
        changedResolution.resolution = runtime::ProxyResolution{*identityProxyExtent.value()};
        auto changedSemantics = first.frame()->identity();
        ++changedSemantics.imagePrimitiveSemanticsVersion;
        expectations.expect(first.frame()->identity() != changedResolution &&
                                first.frame()->identity() != changedSemantics,
                            "resolution and primitive semantics participate in cache identity");
    }

    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    const auto firstDisplay = displayPreparer.prepare(first.frame(), displayRequest(), {});
    const auto largerDisplayBudget =
        displayPreparer.prepare(first.frame(), displayRequest(1U << 21U), {});
    expectations.expect(
        firstDisplay.frame() != nullptr && largerDisplayBudget.frame() != nullptr &&
            firstDisplay.frame()->identity() == largerDisplayBudget.frame()->identity() &&
            firstDisplay.frame()->identity().processFrame == first.frame()->identity(),
        "display identity starts from the exact process identity and excludes execution budget");
    if (firstDisplay.frame() != nullptr) {
        auto changedDisplaySemantics = firstDisplay.frame()->identity();
        ++changedDisplaySemantics.mapperSemanticsVersion;
        expectations.expect(changedDisplaySemantics != firstDisplay.frame()->identity() &&
                                changedDisplaySemantics.processFrame == first.frame()->identity(),
                            "display pipeline semantics change only the distinct display identity");
    }

    const auto preparedOne = runtime::PreparedPreviewFrame::create(1, firstDisplay.frame());
    const auto preparedTwo = runtime::PreparedPreviewFrame::create(2, firstDisplay.frame());
    expectations.expect(preparedOne.has_value() && preparedTwo.has_value() &&
                            preparedOne->processIdentity() == preparedTwo->processIdentity() &&
                            preparedOne->displayIdentity() == preparedTwo->displayIdentity() &&
                            preparedOne->desiredIdentity() != preparedTwo->desiredIdentity(),
                        "publication generation changes desired identity but not frame identities");
    expectations.expect(!runtime::PreparedPreviewFrame::create(0, firstDisplay.frame()).has_value(),
                        "zero is not a publishable preview generation");
    const auto sharedPrepared =
        preparedOne.has_value()
            ? std::make_shared<const runtime::PreparedPreviewFrame>(*preparedOne)
            : std::shared_ptr<const runtime::PreparedPreviewFrame>{};
    const auto preparedResult = runtime::PreviewPreparationResult::prepared(sharedPrepared);
    expectations.expect(preparedResult.has_value() &&
                            preparedResult->status() ==
                                runtime::PreviewPreparationStatus::Prepared &&
                            preparedResult->frame() != nullptr &&
                            !runtime::PreviewPreparationResult::prepared({}).has_value() &&
                            runtime::PreviewPreparationResult::unsupported().status() ==
                                runtime::PreviewPreparationStatus::Unsupported,
                        "typed preview result cannot represent Prepared without a frame");
}

void testPublishedPlanOwnsItsImmutableDefinition(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    auto retainedDefinition = oneSolidPlan()->copyDefinition();
    const auto publishedPlan =
        std::make_shared<const runtime::CompiledCompositionPlan>(retainedDefinition);
    const auto publishedDefinition = publishedPlan->copyDefinition();

    const auto before = evaluator.evaluate(publishedPlan, requestFor(*publishedPlan), {});
    retainedDefinition.sourceRevision = document::Revision::fromRaw(99);
    retainedDefinition.output = runtime::OperationIndex::fromRaw(0);
    std::get<runtime::CompiledSolid>(retainedDefinition.operations.front()).color.source =
        core::Color4d{0.0, 1.0, 0.0, 1.0};
    retainedDefinition.operations.clear();
    retainedDefinition.scalarCurves.push_back(
        {kOpacityCurve,
         {{document::KeyframeId::fromRaw(90), core::RationalTime::fromInteger(0), 0.0,
           runtime::CompiledKeyframeInterpolation::Hold}}});
    ++retainedDefinition.planSemanticsVersion;

    const auto after = evaluator.evaluate(publishedPlan, requestFor(*publishedPlan), {});
    render::Rgba32f beforePixel = render::Rgba32f::transparent();
    render::Rgba32f afterPixel = render::Rgba32f::transparent();
    const auto* beforeSample = pixel(before, 0, 0, beforePixel);
    const auto* afterSample = pixel(after, 0, 0, afterPixel);
    expectations.expect(
        publishedPlan->copyDefinition() == publishedDefinition,
        "retained mutable construction storage cannot change a published compiled plan");
    expectations.expect(
        before.frame() != nullptr && after.frame() != nullptr &&
            before.frame()->identity() == after.frame()->identity() && beforeSample != nullptr &&
            afterSample != nullptr && *beforeSample == *afterSample,
        "definition mutation cannot change process-frame identity or evaluated digest inputs");
    expectations.expect(before.frame() != nullptr && after.frame() != nullptr &&
                            before.frame()->identity().plan.get() == publishedPlan.get() &&
                            after.frame()->identity().plan.get() == publishedPlan.get(),
                        "evaluated frames retain the immutable plan without a per-frame deep copy");
}

void testStructuredFailuresAndProgress(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto plan = oneSolidPlan();
    auto invalidRequest = requestFor(*plan);
    invalidRequest.quality =
        std::bit_cast<runtime::EvaluationQuality>(static_cast<std::uint8_t>(99));
    const auto unsupported = evaluator.evaluate(plan, invalidRequest, {});
    expectations.expect(unsupported.status() == runtime::EvaluationStatus::Failed &&
                            unsupported.diagnostics().front().code ==
                                runtime::EvaluationDiagnosticCode::InvalidRequest,
                        "unknown evaluation quality fails with a structured diagnostic");

    auto invalidDefinition = plan->copyDefinition();
    std::get<runtime::CompiledLayerOutput>(invalidDefinition.operations[1]).input =
        runtime::OperationIndex::fromRaw(99);
    const auto invalidPlan = publishPlan(std::move(invalidDefinition));
    const auto invalid = evaluator.evaluate(invalidPlan, requestFor(*invalidPlan), {});
    expectations.expect(invalid.status() == runtime::EvaluationStatus::Failed &&
                            invalid.diagnostics().front().code ==
                                runtime::EvaluationDiagnosticCode::InvalidPlan,
                        "out-of-range plan references fail without unsafe indexing");

    auto incompatibleDefinition = plan->copyDefinition();
    ++incompatibleDefinition.animationSamplingSemanticsVersion;
    const auto incompatiblePlan = publishPlan(std::move(incompatibleDefinition));
    const auto incompatible =
        evaluator.evaluate(incompatiblePlan, requestFor(*incompatiblePlan), {});
    expectations.expect(incompatible.status() == runtime::EvaluationStatus::Failed &&
                            incompatible.diagnostics().front().code ==
                                runtime::EvaluationDiagnosticCode::InvalidPlan,
                        "incompatible animation plan semantics require recompilation");

    std::vector<runtime::EvaluationProgress> progress;
    const auto result = evaluator.evaluate(
        plan, requestFor(*plan), {},
        [&progress](const runtime::EvaluationProgress& update) { progress.push_back(update); });
    bool monotonic = result.status() == runtime::EvaluationStatus::Evaluated && !progress.empty();
    for (std::size_t index = 1; index < progress.size(); ++index) {
        if (progress[index].stage == progress[index - 1].stage &&
            progress[index].operation == progress[index - 1].operation) {
            monotonic = monotonic && progress[index].completed >= progress[index - 1].completed;
        }
    }
    expectations.expect(monotonic, "process preflight and operation progress is monotonic");

    const auto throwingProgress =
        evaluator.evaluate(plan, requestFor(*plan), {}, [](const runtime::EvaluationProgress&) {
            throw std::runtime_error("monitor failed");
        });
    expectations.expect(throwingProgress.status() == runtime::EvaluationStatus::Evaluated,
                        "best-effort progress observers cannot change pixel evaluation outcome");

    const auto hostileExtent = render::ImageExtent::create(
        std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max());
    if (hostileExtent) {
        const auto overflow =
            evaluator.evaluate(plan,
                               requestFor(*plan, std::numeric_limits<std::size_t>::max(),
                                          runtime::ProxyResolution{*hostileExtent.value()}),
                               {});
        expectations.expect(overflow.status() == runtime::EvaluationStatus::Failed &&
                                !overflow.diagnostics().empty() &&
                                overflow.diagnostics().front().code ==
                                    runtime::EvaluationDiagnosticCode::ArithmeticOverflow,
                            "hostile proxy storage overflow is a structured failure");
    }

    const auto overflowingColor = oneSolidPlan({std::numeric_limits<double>::max(), 0.0, 0.0, 1.0});
    const auto numericFailure =
        evaluator.evaluate(overflowingColor, requestFor(*overflowingColor), {});
    expectations.expect(numericFailure.status() == runtime::EvaluationStatus::Failed &&
                            !numericFailure.diagnostics().empty() &&
                            numericFailure.diagnostics().front().code ==
                                runtime::EvaluationDiagnosticCode::InvalidPixel,
                        "authoring-to-process range failure identifies the responsible pixel");
}

void testRepeatability(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto plan = twoSolidPlan();
    const auto first = evaluator.evaluate(plan, requestFor(*plan), {});
    const auto second = evaluator.evaluate(plan, requestFor(*plan), {});
    const bool processEqual = first.frame() != nullptr && second.frame() != nullptr &&
                              std::ranges::equal(first.frame()->processImage().pixels(),
                                                 second.frame()->processImage().pixels());
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    const auto firstDisplay = displayPreparer.prepare(first.frame(), displayRequest(), {});
    const auto secondDisplay = displayPreparer.prepare(second.frame(), displayRequest(), {});
    const bool displayEqual = firstDisplay.frame() != nullptr && secondDisplay.frame() != nullptr &&
                              std::ranges::equal(firstDisplay.frame()->buffer().pixels(),
                                                 secondDisplay.frame()->buffer().pixels());
    expectations.expect(processEqual && displayEqual &&
                            first.frame()->identity() == second.frame()->identity(),
                        "process evaluation and display preparation are independently repeatable");
}

// ---------------------------------------------------------------------------------------------
// Row bands (task PERF1).
//
// A plan tall enough to divide into many bands and varied enough that every banded kernel runs over
// it: a full-frame solid, a rotated and scaled text layer above it, and a Layer Stack that folds
// both. The text layer's transform is the one the task package names -- rotation 15 degrees, scale
// 0.8 -- so the resample interpolates on every row rather than taking the translate-only path.
[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan> bandedPlan() {
    const auto bandedFormat = format(160, 120);
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledSolid{
        kSolidNodeA,
        {kColorA, core::Color4d{0.2, 0.35, 0.6, 1.0}},
        {bloom::document::ParameterId::fromRaw(kSolidNodeA.value() * 100 + 1000), 160.0},
        {bloom::document::ParameterId::fromRaw(kSolidNodeA.value() * 100 + 1001), 120.0}});
    operations.emplace_back(layerOutput(kLayerNodeA, kLayerA, runtime::OperationIndex::fromRaw(0),
                                        kLayerParametersA,
                                        {.position = {80.0, 60.0}, .opacity = 1.0}));
    operations.emplace_back(runtime::CompiledText{kTextNode,
                                                  kTextContent,
                                                  std::string("Bloom RAM preview"),
                                                  {kTextSize, 24.0},
                                                  {kTextColor, core::Color4d{1.0, 0.9, 0.8, 1.0}},
                                                  {{document::ParameterId::fromRaw(901)},
                                                   0,
                                                   {document::ParameterId::fromRaw(902), 1.0},
                                                   {document::ParameterId::fromRaw(903), 0.0}}});
    operations.emplace_back(layerOutput(
        kLayerNodeB, kLayerB, runtime::OperationIndex::fromRaw(2), kLayerParametersB,
        {.position = {80.0, 60.0}, .scale = {0.8, 0.8}, .rotation = 15.0, .opacity = 0.75}));
    operations.emplace_back(
        runtime::CompiledMerge{kStackNode,
                               {{kSlotB, kLayerB, runtime::OperationIndex::fromRaw(3)},
                                {kSlotA, kLayerA, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNode, runtime::OperationIndex::fromRaw(4)});
    return publishPlan(runtime::CompiledCompositionPlanDefinition{
        document::Revision::fromRaw(7), kProjectId, kCompositionId, bandedFormat,
        std::move(operations), runtime::OperationIndex::fromRaw(5)});
}

void testRowBandPlanIsDeterministicAndBounded(Expectations& expectations) {
    expectations.expect(runtime::planRowBands(0, 8).empty(), "no rows plans no bands");
    expectations.expect(runtime::planRowBands(1080, 0).empty(), "no band budget plans no bands");

    const auto single = runtime::planRowBands(runtime::kMinimumRowsPerBand - 1, 64);
    expectations.expect(single.size() == 1 && single.front().beginRow == 0 &&
                            single.front().rowCount == runtime::kMinimumRowsPerBand - 1,
                        "an image shorter than one band's floor is exactly one band");

    const auto bands = runtime::planRowBands(1080, 6);
    std::uint32_t covered = 0;
    bool contiguous = true;
    bool aboveFloor = true;
    for (const auto band : bands) {
        contiguous = contiguous && band.beginRow == covered;
        aboveFloor = aboveFloor && band.rowCount >= runtime::kMinimumRowsPerBand;
        covered += band.rowCount;
    }
    expectations.expect(bands.size() == 6 && contiguous && aboveFloor && covered == 1080,
                        "1080 rows over six bands partition the image exactly, in order");
    expectations.expect(bands == runtime::planRowBands(1080, 6),
                        "the same row count and band budget always plan the same split");

    const auto narrow = runtime::planRowBands(20, 64);
    expectations.expect(narrow.size() == 2 && narrow[0].rowCount == 10 && narrow[1].rowCount == 10,
                        "the band budget never splits an image below the rows-per-band floor");
    const auto remainder = runtime::planRowBands(25, 3);
    expectations.expect(remainder.size() == 3 && remainder[0].rowCount == 9 &&
                            remainder[1].rowCount == 8 && remainder[2].rowCount == 8,
                        "a remainder is spread one row at a time across the leading bands");
}

void testParallelRowBandsAreBitIdenticalToSerial(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto plan = bandedPlan();
    auto request = requestFor(*plan, 1U << 24U);
    request.bypassOperationCache = true;

    const auto serial = evaluator.evaluate(plan, request, {}, {}, nullptr);
    runtime::CpuRowBandExecutor executor(5);
    const auto parallel = evaluator.evaluate(plan, request, {}, {}, &executor);
    expectations.expect(executor.bandLimit() == 6,
                        "an explicitly sized pool reports its workers plus the calling thread");
    expectations.expect(serial.status() == runtime::EvaluationStatus::Evaluated &&
                            parallel.status() == runtime::EvaluationStatus::Evaluated &&
                            serial.frame() != nullptr && parallel.frame() != nullptr,
                        "the banded fixture evaluates both serially and in parallel");
    if (serial.frame() == nullptr || parallel.frame() == nullptr) {
        return;
    }

    const auto serialPixels = serial.frame()->processImage().pixels();
    const auto parallelPixels = parallel.frame()->processImage().pixels();
    const bool sameStorage = serialPixels.size() == parallelPixels.size();
    const bool bitIdentical =
        sameStorage && serialPixels.size_bytes() > 0 &&
        std::memcmp(serialPixels.data(), parallelPixels.data(), serialPixels.size_bytes()) == 0;
    expectations.expect(bitIdentical,
                        "row-parallel evaluation produces byte-for-byte the serial process frame");
    expectations.expect(serial.frame()->identity() == parallel.frame()->identity(),
                        "the row-band pool is not part of frame identity");

    // The band split must not change the published picture at any band count either: a pool one
    // worker wide, and one wider than the image has bands, both have to land on the same bytes.
    runtime::CpuRowBandExecutor narrow(1);
    const auto narrowResult = evaluator.evaluate(plan, request, {}, {}, &narrow);
    runtime::CpuRowBandExecutor wide(32);
    const auto wideResult = evaluator.evaluate(plan, request, {}, {}, &wide);
    const bool everyWidthAgrees =
        narrowResult.frame() != nullptr && wideResult.frame() != nullptr &&
        std::memcmp(serialPixels.data(), narrowResult.frame()->processImage().pixels().data(),
                    serialPixels.size_bytes()) == 0 &&
        std::memcmp(serialPixels.data(), wideResult.frame()->processImage().pixels().data(),
                    serialPixels.size_bytes()) == 0;
    expectations.expect(everyWidthAgrees, "every band width publishes the same pixels");

    request.bypassOperationCache = false;
    const auto coldBanded = evaluator.evaluate(plan, request, {}, {}, &executor);
    const auto cached = evaluator.evaluate(plan, request, {});
    expectations.expect(coldBanded.frame() && cached.frame(), "banded cache fixture evaluates");
    if (coldBanded.frame() && cached.frame()) {
        expectations.expect(
            std::memcmp(serialPixels.data(), cached.frame()->processImage().pixels().data(),
                        serialPixels.size_bytes()) == 0 &&
                coldBanded.frame()->operationCacheStatistics().misses ==
                    plan->operations().size() &&
                cached.frame()->operationCacheStatistics().hits == plan->operations().size(),
            "cached banded kernels preserve the uncached serial golden");
    }

    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    const auto serialDisplay =
        displayPreparer.prepare(serial.frame(), displayRequest(1U << 24U), {}, {}, nullptr);
    const auto parallelDisplay =
        displayPreparer.prepare(parallel.frame(), displayRequest(1U << 24U), {}, {}, &executor);
    const bool displayIdentical =
        serialDisplay.frame() != nullptr && parallelDisplay.frame() != nullptr &&
        serialDisplay.frame()->buffer().pixels().size() ==
            parallelDisplay.frame()->buffer().pixels().size() &&
        std::memcmp(serialDisplay.frame()->buffer().pixels().data(),
                    parallelDisplay.frame()->buffer().pixels().data(),
                    serialDisplay.frame()->buffer().pixels().size_bytes()) == 0;
    expectations.expect(
        displayIdentical,
        "row-parallel display preparation produces byte-for-byte the serial buffer");
}

class CancellationGate final {
  public:
    // ADAPTED (row bands): an operation's rows are now evaluated in BANDS, so the evaluator no
    // longer reports one progress event per row -- it reports the start of a row pass
    // (`completed == 0`) and its end, both from the thread that owns the frame. Pausing on the
    // start event is a stricter rendezvous than the old `completed == 1` one: it stops the worker
    // before the first band has touched a pixel, so the cancellation this test requests has to be
    // observed by a band's own per-row check rather than by the next operation.
    void pauseAtFirstRowPass(const runtime::EvaluationProgress& progress) {
        if (progress.stage != runtime::EvaluationProgressStage::Operation ||
            progress.completed != 0) {
            return;
        }
        pause();
    }

    // ADAPTED (row bands): the display mapping is banded too, so it reports the start of its row
    // pass rather than one event per row -- same rendezvous change as pauseAtFirstRowPass() above.
    void pauseAtFirstDisplayRow(const runtime::ReferenceDisplayProgress& progress) {
        if (progress.stage != runtime::ReferenceDisplayProgressStage::Mapping ||
            progress.completed != 0) {
            return;
        }
        pause();
    }

    [[nodiscard]] bool waitUntilEntered() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 2s, [this] { return entered_; });
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

  private:
    void pause() {
        std::unique_lock lock(mutex_);
        if (entered_) {
            return;
        }
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

void testDeterministicScanlineCancellation(Expectations& expectations) {
    const auto largeFormat = format(64, 64);
    const auto plan = oneSolidPlan({1.0, 0.0, 0.0, 1.0}, {32.0, 32.0}, 1.0, largeFormat);
    runtime::TaskSchedulerConfig config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    runtime::TaskScheduler scheduler(config);
    CancellationGate gate;
    std::atomic_bool evaluatorCancelled = false;
    const runtime::CpuCompositionEvaluator evaluator;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Cancellation fixture", {.kind = runtime::TaskOwnerKind::Composition,
                                                      .id = runtime::TaskOwnerId::fromRaw(1)}),
        [plan, &evaluator, &gate, &evaluatorCancelled](runtime::TaskContext& context) {
            const auto result =
                evaluator.evaluate(plan, requestFor(*plan, 1U << 20U), context.cancellation(),
                                   [&gate](const runtime::EvaluationProgress& update) {
                                       gate.pauseAtFirstRowPass(update);
                                   });
            evaluatorCancelled.store(result.status() == runtime::EvaluationStatus::Cancelled,
                                     std::memory_order_release);
            return result.status() == runtime::EvaluationStatus::Cancelled
                       ? runtime::TaskResult<void>::cancelled()
                       : runtime::TaskResult<void>::succeeded();
        });
    expectations.expect(submission.accepted() && gate.waitUntilEntered(),
                        "cancellation fixture reaches an exact scanline boundary");
    submission.handle.cancel();
    gate.release();

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::optional<runtime::TaskResult<void>> taskResult;
    while (!taskResult.has_value() && std::chrono::steady_clock::now() < deadline) {
        taskResult = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expectations.expect(taskResult.has_value() &&
                            taskResult->state() == runtime::TaskState::Cancelled &&
                            evaluatorCancelled.load(std::memory_order_acquire),
                        "cancellation is observed before the next scanline and publishes no frame");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    expectations.expect(scheduler.isQuiescent(), "cancellation test shuts the scheduler down");
}

// The same rendezvous as the test above, but with the SCHEDULER's own row-band pool driving the
// rows: cancellation has to be observed inside a band, by the per-row check every band makes, and
// no band may go on to finish the frame after the token was cancelled.
void testBandedEvaluationCancelsInsideABand(Expectations& expectations) {
    const auto plan = bandedPlan();
    runtime::TaskSchedulerConfig config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    config.rowBandWorkerCount = 4;
    runtime::TaskScheduler scheduler(config);
    expectations.expect(scheduler.rowBandExecutor() != nullptr &&
                            scheduler.rowBandExecutor()->bandLimit() == 5,
                        "a configured scheduler owns a row-band pool of the requested width");
    CancellationGate gate;
    std::atomic_bool evaluatorCancelled = false;
    std::atomic_bool framePublished = false;
    const runtime::CpuCompositionEvaluator evaluator;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest(
            "Banded cancellation fixture",
            {.kind = runtime::TaskOwnerKind::Composition, .id = runtime::TaskOwnerId::fromRaw(3)}),
        [plan, &evaluator, &gate, &evaluatorCancelled,
         &framePublished](runtime::TaskContext& context) {
            const auto result = evaluator.evaluate(
                plan, requestFor(*plan, 1U << 24U), context.cancellation(),
                [&gate](const runtime::EvaluationProgress& update) {
                    gate.pauseAtFirstRowPass(update);
                },
                context.rowBandExecutor());
            evaluatorCancelled.store(result.status() == runtime::EvaluationStatus::Cancelled,
                                     std::memory_order_release);
            framePublished.store(result.frame() != nullptr, std::memory_order_release);
            return result.status() == runtime::EvaluationStatus::Cancelled
                       ? runtime::TaskResult<void>::cancelled()
                       : runtime::TaskResult<void>::succeeded();
        });
    expectations.expect(submission.accepted() && gate.waitUntilEntered(),
                        "the banded fixture pauses before its first band touches a pixel");
    submission.handle.cancel();
    gate.release();

    const auto deadline = std::chrono::steady_clock::now() + 4s;
    std::optional<runtime::TaskResult<void>> taskResult;
    while (!taskResult.has_value() && std::chrono::steady_clock::now() < deadline) {
        taskResult = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expectations.expect(taskResult.has_value() &&
                            taskResult->state() == runtime::TaskState::Cancelled &&
                            evaluatorCancelled.load(std::memory_order_acquire) &&
                            !framePublished.load(std::memory_order_acquire),
                        "every band observes cancellation and the frame is never published");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    expectations.expect(scheduler.isQuiescent(), "the banded cancellation test shuts down cleanly");
}

void testDisplayPreparationCancellationPublishesNothing(Expectations& expectations) {
    const auto largeFormat = format(64, 64);
    const auto plan = oneSolidPlan({1.0, 0.0, 0.0, 1.0}, {32.0, 32.0}, 1.0, largeFormat);
    const runtime::CpuCompositionEvaluator evaluator;
    const auto process = evaluator.evaluate(plan, requestFor(*plan, 1U << 20U), {});
    expectations.expect(process.frame() != nullptr,
                        "display cancellation fixture has an immutable process frame");

    runtime::TaskSchedulerConfig config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    runtime::TaskScheduler scheduler(config);
    CancellationGate gate;
    std::atomic_bool preparerCancelled = false;
    std::atomic_bool framePublished = false;
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest(
            "Display cancellation fixture",
            {.kind = runtime::TaskOwnerKind::Composition, .id = runtime::TaskOwnerId::fromRaw(2)}),
        [frame = process.frame(), &displayPreparer, &gate, &preparerCancelled,
         &framePublished](runtime::TaskContext& context) {
            const auto result =
                displayPreparer.prepare(frame, displayRequest(1U << 20U), context.cancellation(),
                                        [&gate](const runtime::ReferenceDisplayProgress& update) {
                                            gate.pauseAtFirstDisplayRow(update);
                                        });
            preparerCancelled.store(result.status() ==
                                        runtime::ReferenceDisplayPreparationStatus::Cancelled,
                                    std::memory_order_release);
            framePublished.store(result.frame() != nullptr, std::memory_order_release);
            return result.status() == runtime::ReferenceDisplayPreparationStatus::Cancelled
                       ? runtime::TaskResult<void>::cancelled()
                       : runtime::TaskResult<void>::succeeded();
        });
    expectations.expect(submission.accepted() && gate.waitUntilEntered(),
                        "display cancellation fixture reaches an exact scanline boundary");
    submission.handle.cancel();
    gate.release();

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::optional<runtime::TaskResult<void>> taskResult;
    while (!taskResult.has_value() && std::chrono::steady_clock::now() < deadline) {
        taskResult = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expectations.expect(taskResult.has_value() &&
                            taskResult->state() == runtime::TaskState::Cancelled &&
                            preparerCancelled.load(std::memory_order_acquire) &&
                            !framePublished.load(std::memory_order_acquire),
                        "cancelled display preparation publishes no partial display product");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    expectations.expect(scheduler.isQuiescent(),
                        "display cancellation test shuts the scheduler down");
}

// Task S3: a text layer is present in the composed frame at known glyph positions, behaves like a
// solid through the Layer Output path, and is clipped rather than wrapped when it leaves the frame.
void testTextLayerIsComposedAtKnownGlyphPositions(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto color = core::Color4d{0.5, 0.25, 0.75, 1.0};
    const auto plan = oneTextPlan(color);
    const auto result = evaluator.evaluate(plan, requestFor(*plan), {});
    expectations.expect(result.status() == runtime::EvaluationStatus::Evaluated &&
                            result.frame() != nullptr && result.diagnostics().empty(),
                        "a text layer evaluates with no diagnostics");
    if (result.frame() == nullptr) {
        return;
    }

    // The premultiplied process pixel for an opaque color is the color itself, so the fully covered
    // interior must match it exactly -- coverage 255 is the exact identity, never a 254/255
    // multiply.
    const auto expected = render::solidPixelFromStraightLinearRec709Scene(color);
    expectations.expect(static_cast<bool>(expected), "the fixture text color is evaluable");
    if (!expected) {
        return;
    }
    render::Rgba32f storage{render::Rgba32f::transparent()};
    bool interiorExact = true;
    std::int64_t interiorChecked = 0;
    for (std::int64_t row = 0; row < kFullBlockFullCoverageHeight; ++row) {
        for (std::int64_t column = 0; column < kFullBlockFullCoverageWidth; ++column) {
            const auto x = kFullBlockFullCoverageOriginX + column;
            const auto y = kFullBlockFullCoverageOriginY + row;
            const auto* composed = pixel(result, x, y, storage);
            if (composed == nullptr || *composed != *expected.value()) {
                interiorExact = false;
                continue;
            }
            ++interiorChecked;
        }
    }
    expectations.expect(interiorExact && interiorChecked == kFullBlockFullCoverageWidth *
                                                                kFullBlockFullCoverageHeight,
                        "every fully covered glyph pixel is exactly the premultiplied text color");

    const auto* farCorner = pixel(result, 15, 19, storage);
    expectations.expect(farCorner != nullptr && *farCorner == render::Rgba32f::transparent(),
                        "a frame pixel the glyph does not reach stays transparent black");

    // Empty content is a valid, fully transparent frame -- not a failure and not a frame of noise.
    const auto emptyPlan = oneTextPlan(color, "");
    const auto emptyResult = evaluator.evaluate(emptyPlan, requestFor(*emptyPlan), {});
    expectations.expect(emptyResult.frame() != nullptr &&
                            std::ranges::all_of(emptyResult.frame()->processImage().pixels(),
                                                [](const auto& value) {
                                                    return value == render::Rgba32f::transparent();
                                                }),
                        "a text layer with no content composes a transparent frame");

    // Opacity and position come from the Layer Output stage, exactly as they do for a solid: half
    // opacity halves every premultiplied component of the covered pixels.
    const auto fadedPlan = oneTextPlan(color, std::string(kFullBlock), kFullBlockSize, 0.5);
    const auto faded = evaluator.evaluate(fadedPlan, requestFor(*fadedPlan), {});
    const auto* fadedPixel =
        faded.frame() == nullptr
            ? nullptr
            : pixel(faded, kFullBlockFullCoverageOriginX, kFullBlockFullCoverageOriginY, storage);
    expectations.expect(fadedPixel != nullptr &&
                            near(fadedPixel->alpha(), expected.value()->alpha() * 0.5F) &&
                            near(fadedPixel->red(), expected.value()->red() * 0.5F),
                        "layer opacity fades text exactly like it fades a solid");

    // Translated far off the left edge, the glyph is clipped away rather than wrapped around.
    const auto offFramePlan =
        oneTextPlan(color, std::string(kFullBlock), kFullBlockSize, 1.0, {-1000.0, 10.0});
    const auto offFrame = evaluator.evaluate(offFramePlan, requestFor(*offFramePlan), {});
    expectations.expect(offFrame.frame() != nullptr &&
                            std::ranges::all_of(offFrame.frame()->processImage().pixels(),
                                                [](const auto& value) {
                                                    return value == render::Rgba32f::transparent();
                                                }),
                        "text moved off the frame is clipped, never wrapped");

    // A size the evaluator cannot rasterize is a typed parameter diagnostic naming the size
    // parameter, not a crash or a blank frame.
    const auto hugePlan =
        oneTextPlan(color, std::string(kFullBlock), document::kMaximumTextSizePixels + 1.0);
    const auto huge = evaluator.evaluate(hugePlan, requestFor(*hugePlan), {});
    expectations.expect(
        huge.status() == runtime::EvaluationStatus::Failed && huge.frame() == nullptr &&
            !huge.diagnostics().empty() &&
            huge.diagnostics().front().code ==
                runtime::EvaluationDiagnosticCode::InvalidParameter &&
            huge.diagnostics().front().subject.parameterId == kTextSize &&
            huge.diagnostics().front().subject.field == "size",
        "an out-of-domain text size fails with a diagnostic naming the size parameter");

    // A proxy evaluation scales the glyph, so a smaller frame holds a smaller picture of the same
    // composition rather than full-size glyphs in a cropped frame.
    const auto proxyExtent = render::ImageExtent::create(8, 10);
    expectations.expect(static_cast<bool>(proxyExtent), "the proxy fixture extent is valid");
    if (proxyExtent) {
        const auto proxy = evaluator.evaluate(
            plan, requestFor(*plan, 1U << 20U, runtime::ProxyResolution{*proxyExtent.value()}), {});
        std::size_t proxyInk = 0;
        if (proxy.frame() != nullptr) {
            for (const auto& value : proxy.frame()->processImage().pixels()) {
                proxyInk += value == render::Rgba32f::transparent() ? 0U : 1U;
            }
        }
        std::size_t fullInk = 0;
        for (const auto& value : result.frame()->processImage().pixels()) {
            fullInk += value == render::Rgba32f::transparent() ? 0U : 1U;
        }
        expectations.expect(proxy.frame() != nullptr && proxyInk > 0 && proxyInk < fullInk,
                            "a half-resolution proxy draws the same glyph smaller, not cropped");
    }
}

#include "content_bounds_tests.ipp"
void testContinuousTextRasterisation(Expectations& expectations) {
    runtime::CpuCompositionEvaluator evaluator;
    const auto native = oneTextPlan({1, 1, 1, 1}, "H", 16, 1, {32, 32}, format(64, 64));
    const auto base = evaluator.evaluate(native, requestFor(*native), {});
    expectations.expect(base.frame() != nullptr, "native text bounds evaluate");
    if (!base.frame())
        return;
    const auto centre = base.frame()->evaluatedBounds()[0].output.centre();
    auto definition = native->copyDefinition();
    auto& layer = std::get<runtime::CompiledLayerOutput>(definition.operations[1]);
    layer.scale.source = document::Vec2d{4, 4};
    layer.position.source = document::Vec2d{centre.x * 4, centre.y * 4};
    const auto scaled =
        std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
    runtime::OperationCacheStatistics statistics;
    const auto result =
        evaluator.evaluate(scaled, requestFor(*scaled), {}, {}, nullptr, &statistics);
    expectations.expect(result.frame() && statistics.hits >= 1,
                        "scaled vector reuses native source cache safely");
    if (!result.frame())
        return;
    render::Rgba32f value = render::Rgba32f::transparent();
    const auto* before = pixel(result, 5, 20, value);
    expectations.expect(before && before->alpha() == 0,
                        "400 percent H outside stem stays transparent");
    const auto* edge = pixel(result, 6, 20, value);
    expectations.expect(edge && near(edge->alpha(), 191.0F / 255.0F),
                        "400 percent H coverage is outline area");
    const auto* inside = pixel(result, 7, 20, value);
    expectations.expect(inside && inside->alpha() == 1,
                        "400 percent H reaches full opacity in one pixel");
    const auto warm = evaluator.evaluate(scaled, requestFor(*scaled), {}, {}, nullptr, &statistics);
    expectations.expect(warm.frame() && statistics.misses == 0 &&
                            std::ranges::equal(warm.frame()->processImage().pixels(),
                                               result.frame()->processImage().pixels()),
                        "vector cache hit is bit-identical to cold output");
}

#include "operation_memoization_tests.ipp"

void testRegionOfInterest(Expectations& expectations) {
    runtime::CpuCompositionEvaluator evaluator;
    const std::array plans{oneSolidPlan(), twoSolidPlan(),
                           squareTransformPlan({.position = {2.25, 1.5}, .rotation = 31}),
                           squareTransformPlan({.position = {30, 30}})};
    for (const auto& plan : plans) {
        auto request = requestFor(*plan);
        const auto full = evaluator.evaluate(plan, request, {});
        const auto region = render::ImageWindow::create(1, 0, 2, 1);
        request.roi = *region.value();
        const auto cropped = evaluator.evaluate(plan, request, {});
        expectations.expect(full.frame() && cropped.frame(), "ROI evaluates successfully");
        if (!full.frame() || !cropped.frame())
            continue;
        expectations.expect(cropped.frame()->processImage().descriptor()->dataWindow() ==
                                *request.roi,
                            "ROI process storage is limited to the requested data window");
        expectations.expect(
            std::ranges::equal(full.frame()->evaluatedBounds(), cropped.frame()->evaluatedBounds()),
            "ROI retains complete evaluated geometry");
        for (std::int64_t x = 1; x < 3; ++x) {
            render::Rgba32f a = render::Rgba32f::transparent(), b = a;
            const auto* expected = pixel(full, x, 0, a);
            const auto* actual = pixel(cropped, x, 0, b);
            expectations.expect(expected && actual &&
                                    std::bit_cast<std::array<std::uint32_t, 4>>(*expected) ==
                                        std::bit_cast<std::array<std::uint32_t, 4>>(*actual),
                                "ROI pixels are byte-identical to full evaluation");
        }
        expectations.expect(full.frame()->identity() != cropped.frame()->identity() &&
                                cropped.frame()->identity().evaluatorSemanticsVersion == 9,
                            "ROI changes request identity without changing semantics versions");
        const auto again = evaluator.evaluate(plan, request, {});
        expectations.expect(again.frame() && again.frame()->operationCacheStatistics().misses == 0,
                            "repeated ROI reuses only matching operation entries");
        request.roi.reset();
        const auto restored = evaluator.evaluate(plan, request, {});
        expectations.expect(restored.frame() &&
                                std::ranges::equal(full.frame()->processImage().pixels(),
                                                   restored.frame()->processImage().pixels()),
                            "clearing ROI restores the unchanged full-frame golden");
        const auto invalid = render::ImageWindow::create(-1, 0, 1, 1);
        request.roi = *invalid.value();
        expectations.expect(evaluator.evaluate(plan, request, {}).status() ==
                                runtime::EvaluationStatus::Failed,
                            "out-of-resolution ROI is rejected");
    }
}

void testImageEffectGroundwork(Expectations& expectations) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto baselinePlan = oneSolidPlan({-0.25, 2.0, 0.5, 0.5});
    const auto baseline = evaluator.evaluate(baselinePlan, requestFor(*baselinePlan), {});
    auto definition = baselinePlan->copyDefinition();
    for (std::size_t i = 0; i < 3; ++i)
        definition.operations.insert(
            definition.operations.begin() + static_cast<std::ptrdiff_t>(1 + i),
            runtime::CompiledImageEffect{document::NodeId::fromRaw(100 + i),
                                         runtime::OperationIndex::fromRaw(i),
                                         runtime::IdentityImageKernel{}, false});
    std::get<runtime::CompiledLayerOutput>(definition.operations[4]).input =
        runtime::OperationIndex::fromRaw(3);
    std::get<runtime::CompiledMerge>(definition.operations[5]).entries[0].input =
        runtime::OperationIndex::fromRaw(4);
    std::get<runtime::CompiledCompositionOutput>(definition.operations[6]).input =
        runtime::OperationIndex::fromRaw(5);
    definition.output = runtime::OperationIndex::fromRaw(6);
    const auto plan = publishPlan(std::move(definition));
    const auto first = evaluator.evaluate(plan, requestFor(*plan), {});
    const auto second = evaluator.evaluate(plan, requestFor(*plan), {});
    expectations.expect(baseline.frame() && first.frame() && second.frame(),
                        "three identity effects evaluate");
    if (baseline.frame() && first.frame() && second.frame()) {
        expectations.expect(std::ranges::equal(baseline.frame()->processImage().pixels(),
                                               first.frame()->processImage().pixels()),
                            "identity chain preserves premultiplied negative/HDR pixels exactly");
        const auto& statistics = second.frame()->operationCacheStatistics();
        expectations.expect(statistics.hits == 7 && statistics.misses == 0,
                            "three effects each memoize independently");
        expectations.expect(!plan->operationTimeDependent(runtime::OperationIndex::fromRaw(3)),
                            "effects inherit static input time dependence");
    }
    auto invalid = plan->copyDefinition();
    std::get<runtime::CompiledImageEffect>(invalid.operations[1]).input =
        runtime::OperationIndex::fromRaw(2);
    const auto invalidPlan = publishPlan(std::move(invalid));
    expectations.expect(evaluator.evaluate(invalidPlan, requestFor(*invalidPlan), {}).status() ==
                            runtime::EvaluationStatus::Failed,
                        "effect forward references fail preflight");
}

} // namespace

int main(int argc, char* argv[]) {
    Expectations expectations;
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--memo-benchmark") {
            benchmarkOperationMemoization(expectations);
            return expectations.failures() == 0 ? 0 : 1;
        }
        testImageEffectGroundwork(expectations);
        testRegionOfInterest(expectations);
        testContinuousTextRasterisation(expectations);
        testParentedBounds(expectations);
        testContentBounds(expectations);
        testContentBoundsEdgeCases(expectations);
        testLayerBoundsReadout(expectations);
        testMemoryBudgetLedger(expectations);
        testOperationCacheByteAccounting(expectations);
        testOperationCacheLifecycle(expectations);
        testOperationMemoization(expectations);
        testOperationTimeInvariance(expectations);
        testEaseHandleChangeReachesAMemoizedFrame(expectations);
        testOperationDirtyPropagation(expectations);
        testNestedMergeEqualsFlat(expectations);
        testTextLayerIsComposedAtKnownGlyphPositions(expectations);
        testDrivenTextContentRendersPerFrame(expectations);
        testDrivenBlendModeCompositesAsItsResolvedMode(expectations);
        testAbsoluteCenterAndFractionalTranslation(expectations);
        testLayerTransformShapesTheFrame(expectations);
        testEveryTransformParameterAnimates(expectations);
        testAnimatedSolidColorChangesPixelsOverTime(expectations);
        testAnimatedTextSizeChangesCoverage(expectations);
        testAnimatedParametersAreSampledOncePerRequest(expectations);
        testClippingAndOpacityEndpoints(expectations);
        testStackOrderingOpacityAndDisplay(expectations);
        testStackCompositesEachLayerUnderItsOwnBlendMode(expectations);
        testLayerRangeIsHalfOpen(expectations);
        testEmptyStackIsTransparent(expectations);
        testProxyAndPeakBudget(expectations);
        testIdentityAndPreparedHandoff(expectations);
        testPublishedPlanOwnsItsImmutableDefinition(expectations);
        testStructuredFailuresAndProgress(expectations);
        testRepeatability(expectations);
        testRowBandPlanIsDeterministicAndBounded(expectations);
        testParallelRowBandsAreBitIdenticalToSerial(expectations);
        testDeterministicScanlineCancellation(expectations);
        testBandedEvaluationCancelsInsideABand(expectations);
        testDisplayPreparationCancellationPublishesNothing(expectations);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    return expectations.failures() == 0 ? 0 : 1;
}
