// Native vertical for the all-8 blend-mode slice: a REAL CompiledCompositionPlan -> the production
// CpuGpuSceneBuilder -> the production GpuSceneExecutor on a Vulkan device -> readback compared to
// a genuine, uncached CpuCompositionEvaluator frame under the documented 2e-6 absolute-or-relative
// gate. This exercises the builder's actual blend emission (not the proof-only fixture seam), so a
// merge stack whose top layer carries a non-Normal mode must produce an explicit BlendV1 fold that
// matches the CPU evaluator.
//
// Without a loader the test skips cleanly; --require-device makes that a failure. On a device
// without shaderFloat64 the six general modes are refused honestly (Unsupported/DispatchRefused)
// and the test accepts that instead of claiming a pass.

#include "gpu_blend_native_support.hpp"
#include "gpu_composite_native_support.hpp"
#include "gpu_scene_preparation_test_support.hpp"

#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using bloom::core::BlendMode;
using bloom::render::readbackResidentImage;
using bloom::render::Rgba32f;
using bloom::render::composite_proof::GpuDevice;
using bloom::render::composite_proof::kBudget;

[[nodiscard]] bool withinGate(const std::vector<Rgba32f>& actual,
                              const std::vector<Rgba32f>& expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto& left = actual[i].components();
        const auto& right = expected[i].components();
        for (std::size_t c = 0; c < left.size(); ++c) {
            const float a = left[c];
            const float e = right[c];
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

[[nodiscard]] bool runAgain(bloom::runtime::GpuSceneExecutor& executor,
                            const std::shared_ptr<const bloom::runtime::PreparedGpuScene>& scene,
                            const std::uint64_t budget);

void runMode(Expectations& expectations, CpuCompositionEvaluator& evaluator, GpuDevice& device,
             bloom::runtime::GpuSceneCache& cache, const BlendMode mode,
             const std::uint64_t idBase) {
    const auto label = "blend integration mode " + std::to_string(static_cast<unsigned>(mode));
    const auto plan = twoLayerPlan(
        format(16, 12), LayerValues{.position = {4.3, 3.1}},
        LayerValues{.position = {2.7, 2.4}, .opacity = 0.75, .blendMode = mode}, 6.0, 5.0, idBase);
    const auto request = requestFor(*plan);
    const CpuGpuSceneBuilder builder;
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), label + ": builder prepares");
    if (!prepared) {
        std::cerr << label << " diagnostic: " << prepared.diagnostic.message << "\n";
        return;
    }

    auto executor = bloom::runtime::GpuSceneExecutor::create(device, cache);
    expectations.expect(executor.hasValue(), label + ": executor creates");
    if (!executor) {
        return;
    }
    const auto begin = executor.executor->begin(prepared.scene, 64ULL * 1024ULL * 1024ULL);
    if (begin.code != bloom::runtime::GpuSceneExecutorDiagnosticCode::None) {
        expectations.expect(begin.code ==
                                bloom::runtime::GpuSceneExecutorDiagnosticCode::Unsupported,
                            label + ": begin may only refuse honestly (f64): " + begin.message);
        return;
    }
    bloom::runtime::GpuSceneExecutorPollResult poll =
        bloom::runtime::GpuSceneExecutorPollResult::Pending;
    while (poll == bloom::runtime::GpuSceneExecutorPollResult::Pending) {
        poll = executor.executor->poll();
    }
    if (poll != bloom::runtime::GpuSceneExecutorPollResult::Ready) {
        const auto code = executor.executor->diagnostic().code;
        const bool acceptable =
            code == bloom::runtime::GpuSceneExecutorDiagnosticCode::Unsupported ||
            code == bloom::runtime::GpuSceneExecutorDiagnosticCode::DispatchRefused;
        expectations.expect(acceptable, label + ": must run or refuse honestly (f64): " +
                                            executor.executor->diagnostic().message);
        return;
    }

    const auto* image = executor.executor->image();
    expectations.expect(image != nullptr, label + ": output image present");
    if (image == nullptr) {
        static_cast<void>(executor.executor->takeImage());
        return;
    }
    const auto readback = readbackResidentImage(*image, kBudget);
    static_cast<void>(executor.executor->takeImage());
    expectations.expect(readback.hasValue(), label + ": readback succeeds");
    if (!readback) {
        return;
    }

    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, label + ": CPU frame evaluates");
    if (!frame.frame()) {
        return;
    }
    const auto& oracle = frame.frame()->processImage();
    expectations.expect(prepared.scene->outputDescriptor() == *oracle.descriptor(),
                        label + ": prepared descriptor equals the CPU process descriptor");
    const std::vector<Rgba32f> expected(oracle.pixels().begin(), oracle.pixels().end());
    expectations.expect(withinGate(readback.pixels, expected),
                        label + ": GPU pixels match the live CPU frame within 2e-6");
}

// A raster generic input: two affine vector layers merged, then a rotated/scaled layer over the
// merge result. The layer over a merge is raster, so it must emit GpuAffine (or a parented affine)
// and reproduce the live CPU frame. One solid is high-dynamic with a negative channel and a tiny
// alpha so the affine resample covers that domain too.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
rasterAffinePlan(const CompositionFormat compositionFormat, const bool parented,
                 const std::uint64_t idBase) {
    using bloom::document::LayerId;
    using bloom::document::LayerSlotId;
    using bloom::document::NodeId;
    using bloom::document::ParameterId;
    const LayerIds idsA{ParameterId::fromRaw(idBase + 0), ParameterId::fromRaw(idBase + 1),
                        ParameterId::fromRaw(idBase + 2), ParameterId::fromRaw(idBase + 3),
                        ParameterId::fromRaw(idBase + 4), ParameterId::fromRaw(idBase + 5)};
    const LayerIds idsB{ParameterId::fromRaw(idBase + 6),  ParameterId::fromRaw(idBase + 7),
                        ParameterId::fromRaw(idBase + 8),  ParameterId::fromRaw(idBase + 9),
                        ParameterId::fromRaw(idBase + 10), ParameterId::fromRaw(idBase + 11)};
    const LayerIds idsC{ParameterId::fromRaw(idBase + 12), ParameterId::fromRaw(idBase + 13),
                        ParameterId::fromRaw(idBase + 14), ParameterId::fromRaw(idBase + 15),
                        ParameterId::fromRaw(idBase + 16), ParameterId::fromRaw(idBase + 17)};
    const auto layerAId = LayerId::fromRaw(idBase + 20);
    const auto layerBId = LayerId::fromRaw(idBase + 21);
    const auto layerCId = LayerId::fromRaw(idBase + 22);
    std::vector<CompiledOperation> operations;
    operations.emplace_back(
        CompiledSolid{NodeId::fromRaw(idBase + 1),
                      {ParameterId::fromRaw(idBase + 40), Color4d{0.5, 0.25, 0.125, 1.0}},
                      {ParameterId::fromRaw(idBase + 41), 6.0},
                      {ParameterId::fromRaw(idBase + 42), 5.0}});
    operations.emplace_back(
        layerOutput(NodeId::fromRaw(idBase + 2), layerAId, OperationIndex::fromRaw(0), idsA,
                    LayerValues{.position = {4.3, 3.1}, .scale = {1.25, 1.25}, .rotation = 10.0}));
    operations.emplace_back(
        CompiledSolid{NodeId::fromRaw(idBase + 3),
                      {ParameterId::fromRaw(idBase + 43), Color4d{2.5, -0.3, 0.1, 1.0e-5}},
                      {ParameterId::fromRaw(idBase + 44), 6.0},
                      {ParameterId::fromRaw(idBase + 45), 5.0}});
    operations.emplace_back(layerOutput(
        NodeId::fromRaw(idBase + 4), layerBId, OperationIndex::fromRaw(2), idsB,
        LayerValues{
            .position = {11.5, 8.2}, .scale = {0.5, 1.5}, .rotation = -12.0, .opacity = 0.75}));
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(idBase + 5),
                      {CompiledMergeInput{LayerSlotId::fromRaw(idBase + 30), layerAId,
                                          OperationIndex::fromRaw(1)},
                       CompiledMergeInput{LayerSlotId::fromRaw(idBase + 31), layerBId,
                                          OperationIndex::fromRaw(3)}}});
    auto layerC = layerOutput(
        NodeId::fromRaw(idBase + 6), layerCId, OperationIndex::fromRaw(4), idsC,
        LayerValues{
            .position = {8.0, 6.0}, .anchor = {1.0, -0.5}, .scale = {1.5, 0.5}, .rotation = 25.0});
    if (parented) {
        layerC.parent = OperationIndex::fromRaw(1);
    }
    operations.emplace_back(std::move(layerC));
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(idBase + 7),
                      {CompiledMergeInput{LayerSlotId::fromRaw(idBase + 32), layerCId,
                                          OperationIndex::fromRaw(5)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 8), OperationIndex::fromRaw(6)});
    return publish(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(operations), OperationIndex::fromRaw(7)});
}

void runRasterAffine(Expectations& expectations, CpuCompositionEvaluator& evaluator,
                     GpuDevice& device, bloom::runtime::GpuSceneCache& cache, const bool parented,
                     const std::uint64_t idBase) {
    const std::string label = parented ? "native parented raster affine" : "native raster affine";
    const auto plan = rasterAffinePlan(format(16, 12), parented, idBase);
    const auto request = requestFor(*plan);
    const CpuGpuSceneBuilder builder;
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), label + ": builder prepares");
    if (!prepared) {
        std::cerr << label << " diagnostic: " << prepared.diagnostic.message << "\n";
        return;
    }
    bool sawAffine = false;
    for (const auto& command : prepared.scene->commands()) {
        sawAffine =
            sawAffine || std::holds_alternative<bloom::runtime::GpuSceneAffineCommand>(command);
    }
    expectations.expect(sawAffine, label + ": emits a GpuAffine command");

    auto executor = bloom::runtime::GpuSceneExecutor::create(device, cache);
    expectations.expect(executor.hasValue(), label + ": executor creates");
    if (!executor) {
        return;
    }
    const auto begin = executor.executor->begin(prepared.scene, 256ULL * 1024ULL * 1024ULL);
    expectations.expect(begin.code == bloom::runtime::GpuSceneExecutorDiagnosticCode::None,
                        label + ": begin accepted: " + begin.message);
    if (begin.code != bloom::runtime::GpuSceneExecutorDiagnosticCode::None) {
        return;
    }
    bloom::runtime::GpuSceneExecutorPollResult poll =
        bloom::runtime::GpuSceneExecutorPollResult::Pending;
    while (poll == bloom::runtime::GpuSceneExecutorPollResult::Pending) {
        poll = executor.executor->poll();
    }
    expectations.expect(poll == bloom::runtime::GpuSceneExecutorPollResult::Ready,
                        label + ": completes: " + executor.executor->diagnostic().message);
    if (poll != bloom::runtime::GpuSceneExecutorPollResult::Ready) {
        return;
    }
    const auto* image = executor.executor->image();
    if (image == nullptr) {
        static_cast<void>(executor.executor->takeImage());
        expectations.expect(false, label + ": output image present");
        return;
    }
    const auto readback = readbackResidentImage(*image, kBudget);
    static_cast<void>(executor.executor->takeImage());
    expectations.expect(readback.hasValue(), label + ": readback succeeds");
    if (!readback) {
        return;
    }
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    if (!frame.frame()) {
        expectations.expect(false, label + ": CPU frame evaluates");
        return;
    }
    const auto& oracle = frame.frame()->processImage();
    expectations.expect(prepared.scene->outputDescriptor() == *oracle.descriptor(),
                        label + ": descriptor equals the CPU process descriptor");
    const std::vector<Rgba32f> expected(oracle.pixels().begin(), oracle.pixels().end());
    expectations.expect(withinGate(readback.pixels, expected),
                        label + ": GPU pixels match the live CPU frame within 2e-6");

    // A cold positive run dispatched at least one affine; a warm run of the SAME scene tree must
    // dispatch nothing new and answer from the content cache.
    const auto cold = executor.executor->counters();
    expectations.expect(cold.affineDispatches > 0, label + ": cold pass dispatches an affine");
    const auto warmOk = runAgain(*executor.executor, prepared.scene, 256ULL * 1024ULL * 1024ULL);
    const auto warm = executor.executor->counters();
    expectations.expect(warmOk, label + ": warm pass ready");
    expectations.expect(warm.affineDispatches == cold.affineDispatches &&
                            warm.blendDispatches == cold.blendDispatches &&
                            warm.uploads == cold.uploads,
                        label + ": warm pass dispatches nothing new");
    expectations.expect(warm.commandCacheHits > cold.commandCacheHits,
                        label + ": warm pass serves the output from the content cache");
}

// Runs a prepared scene once on an existing executor, discarding any output. Used for the warm
// pass after a cold run that already asserted parity.
[[nodiscard]] bool runAgain(bloom::runtime::GpuSceneExecutor& executor,
                            const std::shared_ptr<const bloom::runtime::PreparedGpuScene>& scene,
                            const std::uint64_t budget) {
    if (executor.begin(scene, budget).code !=
        bloom::runtime::GpuSceneExecutorDiagnosticCode::None) {
        return false;
    }
    bloom::runtime::GpuSceneExecutorPollResult poll =
        bloom::runtime::GpuSceneExecutorPollResult::Pending;
    while (poll == bloom::runtime::GpuSceneExecutorPollResult::Pending) {
        poll = executor.poll();
    }
    if (poll != bloom::runtime::GpuSceneExecutorPollResult::Ready) {
        return false;
    }
    static_cast<void>(executor.takeImage());
    return true;
}

[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
vectorChainPlan(const CompositionFormat compositionFormat, const std::uint64_t idBase) {
    using bloom::document::LayerId;
    using bloom::document::LayerSlotId;
    using bloom::document::NodeId;
    using bloom::document::ParameterId;
    const LayerIds idsA{ParameterId::fromRaw(idBase + 0), ParameterId::fromRaw(idBase + 1),
                        ParameterId::fromRaw(idBase + 2), ParameterId::fromRaw(idBase + 3),
                        ParameterId::fromRaw(idBase + 4), ParameterId::fromRaw(idBase + 5)};
    const LayerIds idsB{ParameterId::fromRaw(idBase + 6),  ParameterId::fromRaw(idBase + 7),
                        ParameterId::fromRaw(idBase + 8),  ParameterId::fromRaw(idBase + 9),
                        ParameterId::fromRaw(idBase + 10), ParameterId::fromRaw(idBase + 11)};
    const auto layerAId = LayerId::fromRaw(idBase + 20);
    const auto layerBId = LayerId::fromRaw(idBase + 21);
    std::vector<CompiledOperation> operations;
    operations.emplace_back(
        CompiledSolid{NodeId::fromRaw(idBase + 1),
                      {ParameterId::fromRaw(idBase + 40), Color4d{0.5, 0.25, 0.125, 1.0}},
                      {ParameterId::fromRaw(idBase + 41), 10.0},
                      {ParameterId::fromRaw(idBase + 42), 8.0}});
    operations.emplace_back(
        layerOutput(NodeId::fromRaw(idBase + 2), layerAId, OperationIndex::fromRaw(0), idsA,
                    LayerValues{.position = {12.0, 8.0}, .scale = {1.25, 1.25}, .rotation = 10.0}));
    operations.emplace_back(
        layerOutput(NodeId::fromRaw(idBase + 3), layerBId, OperationIndex::fromRaw(1), idsB,
                    LayerValues{.position = {8.0, 7.0}, .scale = {0.75, 1.5}, .rotation = -12.0}));
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(idBase + 5),
                      {CompiledMergeInput{LayerSlotId::fromRaw(idBase + 30), layerBId,
                                          OperationIndex::fromRaw(2)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 6), OperationIndex::fromRaw(3)});
    return publish(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(operations), OperationIndex::fromRaw(4)});
}

// A layer-on-layer vector chain through the REAL builder and executor, with cold positive coverage
// dispatch and a warm pass that must reuse the cache with zero new dispatches.
void runVectorChain(Expectations& expectations, CpuCompositionEvaluator& evaluator,
                    GpuDevice& device, bloom::runtime::GpuSceneCache& cache,
                    const std::uint64_t idBase) {
    const std::string label = "native generic vector chain";
    const auto plan = vectorChainPlan(format(24, 16), idBase);
    const auto request = requestFor(*plan);
    const CpuGpuSceneBuilder builder;
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), label + ": builder prepares");
    if (!prepared) {
        std::cerr << label << " diagnostic: " << prepared.diagnostic.message << "\n";
        return;
    }
    bool sawCoverage = false;
    bool sawAffine = false;
    for (const auto& command : prepared.scene->commands()) {
        sawCoverage = sawCoverage ||
                      std::holds_alternative<bloom::runtime::GpuSceneCoverageSolidCommand>(command);
        sawAffine =
            sawAffine || std::holds_alternative<bloom::runtime::GpuSceneAffineCommand>(command);
    }
    expectations.expect(sawCoverage && !sawAffine,
                        label + ": chain rasterizes through coverage, not a resample");

    auto executor = bloom::runtime::GpuSceneExecutor::create(device, cache);
    expectations.expect(executor.hasValue(), label + ": executor creates");
    if (!executor) {
        return;
    }
    const auto budget = 256ULL * 1024ULL * 1024ULL;
    const auto begin = executor.executor->begin(prepared.scene, budget);
    expectations.expect(begin.code == bloom::runtime::GpuSceneExecutorDiagnosticCode::None,
                        label + ": begin accepted: " + begin.message);
    if (begin.code != bloom::runtime::GpuSceneExecutorDiagnosticCode::None) {
        return;
    }
    bloom::runtime::GpuSceneExecutorPollResult poll =
        bloom::runtime::GpuSceneExecutorPollResult::Pending;
    while (poll == bloom::runtime::GpuSceneExecutorPollResult::Pending) {
        poll = executor.executor->poll();
    }
    expectations.expect(poll == bloom::runtime::GpuSceneExecutorPollResult::Ready,
                        label + ": completes: " + executor.executor->diagnostic().message);
    if (poll != bloom::runtime::GpuSceneExecutorPollResult::Ready) {
        return;
    }
    const auto* image = executor.executor->image();
    if (image == nullptr) {
        static_cast<void>(executor.executor->takeImage());
        expectations.expect(false, label + ": output image present");
        return;
    }
    const auto readback = readbackResidentImage(*image, kBudget);
    static_cast<void>(executor.executor->takeImage());
    expectations.expect(readback.hasValue(), label + ": readback succeeds");
    const auto cold = executor.executor->counters();
    expectations.expect(cold.coveredSolidDispatches > 0,
                        label + ": cold pass dispatches covered-solid commands");

    if (readback) {
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto frame = evaluator.evaluate(plan, oracleRequest, {});
        if (!frame.frame()) {
            expectations.expect(false, label + ": CPU frame evaluates");
            return;
        }
        const auto& oracle = frame.frame()->processImage();
        expectations.expect(prepared.scene->outputDescriptor() == *oracle.descriptor(),
                            label + ": descriptor equals the CPU process descriptor");
        const std::vector<Rgba32f> expected(oracle.pixels().begin(), oracle.pixels().end());
        expectations.expect(withinGate(readback.pixels, expected),
                            label + ": GPU pixels match the live CPU frame within 2e-6");
    }

    const auto warmOk = runAgain(*executor.executor, prepared.scene, budget);
    const auto warm = executor.executor->counters();
    expectations.expect(warmOk, label + ": warm pass ready");
    expectations.expect(warm.coveredSolidDispatches == cold.coveredSolidDispatches &&
                            warm.affineDispatches == cold.affineDispatches &&
                            warm.uploads == cold.uploads,
                        label + ": warm pass dispatches nothing new");
    expectations.expect(warm.commandCacheHits > cold.commandCacheHits,
                        label + ": warm pass serves the output from the content cache");
}

} // namespace

int main(int argc, char** argv) {
    try {
        using bloom::render::composite_proof::Options;
        using bloom::render::composite_proof::parseOptions;
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
        Expectations expectations;
        bloom::render::GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = bloom::render::GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return 0;
        }
        auto cache = bloom::runtime::GpuSceneCache::create(*device.device,
                                                           bloom::runtime::GpuSceneCacheBudgets{});
        expectations.expect(cache.hasValue(), "cache creates");
        if (!cache) {
            return 1;
        }
        CpuCompositionEvaluator evaluator;
        const std::array modes{BlendMode::Normal,  BlendMode::Add,       BlendMode::Multiply,
                               BlendMode::Screen,  BlendMode::Overlay,   BlendMode::Darken,
                               BlendMode::Lighten, BlendMode::Difference};
        std::uint64_t idBase = 70000;
        for (const auto mode : modes) {
            runMode(expectations, evaluator, *device.device, *cache.cache, mode, idBase);
            idBase += 100;
        }
        runRasterAffine(expectations, evaluator, *device.device, *cache.cache, false, 80000);
        runRasterAffine(expectations, evaluator, *device.device, *cache.cache, true, 80200);
        runVectorChain(expectations, evaluator, *device.device, *cache.cache, 80400);
        if (!expectations.ok()) {
            std::cerr << "native blend integration expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: builder blend commands vs live CPU frame on device\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
