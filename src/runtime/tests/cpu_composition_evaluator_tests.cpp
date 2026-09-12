#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/composition_settings.hpp>
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
#include <iostream>
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
constexpr auto kTextContent = document::ParameterId::fromRaw(46);
constexpr auto kTextSize = document::ParameterId::fromRaw(47);
constexpr auto kTextColor = document::ParameterId::fromRaw(48);
constexpr auto kAnchorA = document::ParameterId::fromRaw(49);
constexpr auto kScaleA = document::ParameterId::fromRaw(52);
constexpr auto kRotationA = document::ParameterId::fromRaw(53);
constexpr auto kAnchorB = document::ParameterId::fromRaw(54);
constexpr auto kScaleB = document::ParameterId::fromRaw(55);
constexpr auto kRotationB = document::ParameterId::fromRaw(56);
constexpr auto kAnchorCurve = document::AnimationCurveId::fromRaw(55);
constexpr auto kScaleCurve = document::AnimationCurveId::fromRaw(56);
constexpr auto kRotationCurve = document::AnimationCurveId::fromRaw(57);

// The authored transform a Layer Output carries. Defaulted to the identity -- no anchor offset,
// unit scale, no rotation -- so a fixture that cares only about position or opacity reads exactly
// as it did before the transform breadth slice, and a fixture that cares about the transform names
// only the value it is exercising.
struct LayerTransformValues final {
    document::Vec2d position{2.0, 1.0};
    document::Vec2d anchor = document::kDefaultAnchor;
    document::Vec2d scale = document::kDefaultScale;
    double rotation = document::kDefaultRotationDegrees;
    double opacity = 1.0;
};

struct LayerParameterIds final {
    document::ParameterId position;
    document::ParameterId anchor;
    document::ParameterId scale;
    document::ParameterId rotation;
    document::ParameterId opacity;
};

inline constexpr LayerParameterIds kLayerParametersA{kPositionA, kAnchorA, kScaleA, kRotationA,
                                                     kOpacityA};
inline constexpr LayerParameterIds kLayerParametersB{kPositionB, kAnchorB, kScaleB, kRotationB,
                                                     kOpacityB};

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
        runtime::CompiledScalarParameter{ids.opacity, values.opacity}};
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
    operations.emplace_back(runtime::CompiledSolid{kSolidNodeA, kColorA, color});
    operations.emplace_back(layerOutput(kLayerNodeA, kLayerA, runtime::OperationIndex::fromRaw(0),
                                        kLayerParametersA,
                                        {.position = position, .opacity = opacity}));
    operations.emplace_back(runtime::CompiledLayerStack{
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
    operations.emplace_back(
        runtime::CompiledSolid{kSolidNodeA, kColorA, core::Color4d{1.0, 0.0, 0.0, 0.5}});
    operations.emplace_back(layerOutput(kLayerNodeA, kLayerA, runtime::OperationIndex::fromRaw(0),
                                        kLayerParametersA, {}));
    operations.emplace_back(
        runtime::CompiledSolid{kSolidNodeB, kColorB, core::Color4d{0.0, 0.0, 1.0, 1.0}});
    operations.emplace_back(layerOutput(kLayerNodeB, kLayerB, runtime::OperationIndex::fromRaw(2),
                                        kLayerParametersB, {}));
    const runtime::CompiledLayerStackEntry red{kSlotA, kLayerA,
                                               runtime::OperationIndex::fromRaw(1)};
    const runtime::CompiledLayerStackEntry blue{kSlotB, kLayerB,
                                                runtime::OperationIndex::fromRaw(3)};
    operations.emplace_back(runtime::CompiledLayerStack{
        kStackNode, redOnTop ? std::vector{red, blue} : std::vector{blue, red}});
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
constexpr std::int64_t kFullBlockOriginX = -1;
constexpr std::int64_t kFullBlockOriginY = -1;
constexpr std::int64_t kFullBlockFullCoverageWidth = 9;
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
    operations.emplace_back(runtime::CompiledText{kTextNode, kTextContent, content, kTextSize, size,
                                                  kTextColor, color});
    operations.emplace_back(layerOutput(kLayerNodeA, kLayerA, runtime::OperationIndex::fromRaw(0),
                                        kLayerParametersA,
                                        {.position = position, .opacity = opacity}));
    operations.emplace_back(runtime::CompiledLayerStack{
        kStackNode, {{kSlotA, kLayerA, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNode, runtime::OperationIndex::fromRaw(2)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{
            document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
            std::move(operations), runtime::OperationIndex::fromRaw(3)});
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan> emptyStackPlan() {
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledLayerStack{kStackNode, {}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNode, runtime::OperationIndex::fromRaw(0)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{document::Revision::fromRaw(7), kProjectId,
                                                   kCompositionId, format(), std::move(operations),
                                                   runtime::OperationIndex::fromRaw(1)});
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
    definition.vec2Curves.push_back({kPositionCurve,
                                     {{document::KeyframeId::fromRaw(62),
                                       core::RationalTime::fromInteger(0),
                                       {2.0, 1.0},
                                       runtime::CompiledKeyframeInterpolation::Linear},
                                      {document::KeyframeId::fromRaw(63),
                                       core::RationalTime::fromInteger(1),
                                       {3.0, 1.0},
                                       runtime::CompiledKeyframeInterpolation::Linear}}});
    return std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
}

[[nodiscard]] runtime::EvaluationRequest
requestFor(const runtime::CompiledCompositionPlan& plan, const std::size_t budget = 1U << 20U,
           runtime::EvaluationResolution resolution = runtime::CompositionFormatResolution{}) {
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
            .aggregatePixelStorageByteLimit = aggregateBudget};
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
    expectations.expect(shiftedResult.status() == runtime::EvaluationStatus::Evaluated &&
                            edgePixel != nullptr && near(edgePixel->red(), 0.5F) &&
                            near(edgePixel->alpha(), 0.5F),
                        "fractional center displacement bilinearly blends transparent borders");
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
    operations.emplace_back(
        runtime::CompiledSolid{kSolidNodeA, kColorA, core::Color4d{1.0, 1.0, 1.0, 1.0}});
    operations.emplace_back(layerOutput(kLayerNodeA, kLayerA, runtime::OperationIndex::fromRaw(0),
                                        kLayerParametersA, values));
    operations.emplace_back(runtime::CompiledLayerStack{
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
    constexpr std::array<float, 16> kLeftBar{1.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 0.0F,
                                             1.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 0.0F};
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
    expectations.expect(anchoredMask.has_value() && *anchoredMask == kLeftBar,
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
    constexpr std::array<float, 16> kLeftBar{1.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 0.0F,
                                             1.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 0.0F};

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
    scaleDefinition.vec2Curves.push_back({kScaleCurve,
                                          {{document::KeyframeId::fromRaw(70),
                                            core::RationalTime::fromInteger(0),
                                            {1.0, 1.0},
                                            runtime::CompiledKeyframeInterpolation::Linear},
                                           {document::KeyframeId::fromRaw(71),
                                            core::RationalTime::fromInteger(1),
                                            {0.5, 1.0},
                                            runtime::CompiledKeyframeInterpolation::Linear}}});
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

    // Anchor: the centre at t = 0 to the left edge at t = 1, with a constant one-axis narrowing, so
    // the bar slides from the middle to the left edge without the layer itself moving.
    auto anchorDefinition =
        squareTransformPlan({.position = kSquareCentre, .scale = document::Vec2d{0.5, 1.0}})
            ->copyDefinition();
    std::get<runtime::CompiledLayerOutput>(anchorDefinition.operations[1]).anchor.source =
        runtime::Vec2CurveIndex::fromRaw(0);
    anchorDefinition.vec2Curves.push_back({kAnchorCurve,
                                           {{document::KeyframeId::fromRaw(74),
                                             core::RationalTime::fromInteger(0),
                                             {0.0, 0.0},
                                             runtime::CompiledKeyframeInterpolation::Linear},
                                            {document::KeyframeId::fromRaw(75),
                                             core::RationalTime::fromInteger(1),
                                             {-1.5, 0.0},
                                             runtime::CompiledKeyframeInterpolation::Linear}}});
    const auto anchorPlan = publishPlan(std::move(anchorDefinition));
    const auto anchorStart = maskAt(anchorPlan, 0, 1);
    const auto anchorMiddle = maskAt(anchorPlan, 1, 2);
    const auto anchorEnd = maskAt(anchorPlan, 1, 1);
    expectations.expect(anchorStart.has_value() && *anchorStart == kVerticalBar &&
                            anchorEnd.has_value() && *anchorEnd == kLeftBar &&
                            anchorMiddle.has_value() && *anchorMiddle != kVerticalBar &&
                            *anchorMiddle != kLeftBar,
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
                            edge != nullptr && near(edge->red(), 0.25F) &&
                            near(edge->alpha(), 0.25F),
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
    duplicateKeyDefinition.vec2Curves.front().keyframes.front().id =
        duplicateKeyDefinition.scalarCurves.front().keyframes.front().id;
    const auto duplicateKey = publishPlan(std::move(duplicateKeyDefinition));
    const auto duplicateKeyResult = evaluator.evaluate(duplicateKey, requestFor(*duplicateKey), {});
    expectations.expect(duplicateKeyResult.status() == runtime::EvaluationStatus::Failed &&
                            !duplicateKeyResult.diagnostics().empty() &&
                            duplicateKeyResult.diagnostics().front().subject.animationCurveId ==
                                kPositionCurve &&
                            duplicateKeyResult.diagnostics().front().subject.keyframeId ==
                                duplicateKey->vec2Curves().front().keyframes.front().id,
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
    expectations.expect(proxyEdgePixel != nullptr && near(proxyEdgePixel->red(), 0.5F) &&
                            near(proxyEdgePixel->alpha(), 0.5F),
                        "absolute authoring displacement is scaled independently for a proxy");

    // 4x2 RGBA32F is 128 bytes. This plan peaks at two resident process images (256 bytes).
    const auto below = evaluator.evaluate(plan, requestFor(*plan, 255), {});
    const auto exact = evaluator.evaluate(plan, requestFor(*plan, 256), {});
    expectations.expect(below.status() == runtime::EvaluationStatus::Failed &&
                            !below.diagnostics().empty() &&
                            below.diagnostics().front().code ==
                                runtime::EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                        "preflight rejects one byte below the exact live-image peak");
    expectations.expect(exact.status() == runtime::EvaluationStatus::Evaluated,
                        "exact live-image peak budget succeeds");

    const auto twoLayers = twoSolidPlan();
    const auto twoBelow = evaluator.evaluate(twoLayers, requestFor(*twoLayers, 383), {});
    const auto twoExact = evaluator.evaluate(twoLayers, requestFor(*twoLayers, 384), {});
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
    std::get<runtime::CompiledSolid>(retainedDefinition.operations.front()).color =
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

class CancellationGate final {
  public:
    void pauseAtFirstRow(const runtime::EvaluationProgress& progress) {
        if (progress.stage != runtime::EvaluationProgressStage::Operation ||
            progress.completed != 1) {
            return;
        }
        std::unique_lock lock(mutex_);
        if (entered_) {
            return;
        }
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    void pauseAtFirstDisplayRow(const runtime::ReferenceDisplayProgress& progress) {
        if (progress.stage != runtime::ReferenceDisplayProgressStage::Mapping ||
            progress.completed != 1) {
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
                                       gate.pauseAtFirstRow(update);
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
            // Bitmap (1 + column, 1 + row) is inside the full-coverage interior; the text origin is
            // the frame's own data-window origin, so the frame pixel is that plus the bitmap
            // origin.
            const auto x = kFullBlockOriginX + 1 + column;
            const auto y = kFullBlockOriginY + 1 + row;
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
    const auto* fadedPixel = faded.frame() == nullptr ? nullptr : pixel(faded, 2, 2, storage);
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

} // namespace

int main() {
    Expectations expectations;
    try {
        testTextLayerIsComposedAtKnownGlyphPositions(expectations);
        testAbsoluteCenterAndFractionalTranslation(expectations);
        testLayerTransformShapesTheFrame(expectations);
        testEveryTransformParameterAnimates(expectations);
        testAnimatedParametersAreSampledOncePerRequest(expectations);
        testClippingAndOpacityEndpoints(expectations);
        testStackOrderingOpacityAndDisplay(expectations);
        testEmptyStackIsTransparent(expectations);
        testProxyAndPeakBudget(expectations);
        testIdentityAndPreparedHandoff(expectations);
        testPublishedPlanOwnsItsImmutableDefinition(expectations);
        testStructuredFailuresAndProgress(expectations);
        testRepeatability(expectations);
        testDeterministicScanlineCancellation(expectations);
        testDisplayPreparationCancellationPublishesNothing(expectations);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    return expectations.failures() == 0 ? 0 : 1;
}
