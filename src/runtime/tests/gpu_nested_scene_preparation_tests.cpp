// CPU-side production preparation proof for CompiledCompositionSource nested compositions.
//
// Every fixture is a REAL CompiledCompositionPlan pair (parent owning a child through
// nestedPlans), prepared through the production CpuGpuSceneBuilder and compared bit-for-bit against
// a genuine CpuCompositionEvaluator frame: identity, bounds, output descriptor, and every output
// pixel replayed with the existing CPU primitives. The child is never CPU-evaluated into an
// uploaded frame -- the prepared scene carries the child's own genuine GPU commands, spliced into
// the parent command list.
//
// No device and no loader are required; this is a pure CPU gate.

#include "gpu_scene_preparation_test_support.hpp"

#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/runtime/task_types.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>

namespace {

using bloom::document::CompositionId;
using bloom::document::NodeId;
using bloom::document::ParameterId;
using bloom::runtime::CompiledCompositionPlanDefinition;
using bloom::runtime::CompiledCompositionSource;
using bloom::runtime::CompiledCompositionTimeMapping;
using bloom::runtime::CompiledKeyframeInterpolation;
using bloom::runtime::CompiledScalarCurve;
using bloom::runtime::CompiledScalarKeyframe;
using bloom::runtime::GpuSceneAffineCommand;
using bloom::runtime::GpuSceneBlendCommand;
using bloom::runtime::GpuSceneCoverageSolidCommand;
using bloom::runtime::GpuSceneMergeCommand;
using bloom::runtime::GpuSceneSolidCommand;
using bloom::runtime::GpuSceneTranslationCommand;
using bloom::runtime::GpuSceneUploadCommand;

constexpr std::uint64_t kRevision = 7;
constexpr auto kProject = bloom::document::ProjectId::fromRaw(1);

[[nodiscard]] CompiledCompositionTimeMapping timeMapping(const std::uint64_t idBase,
                                                         const double offset, const double scale,
                                                         const std::int64_t loopMode) {
    return CompiledCompositionTimeMapping{{ParameterId::fromRaw(idBase), offset},
                                          {ParameterId::fromRaw(idBase + 1), scale},
                                          loopMode};
}

// A solid A -> layer A, solid B -> layer B, both merged bottom-to-top, then composition output. The
// two branches are independent, which is what the key-isolation proof exercises. Layer A's opacity
// can be put on a linear curve so the child becomes time dependent.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
childPlan(const CompositionFormat compositionFormat, const LayerValues a, const LayerValues b,
          const double solidWidth, const double solidHeight, const std::uint64_t idBase,
          const std::uint64_t compositionRaw, const bool animatedOpacity) {
    const LayerIds idsA{ParameterId::fromRaw(idBase + 0), ParameterId::fromRaw(idBase + 1),
                        ParameterId::fromRaw(idBase + 2), ParameterId::fromRaw(idBase + 3),
                        ParameterId::fromRaw(idBase + 4), ParameterId::fromRaw(idBase + 5)};
    const LayerIds idsB{ParameterId::fromRaw(idBase + 6),  ParameterId::fromRaw(idBase + 7),
                        ParameterId::fromRaw(idBase + 8),  ParameterId::fromRaw(idBase + 9),
                        ParameterId::fromRaw(idBase + 10), ParameterId::fromRaw(idBase + 11)};
    std::vector<CompiledOperation> operations;
    operations.emplace_back(
        CompiledSolid{NodeId::fromRaw(idBase + 12),
                      {ParameterId::fromRaw(idBase + 20), Color4d{0.5, 0.25, 0.125, 1.0}},
                      {ParameterId::fromRaw(idBase + 21), solidWidth},
                      {ParameterId::fromRaw(idBase + 22), solidHeight}});
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 13),
                                        bloom::document::LayerId::fromRaw(idBase + 30),
                                        OperationIndex::fromRaw(0), idsA, a));
    operations.emplace_back(
        CompiledSolid{NodeId::fromRaw(idBase + 40),
                      {ParameterId::fromRaw(idBase + 41), Color4d{0.125, 0.375, 0.75, 0.5}},
                      {ParameterId::fromRaw(idBase + 42), solidWidth},
                      {ParameterId::fromRaw(idBase + 43), solidHeight}});
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 44),
                                        bloom::document::LayerId::fromRaw(idBase + 45),
                                        OperationIndex::fromRaw(2), idsB, b));
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(idBase + 52),
                      std::vector<CompiledMergeInput>{
                          CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(idBase + 50),
                                             bloom::document::LayerId::fromRaw(idBase + 30),
                                             OperationIndex::fromRaw(1)},
                          CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(idBase + 51),
                                             bloom::document::LayerId::fromRaw(idBase + 45),
                                             OperationIndex::fromRaw(3)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 53), OperationIndex::fromRaw(4)});
    CompiledCompositionPlanDefinition definition{bloom::document::Revision::fromRaw(kRevision),
                                                 kProject,
                                                 CompositionId::fromRaw(compositionRaw),
                                                 compositionFormat,
                                                 std::move(operations),
                                                 OperationIndex::fromRaw(5)};
    definition.duration = RationalTime::fromInteger(100);
    if (animatedOpacity) {
        auto& layerA = std::get<CompiledLayerOutput>(definition.operations[1]);
        layerA.opacity.source = bloom::runtime::ScalarCurveIndex::fromRaw(0);
        CompiledScalarCurve curve;
        curve.id = bloom::document::AnimationCurveId::fromRaw(idBase + 60);
        curve.keyframes.push_back({bloom::document::KeyframeId::fromRaw(idBase + 61),
                                   RationalTime::fromInteger(0), 0.25,
                                   CompiledKeyframeInterpolation::Linear});
        curve.keyframes.push_back({bloom::document::KeyframeId::fromRaw(idBase + 62),
                                   RationalTime::fromInteger(4), 1.0,
                                   CompiledKeyframeInterpolation::Linear});
        definition.scalarCurves.push_back(std::move(curve));
    }
    return publish(std::move(definition));
}

// A child whose merge folds nothing, so the child publishes a fully transparent composition.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
emptyChildPlan(const CompositionFormat compositionFormat, const std::uint64_t idBase,
               const std::uint64_t compositionRaw) {
    std::vector<CompiledOperation> operations;
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(idBase), std::vector<CompiledMergeInput>{}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 1), OperationIndex::fromRaw(0)});
    CompiledCompositionPlanDefinition definition{bloom::document::Revision::fromRaw(kRevision),
                                                 kProject,
                                                 CompositionId::fromRaw(compositionRaw),
                                                 compositionFormat,
                                                 std::move(operations),
                                                 OperationIndex::fromRaw(1)};
    definition.duration = RationalTime::fromInteger(100);
    return publish(std::move(definition));
}

[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
parentPlan(const std::shared_ptr<const CompiledCompositionPlan>& child,
           const CompositionFormat compositionFormat, const CompiledCompositionTimeMapping& mapping,
           const std::uint64_t idBase, const std::uint64_t compositionRaw) {
    std::vector<CompiledOperation> operations;
    operations.emplace_back(CompiledCompositionSource{NodeId::fromRaw(idBase), 0, mapping});
    operations.emplace_back(CompiledMerge{
        NodeId::fromRaw(idBase + 1), std::vector<CompiledMergeInput>{CompiledMergeInput{
                                         bloom::document::LayerSlotId::fromRaw(idBase + 2),
                                         bloom::document::LayerId{}, OperationIndex::fromRaw(0)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 3), OperationIndex::fromRaw(1)});
    CompiledCompositionPlanDefinition definition{bloom::document::Revision::fromRaw(kRevision),
                                                 kProject,
                                                 CompositionId::fromRaw(compositionRaw),
                                                 compositionFormat,
                                                 std::move(operations),
                                                 OperationIndex::fromRaw(2)};
    definition.duration = RationalTime::fromInteger(100);
    definition.nestedPlans.push_back(child);
    return publish(std::move(definition));
}

// A parent where a Layer Output transforms the nested composition before the merge. The nested
// source is a raster leaf exactly like a media source, so it takes the same translation path.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
layerParentPlan(const std::shared_ptr<const CompiledCompositionPlan>& child,
                const CompositionFormat compositionFormat,
                const CompiledCompositionTimeMapping& mapping, const LayerValues layerValues,
                const std::uint64_t idBase, const std::uint64_t compositionRaw) {
    const LayerIds ids{ParameterId::fromRaw(idBase + 10), ParameterId::fromRaw(idBase + 11),
                       ParameterId::fromRaw(idBase + 12), ParameterId::fromRaw(idBase + 13),
                       ParameterId::fromRaw(idBase + 14), ParameterId::fromRaw(idBase + 15)};
    std::vector<CompiledOperation> operations;
    operations.emplace_back(CompiledCompositionSource{NodeId::fromRaw(idBase), 0, mapping});
    const auto layerId = bloom::document::LayerId::fromRaw(idBase + 20);
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 21), layerId,
                                        OperationIndex::fromRaw(0), ids, layerValues));
    operations.emplace_back(CompiledMerge{NodeId::fromRaw(idBase + 22),
                                          std::vector<CompiledMergeInput>{CompiledMergeInput{
                                              bloom::document::LayerSlotId::fromRaw(idBase + 23),
                                              layerId, OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 24), OperationIndex::fromRaw(2)});
    CompiledCompositionPlanDefinition definition{bloom::document::Revision::fromRaw(kRevision),
                                                 kProject,
                                                 CompositionId::fromRaw(compositionRaw),
                                                 compositionFormat,
                                                 std::move(operations),
                                                 OperationIndex::fromRaw(3)};
    definition.duration = RationalTime::fromInteger(100);
    definition.nestedPlans.push_back(child);
    return publish(std::move(definition));
}

[[nodiscard]] EvaluationRequest requestAt(const CompiledCompositionPlan& plan,
                                          const RationalTime time, const std::size_t budget) {
    return EvaluationRequest{.time = time,
                             .output = plan.output(),
                             .resolution = bloom::runtime::CompositionFormatResolution{},
                             .quality = bloom::runtime::EvaluationQuality::Reference,
                             .colorIntent =
                                 bloom::runtime::EvaluationColorIntent::LinearRec709Scene,
                             .pixelStorageByteLimit = budget};
}

[[nodiscard]] const std::string& commandKey(const bloom::runtime::GpuSceneCommand& command) {
    return std::visit([](const auto& item) -> const std::string& { return item.semanticKey; },
                      command);
}

void checkNestedParity(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                       const std::shared_ptr<const CompiledCompositionPlan>& plan,
                       const EvaluationRequest& request, const std::string& label) {
    const CpuGpuSceneBuilder builder;
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), label + ": prepares");
    if (!prepared) {
        std::cerr << label << " diagnostic: " << prepared.diagnostic.message << "\n";
        return;
    }
    // The nested build must have spliced the child's REAL commands in, not uploaded a frame.
    expectations.expect(prepared.scene->commands().size() > plan->operations().size(),
                        label + ": nested commands are spliced, not uploaded");
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, label + ": CPU frame evaluates");
    if (!frame.frame()) {
        return;
    }
    expectations.expect(prepared.scene->processIdentity() == frame.frame()->identity(),
                        label + ": identity matches");
    expectations.expect(
        std::ranges::equal(prepared.scene->bounds(), frame.frame()->evaluatedBounds()),
        label + ": bounds match");
    expectations.expect(prepared.scene->outputDescriptor() ==
                            *frame.frame()->processImage().descriptor(),
                        label + ": output descriptor matches");
    std::vector<std::shared_ptr<const Rgba32fImage>> images;
    expectations.expect(replayScene(*prepared.scene, images), label + ": replays");
    const auto& replayed = images[prepared.scene->outputCommand()];
    expectations.expect(
        replayed != nullptr &&
            replayed->pixels().size() == frame.frame()->processImage().pixels().size() &&
            std::memcmp(replayed->pixels().data(), frame.frame()->processImage().pixels().data(),
                        replayed->pixels().size() * sizeof(Rgba32f)) == 0,
        replayed == nullptr
            ? label + ": replay produced no image"
            : label + ": pixel parity " + firstMismatch(*replayed, frame.frame()->processImage()));
}

void testBasicNestedMerge(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    const auto child = childPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                                 LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0,
                                 1000, 101, false);
    const auto parent =
        parentPlan(child, format(16, 12), timeMapping(1100, 0.0, 1.0, 0), 1200, 100);
    checkNestedParity(expectations, evaluator, parent, requestFor(*parent), "nested merge");
}

void testLayerOverNested(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    const auto child = childPlan(format(16, 12), LayerValues{.position = {8.0, 6.0}},
                                 LayerValues{.position = {8.0, 6.0}}, 16.0, 12.0, 2000, 201, false);
    const auto parent = layerParentPlan(child, format(24, 16), timeMapping(2100, 0.0, 1.0, 0),
                                        LayerValues{.position = {12.3, 8.1}}, 2200, 200);
    checkNestedParity(expectations, evaluator, parent,
                      requestAt(*parent, RationalTime{}, 1U << 28U), "layer over nested");
}

void testDepthThree(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    const auto grandchild =
        childPlan(format(8, 8), LayerValues{.position = {4.0, 4.0}},
                  LayerValues{.position = {5.0, 5.0}}, 5.0, 4.0, 3000, 303, false);
    const auto child =
        parentPlan(grandchild, format(8, 8), timeMapping(3100, 0.0, 1.0, 0), 3200, 302);
    const auto parent = parentPlan(child, format(8, 8), timeMapping(3300, 0.0, 1.0, 0), 3400, 301);
    checkNestedParity(expectations, evaluator, parent,
                      requestAt(*parent, RationalTime{}, 1U << 28U), "depth three nested");
}

void testTimeMapping(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    const auto child = childPlan(format(16, 12), LayerValues{.position = {8.0, 6.0}},
                                 LayerValues{.position = {8.0, 6.0}}, 16.0, 12.0, 4000, 401, true);
    // Offset 1, scale 2, hold.
    const auto held = parentPlan(child, format(16, 12), timeMapping(4100, 1.0, 2.0, 0), 4200, 400);
    for (const auto time :
         {RationalTime{}, rationalTime(1, 2), RationalTime::fromInteger(2), rationalTime(5, 2)}) {
        checkNestedParity(expectations, evaluator, held, requestAt(*held, time, 1U << 28U),
                          "time offset/scale hold");
    }
    // Reverse (negative scale) with loop wrapping.
    const auto reversed =
        parentPlan(child, format(16, 12), timeMapping(4300, 0.0, -1.0, 1), 4400, 402);
    for (const auto time : {RationalTime{}, rationalTime(1, 3), rationalTime(7, 3)}) {
        checkNestedParity(expectations, evaluator, reversed, requestAt(*reversed, time, 1U << 28U),
                          "time reverse loop");
    }
    // Ping-pong.
    const auto pingPong =
        parentPlan(child, format(16, 12), timeMapping(4500, 0.0, 1.0, 2), 4600, 403);
    for (const auto time : {RationalTime{}, rationalTime(5, 2), RationalTime::fromInteger(9)}) {
        checkNestedParity(expectations, evaluator, pingPong, requestAt(*pingPong, time, 1U << 28U),
                          "time ping-pong");
    }
}

void testChildDimensionParAndProxy(Expectations& expectations,
                                   const CpuCompositionEvaluator& evaluator) {
    // The child is a different size from the parent and carries a non-square pixel aspect.
    const auto child =
        childPlan(format(24, 8, pixelAspect(2, 1)), LayerValues{.position = {12.0, 4.0}},
                  LayerValues{.position = {6.0, 4.0}}, 20.0, 6.0, 5000, 501, false);
    const auto parent =
        parentPlan(child, format(16, 16), timeMapping(5100, 0.0, 1.0, 0), 5200, 500);
    checkNestedParity(expectations, evaluator, parent,
                      requestAt(*parent, RationalTime{}, 1U << 28U), "child dimension and PAR");

    // A proxy parent must derive the child proxy extent from the child's own format and the parent
    // scales, exactly like the CPU evaluator.
    const auto extent = bloom::render::ImageExtent::create(8, 4);
    expectations.expect(static_cast<bool>(extent), "proxy extent fixture is valid");
    if (extent) {
        auto request = requestAt(*parent, RationalTime{}, 1U << 28U);
        request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
        checkNestedParity(expectations, evaluator, parent, request, "nested proxy resolution");
    }
}

void testEmptyAndInactiveChild(Expectations& expectations,
                               const CpuCompositionEvaluator& evaluator) {
    const auto empty = emptyChildPlan(format(16, 12), 6000, 601);
    const auto emptyParent =
        parentPlan(empty, format(16, 12), timeMapping(6100, 0.0, 1.0, 0), 6200, 600);
    checkNestedParity(expectations, evaluator, emptyParent,
                      requestAt(*emptyParent, RationalTime{}, 1U << 28U), "empty child");

    // An inactive child layer publishes nothing and the child output stays transparent.
    auto childDefinition =
        childPlan(format(16, 12), LayerValues{.position = {8.0, 6.0}},
                  LayerValues{.position = {8.0, 6.0}}, 16.0, 12.0, 7000, 701, false)
            ->copyDefinition();
    auto& layerA = std::get<CompiledLayerOutput>(childDefinition.operations[1]);
    layerA.inPoint = RationalTime::fromInteger(5);
    layerA.outPoint = RationalTime::fromInteger(6);
    const auto inactive = publish(std::move(childDefinition));
    const auto inactiveParent =
        parentPlan(inactive, format(16, 12), timeMapping(7100, 0.0, 1.0, 0), 7200, 700);
    checkNestedParity(expectations, evaluator, inactiveParent,
                      requestAt(*inactiveParent, RationalTime{}, 1U << 28U), "inactive child");
}

// The nested build recursively invokes the full production builder, so the child may be any leaf
// the builder supports. Text, shape and an HDR/half-alpha solid are proven here; media goes through
// the same child build and is covered by the media preparation tests.
void testNestedVectorLeavesAndHdr(Expectations& expectations,
                                  const CpuCompositionEvaluator& evaluator) {
    {
        auto definition =
            textPlan(format(16, 12), LayerValues{.position = {8.0, 6.0}}, 15000)->copyDefinition();
        definition.compositionId = CompositionId::fromRaw(1501);
        definition.duration = RationalTime::fromInteger(100);
        const auto child = publish(std::move(definition));
        const auto parent =
            parentPlan(child, format(16, 12), timeMapping(15100, 0.0, 1.0, 0), 15200, 1500);
        checkNestedParity(expectations, evaluator, parent,
                          requestAt(*parent, RationalTime{}, 1U << 28U), "nested text child");
    }
    {
        ShapeValues values;
        values.kind = bloom::document::ShapeKind::Ellipse;
        values.cornerRadius = 0.5;
        auto definition =
            shapePlan(format(16, 12), LayerValues{.position = {8.0, 6.0}}, values, 16000)
                ->copyDefinition();
        definition.compositionId = CompositionId::fromRaw(1601);
        definition.duration = RationalTime::fromInteger(100);
        const auto child = publish(std::move(definition));
        const auto parent =
            parentPlan(child, format(16, 12), timeMapping(16100, 0.0, 1.0, 0), 16200, 1600);
        checkNestedParity(expectations, evaluator, parent,
                          requestAt(*parent, RationalTime{}, 1U << 28U), "nested shape child");
    }
    {
        // HDR RGB above one with a fractional alpha exercises strict alpha/linear parity.
        auto definition =
            childPlan(format(16, 12), LayerValues{.position = {8.0, 6.0}},
                      LayerValues{.position = {8.0, 6.0}}, 10.0, 8.0, 17000, 1701, false)
                ->copyDefinition();
        std::get<CompiledSolid>(definition.operations[0]).color.source =
            Color4d{4.0, 8.0, 0.5, 0.75};
        std::get<CompiledSolid>(definition.operations[2]).color.source =
            Color4d{2.0, -0.25, 9.0, 0.5};
        const auto child = publish(std::move(definition));
        const auto parent =
            parentPlan(child, format(16, 12), timeMapping(17100, 0.0, 1.0, 0), 17200, 1700);
        checkNestedParity(expectations, evaluator, parent,
                          requestAt(*parent, RationalTime{}, 1U << 28U), "nested HDR child");
    }
}

void testReferenceFailures(Expectations& expectations) {
    const CpuGpuSceneBuilder builder;
    const auto child =
        childPlan(format(8, 8), LayerValues{}, LayerValues{}, 5.0, 4.0, 8000, 801, false);
    const auto valid = parentPlan(child, format(8, 8), timeMapping(8100, 0.0, 1.0, 0), 8200, 800);

    // No nested plan at all: refused before any resolution, exactly as before nested was prepared.
    {
        auto definition = valid->copyDefinition();
        definition.nestedPlans.clear();
        const auto missing = publish(std::move(definition));
        const auto prepared =
            builder.build(missing, requestAt(*missing, RationalTime{}, 1U << 28U));
        expectations.expect(!prepared && prepared.diagnostic.code ==
                                             PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                            "a missing nested plan is refused as unsupported");
    }
    // Child composition identity must differ from its parent.
    {
        auto definition = valid->copyDefinition();
        definition.operations[0] =
            CompiledCompositionSource{NodeId::fromRaw(9000), 0, timeMapping(9100, 0.0, 1.0, 0)};
        const auto sameId =
            childPlan(format(8, 8), LayerValues{}, LayerValues{}, 5.0, 4.0, 9200, 800, false);
        definition.nestedPlans = {sameId};
        const auto published = publish(std::move(definition));
        const auto prepared =
            builder.build(published, requestAt(*published, RationalTime{}, 1U << 28U));
        expectations.expect(!prepared, "an equal parent/child composition identity is refused");
    }
    // A repeated composition identity along the chain is a cycle and is refused.
    {
        const auto inner =
            childPlan(format(8, 8), LayerValues{}, LayerValues{}, 5.0, 4.0, 10000, 1001, false);
        const auto middle =
            parentPlan(inner, format(8, 8), timeMapping(10100, 0.0, 1.0, 0), 10200, 1002);
        const auto cyclic =
            parentPlan(middle, format(8, 8), timeMapping(10300, 0.0, 1.0, 0), 10400, 1001);
        const auto prepared = builder.build(cyclic, requestAt(*cyclic, RationalTime{}, 1U << 28U));
        expectations.expect(!prepared && prepared.diagnostic.code ==
                                             PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                            "a repeated composition identity is refused as a cycle");
    }
    // An incompatible child revision is refused.
    {
        auto childDefinition = child->copyDefinition();
        childDefinition.sourceRevision = bloom::document::Revision::fromRaw(kRevision + 1);
        const auto otherRevision = publish(std::move(childDefinition));
        const auto parent =
            parentPlan(otherRevision, format(8, 8), timeMapping(10500, 0.0, 1.0, 0), 10600, 800);
        const auto prepared = builder.build(parent, requestAt(*parent, RationalTime{}, 1U << 28U));
        expectations.expect(!prepared, "a child revision mismatch is refused");
    }
}

void testByteCeiling(Expectations& expectations) {
    const auto child =
        childPlan(format(64, 64), LayerValues{.position = {32.0, 32.0}},
                  LayerValues{.position = {32.0, 32.0}}, 64.0, 64.0, 11000, 1101, false);
    const auto parent =
        parentPlan(child, format(64, 64), timeMapping(11100, 0.0, 1.0, 0), 11200, 1100);
    const CpuGpuSceneBuilder builder;
    const auto prepared = builder.build(parent, requestAt(*parent, RationalTime{}, 1U << 14U));
    expectations.expect(!prepared && prepared.diagnostic.code ==
                                         PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
                        "a nested scene under a tiny allowance fails closed on the byte ceiling");
}

// A nested child whose only retained host allocation is its own vector coverage geometry: the
// parent allowance must cover the ACTUAL row ranges + spans, not the width*height R8 mask. A
// 6000x4000 simple child geometry is far below its 24 MB R8 mask, so a 1 MiB allowance admits it.
void testNestedCoverageGeometryBytes(Expectations& expectations) {
    const auto child =
        childPlan(format(128, 128), LayerValues{.position = {64.3, 64.7}},
                  LayerValues{.position = {64.0, 64.0}}, 6000.0, 4000.0, 12000, 1201, false);
    const auto parent =
        parentPlan(child, format(128, 128), timeMapping(12100, 0.0, 1.0, 0), 12200, 1200);
    const CpuGpuSceneBuilder builder;
    const auto prepared = builder.build(parent, requestAt(*parent, RationalTime{}, 1U << 20U));
    expectations.expect(prepared.hasValue(),
                        "a nested child's actual coverage geometry fits a sub-R8 allowance");
    if (!prepared) {
        std::cerr << "nested coverage diagnostic: " << prepared.diagnostic.message << "\n";
    }
}

[[nodiscard]] bloom::runtime::CancellationToken makeCancelledToken() {
    using namespace bloom::runtime;
    TaskSchedulerConfig config = TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    TaskScheduler scheduler(config);
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    CancellationToken token;
    auto submission = scheduler.submit<void>(
        TaskRequest("gpu nested cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(99)}),
        [&](TaskContext& context) {
            {
                std::lock_guard lock(mutex);
                token = context.cancellation();
                entered = true;
            }
            condition.notify_all();
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] { return release; });
            return TaskResult<void>::succeeded();
        });
    if (!submission.accepted()) {
        throw std::logic_error("cancel token task was refused");
    }
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return entered; });
    }
    submission.handle.cancel();
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!submission.handle.tryTakeResult().has_value() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return token;
}

void testCancellation(Expectations& expectations) {
    const auto child =
        childPlan(format(8, 8), LayerValues{}, LayerValues{}, 5.0, 4.0, 12000, 1201, false);
    const auto parent =
        parentPlan(child, format(8, 8), timeMapping(12100, 0.0, 1.0, 0), 12200, 1200);
    const auto token = makeCancelledToken();
    expectations.expect(token.isCancellationRequested(), "the captured token is cancelled");
    const CpuGpuSceneBuilder builder;
    const auto prepared =
        builder.build(parent, requestAt(*parent, RationalTime{}, 1U << 28U), token);
    expectations.expect(!prepared &&
                            prepared.diagnostic.code == PreparedGpuSceneDiagnosticCode::Cancelled,
                        "a pre-cancelled nested request returns no partial scene");
}

// Changing one child branch must leave the other branch's content-addressed command keys intact,
// and the whole-document revision must never enter a key.
void testBranchKeyIsolation(Expectations& expectations) {
    const auto makeChild = [](const Color4d colorB, const std::uint64_t revision) {
        auto definition = childPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                                    LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0,
                                    13000, 1301, false)
                              ->copyDefinition();
        std::get<CompiledSolid>(definition.operations[2]).color.source = colorB;
        definition.sourceRevision = bloom::document::Revision::fromRaw(revision);
        return publish(std::move(definition));
    };
    const CpuGpuSceneBuilder builder;
    const auto makeParent = [](const std::shared_ptr<const CompiledCompositionPlan>& child,
                               const std::uint64_t revision) {
        auto definition =
            parentPlan(child, format(16, 12), timeMapping(13100, 0.0, 1.0, 0), 13200, 1300)
                ->copyDefinition();
        definition.sourceRevision = bloom::document::Revision::fromRaw(revision);
        return publish(std::move(definition));
    };
    const auto childA = makeChild(Color4d{0.125, 0.375, 0.75, 0.5}, kRevision);
    const auto childB = makeChild(Color4d{0.9, 0.1, 0.05, 1.0}, kRevision);
    // A different whole-document revision with IDENTICAL content must produce identical keys.
    const auto childARevi = makeChild(Color4d{0.125, 0.375, 0.75, 0.5}, kRevision + 5);
    const auto parentA = makeParent(childA, kRevision);
    const auto parentB = makeParent(childB, kRevision);
    const auto parentARevi = makeParent(childARevi, kRevision + 5);
    const auto first = builder.build(parentA, requestAt(*parentA, RationalTime{}, 1U << 28U));
    const auto changed = builder.build(parentB, requestAt(*parentB, RationalTime{}, 1U << 28U));
    const auto revisionOnly =
        builder.build(parentARevi, requestAt(*parentARevi, RationalTime{}, 1U << 28U));
    expectations.expect(first.hasValue() && changed.hasValue() && revisionOnly.hasValue(),
                        "branch isolation scenes prepare");
    if (!first || !changed || !revisionOnly) {
        return;
    }

    std::vector<std::string> firstKeys;
    std::vector<std::string> changedKeys;
    std::vector<std::string> revisionKeys;
    for (const auto& command : first.scene->commands())
        firstKeys.push_back(commandKey(command));
    for (const auto& command : changed.scene->commands())
        changedKeys.push_back(commandKey(command));
    for (const auto& command : revisionOnly.scene->commands())
        revisionKeys.push_back(commandKey(command));
    std::ranges::sort(firstKeys);
    std::ranges::sort(changedKeys);
    std::ranges::sort(revisionKeys);
    expectations.expect(firstKeys == revisionKeys,
                        "an unrelated document revision never enters a nested command key");
    expectations.expect(firstKeys != changedKeys, "changing one child branch changes its keys");
    // Exactly the untouched branch keys survive: one solid key is shared and one differs.
    std::vector<std::string> shared;
    std::ranges::set_intersection(firstKeys, changedKeys, std::back_inserter(shared));
    expectations.expect(!shared.empty(),
                        "the untouched child branch keeps its content-addressed key");
}

} // namespace

int main() {
    try {
        Expectations expectations;
        const CpuCompositionEvaluator evaluator;
        testBasicNestedMerge(expectations, evaluator);
        testLayerOverNested(expectations, evaluator);
        testDepthThree(expectations, evaluator);
        testTimeMapping(expectations, evaluator);
        testChildDimensionParAndProxy(expectations, evaluator);
        testEmptyAndInactiveChild(expectations, evaluator);
        testNestedVectorLeavesAndHdr(expectations, evaluator);
        testReferenceFailures(expectations);
        testByteCeiling(expectations);
        testNestedCoverageGeometryBytes(expectations);
        testCancellation(expectations);
        testBranchKeyIsolation(expectations);
        if (!expectations.ok()) {
            std::cerr << "FAIL: GPU nested scene preparation expectations failed\n";
            return 1;
        }
        std::cout << "PASS: GPU nested scene preparation\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
