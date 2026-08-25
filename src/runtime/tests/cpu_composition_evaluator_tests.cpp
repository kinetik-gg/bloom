#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/render/image_types.hpp>

#include <algorithm>
#include <atomic>
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
    operations.emplace_back(
        runtime::CompiledLayerOutput{kLayerNodeA, kLayerA, runtime::OperationIndex::fromRaw(0),
                                     runtime::CompiledVec2Parameter{kPositionA, position},
                                     runtime::CompiledScalarParameter{kOpacityA, opacity}});
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
    operations.emplace_back(runtime::CompiledLayerOutput{
        kLayerNodeA, kLayerA, runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{kPositionA, document::Vec2d{2.0, 1.0}},
        runtime::CompiledScalarParameter{kOpacityA, 1.0}});
    operations.emplace_back(
        runtime::CompiledSolid{kSolidNodeB, kColorB, core::Color4d{0.0, 0.0, 1.0, 1.0}});
    operations.emplace_back(runtime::CompiledLayerOutput{
        kLayerNodeB, kLayerB, runtime::OperationIndex::fromRaw(2),
        runtime::CompiledVec2Parameter{kPositionB, document::Vec2d{2.0, 1.0}},
        runtime::CompiledScalarParameter{kOpacityB, 1.0}});
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
            .resolution = std::move(resolution),
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
    expectations.expect(descriptor != nullptr && descriptor->dataWindow().extent().width() == 2 &&
                            descriptor->dataWindow().extent().height() == 2 &&
                            descriptor->pixelAspect() == *core::PixelAspectRatio::create(2, 1),
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
            ? std::make_shared<const runtime::PreparedPreviewFrame>(std::move(*preparedOne))
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
    invalidRequest.quality = static_cast<runtime::EvaluationQuality>(99);
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

} // namespace

int main() {
    Expectations expectations;
    try {
        testAbsoluteCenterAndFractionalTranslation(expectations);
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
