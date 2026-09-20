// Real native proof that the production GpuSceneExecutor runs a compiled PARENT plan whose
// CompiledCompositionSource splices a compiled CHILD plan's genuine GPU commands, and that the
// readback matches the unchanged CpuCompositionEvaluator oracle under the documented 2e-6 gate.
//
// The scene is built by the REAL production CpuGpuSceneBuilder from a real parent+child compiled
// plan pair; no hand-built PreparedGpuScene is used. The child is never CPU-evaluated into an
// uploaded frame. Readback happens only for the oracle assertion.
//
// Without a device the test reports an explicit SKIP (exit 77); --require-device makes that a
// failure. An explicit --loader selects the Vulkan loader.

#include "gpu_composite_native_support.hpp"

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using bloom::core::Color4d;
using bloom::core::RationalTime;
using bloom::document::CompositionFormat;
using bloom::document::CompositionId;
using bloom::document::NodeId;
using bloom::document::ParameterId;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::ImageWindow;
using bloom::render::readbackResidentImage;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::composite_proof::Expectations;
using bloom::render::composite_proof::Options;
using bloom::render::composite_proof::parseOptions;
using bloom::runtime::CompiledCompositionOutput;
using bloom::runtime::CompiledCompositionPlan;
using bloom::runtime::CompiledCompositionPlanDefinition;
using bloom::runtime::CompiledCompositionSource;
using bloom::runtime::CompiledCompositionTimeMapping;
using bloom::runtime::CompiledLayerOutput;
using bloom::runtime::CompiledMerge;
using bloom::runtime::CompiledMergeInput;
using bloom::runtime::CompiledOperation;
using bloom::runtime::CompiledScalarParameter;
using bloom::runtime::CompiledSolid;
using bloom::runtime::CompiledVec2Parameter;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneCommand;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneExecutorDiagnosticCode;
using bloom::runtime::GpuSceneExecutorPollResult;
using bloom::runtime::GpuSceneSolidCommand;
using bloom::runtime::OperationIndex;

constexpr std::uint64_t kRevision = 7;
constexpr auto kProject = bloom::document::ProjectId::fromRaw(1);
constexpr std::uint64_t kSceneBudget = 64ULL * 1024ULL * 1024ULL;

[[nodiscard]] CompositionFormat format(const std::uint32_t width, const std::uint32_t height) {
    const auto value = CompositionFormat::create(width, height);
    if (!value.has_value()) {
        throw std::logic_error("native nested fixture format is invalid");
    }
    return *value;
}

struct LayerValues final {
    bloom::document::Vec2d position{4.0, 3.0};
    double opacity = 1.0;
};

[[nodiscard]] CompiledLayerOutput layer(const std::uint64_t idBase, const std::uint64_t layerRaw,
                                        const OperationIndex input, const LayerValues values) {
    return CompiledLayerOutput{
        NodeId::fromRaw(idBase),
        bloom::document::LayerId::fromRaw(layerRaw),
        input,
        CompiledVec2Parameter{ParameterId::fromRaw(idBase + 1), values.position},
        CompiledVec2Parameter{ParameterId::fromRaw(idBase + 2), bloom::document::Vec2d{0.0, 0.0}},
        CompiledVec2Parameter{ParameterId::fromRaw(idBase + 3), bloom::document::Vec2d{1.0, 1.0}},
        CompiledScalarParameter{ParameterId::fromRaw(idBase + 4), 0.0},
        CompiledScalarParameter{ParameterId::fromRaw(idBase + 5), values.opacity},
        ParameterId::fromRaw(idBase + 6),
        bloom::core::kDefaultBlendMode};
}

// A child composition: two independently-coloured solid branches merged bottom-to-top.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
childPlan(const std::uint64_t idBase, const std::uint64_t compositionRaw, const Color4d colorB) {
    std::vector<CompiledOperation> operations;
    operations.emplace_back(
        CompiledSolid{NodeId::fromRaw(idBase + 10),
                      {ParameterId::fromRaw(idBase + 11), Color4d{0.5, 0.25, 0.125, 1.0}},
                      {ParameterId::fromRaw(idBase + 12), 8.0},
                      {ParameterId::fromRaw(idBase + 13), 6.0}});
    operations.emplace_back(layer(idBase + 20, idBase + 30, OperationIndex::fromRaw(0),
                                  LayerValues{.position = {4.0, 3.0}}));
    operations.emplace_back(CompiledSolid{NodeId::fromRaw(idBase + 40),
                                          {ParameterId::fromRaw(idBase + 41), colorB},
                                          {ParameterId::fromRaw(idBase + 42), 6.0},
                                          {ParameterId::fromRaw(idBase + 43), 5.0}});
    operations.emplace_back(layer(idBase + 50, idBase + 31, OperationIndex::fromRaw(2),
                                  LayerValues{.position = {6.0, 5.0}, .opacity = 0.75}));
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(idBase + 60),
                      std::vector<CompiledMergeInput>{
                          CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(idBase + 61),
                                             bloom::document::LayerId::fromRaw(idBase + 30),
                                             OperationIndex::fromRaw(1)},
                          CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(idBase + 62),
                                             bloom::document::LayerId::fromRaw(idBase + 31),
                                             OperationIndex::fromRaw(3)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 70), OperationIndex::fromRaw(4)});
    CompiledCompositionPlanDefinition definition{bloom::document::Revision::fromRaw(kRevision),
                                                 kProject,
                                                 CompositionId::fromRaw(compositionRaw),
                                                 format(12, 10),
                                                 std::move(operations),
                                                 OperationIndex::fromRaw(5)};
    definition.duration = RationalTime::fromInteger(100);
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
parentPlan(const std::shared_ptr<const CompiledCompositionPlan>& child,
           const std::uint64_t compositionRaw) {
    std::vector<CompiledOperation> operations;
    operations.emplace_back(CompiledCompositionSource{
        NodeId::fromRaw(9000), 0,
        CompiledCompositionTimeMapping{
            {ParameterId::fromRaw(9001), 0.0}, {ParameterId::fromRaw(9002), 1.0}, 0}});
    operations.emplace_back(CompiledMerge{
        NodeId::fromRaw(9003), std::vector<CompiledMergeInput>{CompiledMergeInput{
                                   bloom::document::LayerSlotId::fromRaw(9004),
                                   bloom::document::LayerId{}, OperationIndex::fromRaw(0)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(9005), OperationIndex::fromRaw(1)});
    CompiledCompositionPlanDefinition definition{bloom::document::Revision::fromRaw(kRevision),
                                                 kProject,
                                                 CompositionId::fromRaw(compositionRaw),
                                                 format(12, 10),
                                                 std::move(operations),
                                                 OperationIndex::fromRaw(2)};
    definition.duration = RationalTime::fromInteger(100);
    definition.nestedPlans.push_back(child);
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

[[nodiscard]] EvaluationRequest requestFor(const CompiledCompositionPlan& plan) {
    return EvaluationRequest{.time = RationalTime{},
                             .output = plan.output(),
                             .resolution = bloom::runtime::CompositionFormatResolution{},
                             .quality = bloom::runtime::EvaluationQuality::Reference,
                             .colorIntent =
                                 bloom::runtime::EvaluationColorIntent::LinearRec709Scene,
                             .pixelStorageByteLimit = kSceneBudget};
}

struct RunOutcome final {
    GpuSceneExecutorDiagnosticCode code = GpuSceneExecutorDiagnosticCode::None;
    GpuSceneExecutorPollResult pollResult = GpuSceneExecutorPollResult::Failure;
    std::vector<Rgba32f> pixels;
    // The resident output descriptor, captured before takeImage(), for the descriptor oracle.
    std::optional<ImageWindow> dataWindow;
    std::optional<ImageWindow> displayWindow;
    bloom::core::PixelAspectRatio pixelAspect = bloom::core::PixelAspectRatio::square();
};

[[nodiscard]] RunOutcome
runScene(GpuSceneExecutor& executor,
         const std::shared_ptr<const bloom::runtime::PreparedGpuScene>& scene) {
    RunOutcome outcome;
    outcome.code = executor.begin(scene, kSceneBudget).code;
    if (outcome.code != GpuSceneExecutorDiagnosticCode::None) {
        return outcome;
    }
    for (;;) {
        const auto poll = executor.poll();
        if (poll == GpuSceneExecutorPollResult::Pending) {
            continue;
        }
        outcome.pollResult = poll;
        break;
    }
    if (outcome.pollResult == GpuSceneExecutorPollResult::Ready) {
        const auto* image = executor.image();
        if (image != nullptr) {
            outcome.dataWindow = image->dataWindow();
            outcome.displayWindow = image->displayWindow();
            outcome.pixelAspect = image->pixelAspect();
            const auto readback = readbackResidentImage(*image, kSceneBudget);
            if (readback.hasValue()) {
                outcome.pixels = readback.pixels;
            }
        }
        static_cast<void>(executor.takeImage());
    }
    return outcome;
}

[[nodiscard]] bool parity(const std::vector<Rgba32f>& actual,
                          const std::span<const Rgba32f> expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        for (std::size_t c = 0; c < 4; ++c) {
            const float a = actual[i].components()[c];
            const float e = expected[i].components()[c];
            if (!std::isfinite(a) || !std::isfinite(e)) {
                return false;
            }
            const float scale = std::max(std::abs(a), std::abs(e));
            if (std::abs(a - e) > 2e-6F * std::max(1.0F, scale)) {
                return false;
            }
        }
    }
    return true;
}

// The first child solid's content-addressed key, used to prove the untouched branch stays cached.
[[nodiscard]] std::optional<std::string>
unrelatedBranchKey(const bloom::runtime::PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* solid = std::get_if<GpuSceneSolidCommand>(&command);
            solid != nullptr && solid->sourceOperation.value() == 0) {
            return solid->semanticKey;
        }
    }
    return std::nullopt;
}

void runNested(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    const auto childA = childPlan(1000, 101, Color4d{0.125, 0.375, 0.75, 0.5});
    const auto childB = childPlan(2000, 102, Color4d{0.9, 0.1, 0.05, 1.0});
    const auto parentA = parentPlan(childA, 100);
    const auto parentB = parentPlan(childB, 103);
    const CpuGpuSceneBuilder builder;
    const auto requestA = requestFor(*parentA);
    const auto requestB = requestFor(*parentB);
    const auto preparedA = builder.build(parentA, requestA);
    const auto preparedB = builder.build(parentB, requestB);
    expectations.expect(preparedA.hasValue() && preparedB.hasValue(),
                        "nested native: both parent+child scenes prepare");
    if (!preparedA || !preparedB) {
        std::cerr << "prepare diagnostic: "
                  << (preparedA ? preparedB.diagnostic.message : preparedA.diagnostic.message)
                  << "\n";
        return;
    }

    // CPU oracle for the exact same parent plan.
    const CpuCompositionEvaluator evaluator;
    auto oracleRequest = requestA;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(parentA, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, "nested native: CPU oracle evaluates");
    if (!frame.frame()) {
        return;
    }

    auto executor = GpuSceneExecutor::create(device, cache);
    expectations.expect(executor.hasValue(), "nested native: executor creates");
    if (!executor) {
        return;
    }

    const auto cold = runScene(*executor.executor, preparedA.scene);
    expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready,
                        "nested native: cold run completes: " +
                            executor.executor->diagnostic().message);
    if (cold.pollResult != GpuSceneExecutorPollResult::Ready) {
        return;
    }
    const auto coldCounters = executor.executor->counters();
    expectations.expect(coldCounters.dispatches > 0,
                        "nested native: positive real native dispatch for the child commands");
    expectations.expect(coldCounters.commandsExecuted > 0,
                        "nested native: the executor executed the spliced child commands");
    expectations.expect(coldCounters.readbacks == 0,
                        "nested native: the executor performs no full-frame readback");
    expectations.expect(parity(cold.pixels, frame.frame()->processImage().pixels()),
                        "nested native: GPU pixels match the CPU evaluator (2e-6)");
    const auto& oracleDescriptor = *frame.frame()->processImage().descriptor();
    expectations.expect(cold.dataWindow.has_value() &&
                            *cold.dataWindow == oracleDescriptor.dataWindow() &&
                            cold.displayWindow.has_value() &&
                            *cold.displayWindow == oracleDescriptor.displayWindow() &&
                            cold.pixelAspect == oracleDescriptor.pixelAspect(),
                        "nested native: GPU output descriptor matches the CPU process descriptor");

    // Warm, unchanged nested tree: zero new native dispatch of any family.
    const auto warm = runScene(*executor.executor, preparedA.scene);
    expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready,
                        "nested native: warm run completes");
    const auto warmCounters = executor.executor->counters();
    expectations.expect(warmCounters.dispatches == coldCounters.dispatches,
                        "nested native: a warm unchanged nested tree runs zero new dispatches");
    expectations.expect(warmCounters.commandCacheHits > coldCounters.commandCacheHits,
                        "nested native: the warm run is served from the content cache");
    expectations.expect(parity(warm.pixels, frame.frame()->processImage().pixels()),
                        "nested native: warm pixels still match the CPU oracle");

    // Change ONE child branch. The unrelated branch stays cached; only the changed subtree and its
    // downstream commands dispatch again.
    const auto branchKey = unrelatedBranchKey(*preparedA.scene);
    expectations.expect(branchKey.has_value(), "nested native: the unrelated branch has a key");
    const auto changed = runScene(*executor.executor, preparedB.scene);
    expectations.expect(changed.pollResult == GpuSceneExecutorPollResult::Ready,
                        "nested native: changed-branch run completes");
    const auto changedCounters = executor.executor->counters();
    expectations.expect(changedCounters.dispatches > warmCounters.dispatches,
                        "nested native: the changed branch really dispatches");
    expectations.expect(changedCounters.dispatches - warmCounters.dispatches <
                            coldCounters.dispatches,
                        "nested native: only the changed branch re-dispatches");
    if (branchKey.has_value()) {
        expectations.expect(cache.find(*branchKey) != nullptr,
                            "nested native: the untouched child branch stays cached");
    }
    auto changedRequest = requestB;
    changedRequest.bypassOperationCache = true;
    const auto changedFrame = evaluator.evaluate(parentB, changedRequest, {});
    expectations.expect(changedFrame.frame() != nullptr &&
                            parity(changed.pixels, changedFrame.frame()->processImage().pixels()),
                        "nested native: changed-branch pixels match the CPU oracle");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            std::cerr << "invalid arguments\n";
            return 2;
        }
        Expectations expectations;
        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return 77;
        }
        auto cache = GpuSceneCache::create(*device.device, GpuSceneCacheBudgets{});
        expectations.expect(cache.hasValue(), "nested native: cache creates");
        if (!cache) {
            return 1;
        }
        runNested(expectations, *device.device, *cache.cache);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " nested native expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: GPU nested scene executor vs CPU oracle\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
