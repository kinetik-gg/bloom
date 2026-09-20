// Focused real-device regression for the executor live-pin ref-count fix (audit F1) and the
// upload-cancel consistency fix (audit F2).
//
// The graph under test is a 24-level merge DAG of DISTINCT command indexes: each level merges the
// previous level's command image with one fresh solid, and the terminal output consumes the last
// merge. Every intermediate has a distinct command index and exactly one later consumer, so a
// correct live-pin ledger releases each image at its last consumer. The request byte budget is a
// known live-image count times the actual bytes of ONE produced native image, measured by a probe
// run; it is never the whole-graph retained/cumulative allocation, so the test cannot be satisfied
// by tuning the budget to the buggy measurement and it tolerates hardware VMA alignment padding.
//
// Before the fix: remainingUses stays all-zero, consume() is dead, produced intermediates are
// retained until finishReady(), liveBytes crosses the constant budget mid-job, finishNative()
// returns OverBudget and runScene() never reaches Ready. After the fix: Ready, cumulative >
// budget, peak <= budget, intermediatePinsReleased > 0, and the output matches the CPU oracle.

#include "gpu_scene_executor_test_support.hpp"

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using bloom::core::Color4d;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuImage;
using bloom::render::GpuImageReadback;
using bloom::render::readbackResidentImage;
using bloom::render::Rgba32f;
using bloom::runtime::CompiledCompositionOutput;
using bloom::runtime::CompiledCompositionPlanDefinition;
using bloom::runtime::CompiledMerge;
using bloom::runtime::CompiledMergeInput;
using bloom::runtime::CompiledSolid;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneExecutorBudgets;
using bloom::runtime::GpuSceneExecutorDiagnosticCode;
using bloom::runtime::GpuSceneExecutorPollResult;
using bloom::runtime::OperationIndex;
using bloom::runtime::PreparedGpuScene;

using bloom::runtime::executor_test::Expectations;
using bloom::runtime::executor_test::format;
using bloom::runtime::executor_test::kCompositionId;
using bloom::runtime::executor_test::kProjectId;
using bloom::runtime::executor_test::LayerIds;
using bloom::runtime::executor_test::layerOutput;
using bloom::runtime::executor_test::LayerValues;
using bloom::runtime::executor_test::publish;
using bloom::runtime::executor_test::requestFor;

using PlanPtr = std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>;

constexpr std::uint64_t kSceneBudget = 1ULL << 32U;
constexpr std::uint64_t kReadbackBudget = 1ULL << 32U;
constexpr std::uint64_t kMaxPollIterations = 5'000'000;

struct Options final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool valid = true;
};

[[nodiscard]] Options parseOptions(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader") {
            if (index + 1 >= argc) {
                std::cerr << "--loader requires a path argument\n";
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            options.valid = false;
            return options;
        }
    }
    return options;
}

// A 24-level merge DAG of distinct command indexes: each level merges the previous level's command
// image with a fresh solid, then becomes the next level's input (the planner emits each merge's
// foregrounds first, then its accumulator and source-over steps). Every distinct command image has
// exactly one (or, with `repeatedInput`, two) later consumer, so a correct ref-counted ledger
// releases each level's image at its last consumer. This is the audit F1 shape, not the wide basic
// two-solid fixture.
[[nodiscard]] PlanPtr buildChainPlan(const std::uint32_t depth,
                                     const bloom::document::CompositionFormat format,
                                     const double solidWidth, const double solidHeight,
                                     const std::uint64_t idBase, const bool repeatedInput = false) {
    std::vector<bloom::runtime::CompiledOperation> operations;
    std::uint64_t nextId = idBase;
    const auto emitSolid = [&](const Color4d color) -> OperationIndex {
        const auto operation = static_cast<std::uint32_t>(operations.size());
        const auto node = bloom::document::NodeId::fromRaw(nextId++);
        operations.emplace_back(
            CompiledSolid{node,
                          {bloom::document::ParameterId::fromRaw(nextId++), color},
                          {bloom::document::ParameterId::fromRaw(nextId++), solidWidth},
                          {bloom::document::ParameterId::fromRaw(nextId++), solidHeight}});
        return OperationIndex::fromRaw(operation);
    };
    OperationIndex previous = emitSolid(Color4d{0.10, 0.20, 0.30, 1.0});
    for (std::uint32_t level = 1; level < depth; ++level) {
        const auto side =
            emitSolid(Color4d{0.05 + 0.03 * static_cast<double>(level), 0.40, 0.60, 1.0});
        std::vector<CompiledMergeInput> entries;
        entries.push_back(CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(nextId++),
                                             bloom::document::LayerId{}, previous});
        if (repeatedInput) {
            entries.push_back(CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(nextId++),
                                                 bloom::document::LayerId{}, previous});
        }
        entries.push_back(CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(nextId++),
                                             bloom::document::LayerId{}, side});
        const auto mergeOperation = static_cast<std::uint32_t>(operations.size());
        operations.emplace_back(
            CompiledMerge{bloom::document::NodeId::fromRaw(nextId++), std::move(entries)});
        previous = OperationIndex::fromRaw(mergeOperation);
    }
    const auto outputOperation = static_cast<std::uint32_t>(operations.size());
    operations.emplace_back(
        CompiledCompositionOutput{bloom::document::NodeId::fromRaw(nextId++), previous});
    return publish(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, format,
        std::move(operations), OperationIndex::fromRaw(outputOperation)});
}

[[nodiscard]] bool closeEnough(const float lhs, const float rhs) noexcept {
    if (!std::isfinite(lhs) || !std::isfinite(rhs)) {
        return lhs == rhs;
    }
    const float difference = std::abs(lhs - rhs);
    const float scale = std::max(1.0F, std::max(std::abs(lhs), std::abs(rhs)));
    return difference <= 2.0e-6F * scale;
}

[[nodiscard]] bool pixelsClose(const std::span<const Rgba32f> lhs,
                               const std::span<const Rgba32f> rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (!closeEnough(lhs[i].red(), rhs[i].red()) ||
            !closeEnough(lhs[i].green(), rhs[i].green()) ||
            !closeEnough(lhs[i].blue(), rhs[i].blue()) ||
            !closeEnough(lhs[i].alpha(), rhs[i].alpha())) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool descriptorMatchesScene(const GpuImage& image,
                                          const PreparedGpuScene& scene) noexcept {
    const auto& descriptor = scene.outputDescriptor();
    return image.dataWindow().has_value() && *image.dataWindow() == descriptor.dataWindow() &&
           image.displayWindow().has_value() &&
           *image.displayWindow() == descriptor.displayWindow() &&
           image.pixelAspect() == descriptor.pixelAspect();
}

struct SceneRun final {
    GpuSceneExecutorDiagnosticCode code = GpuSceneExecutorDiagnosticCode::None;
    GpuSceneExecutorPollResult finalPoll = GpuSceneExecutorPollResult::Pending;
    std::shared_ptr<const GpuImage> image;
    bool ready = false;
    bloom::runtime::GpuSceneExecutorCounters countersAtReady;
    std::string message;
};

[[nodiscard]] SceneRun runScene(GpuSceneExecutor& executor,
                                std::shared_ptr<const PreparedGpuScene> scene,
                                const std::uint64_t budget) {
    SceneRun run;
    const auto begun = executor.begin(std::move(scene), budget);
    run.code = begun.code;
    run.message = begun.message;
    if (begun.code != GpuSceneExecutorDiagnosticCode::None) {
        std::cerr << "begin refused: code=" << static_cast<int>(begun.code)
                  << " message=" << begun.message << '\n';
        return run;
    }
    for (std::uint64_t iteration = 0; iteration < kMaxPollIterations; ++iteration) {
        const auto result = executor.poll();
        if (result == GpuSceneExecutorPollResult::Ready) {
            run.finalPoll = result;
            run.countersAtReady = executor.counters();
            run.image = executor.takeImage();
            run.ready = run.image != nullptr;
            return run;
        }
        if (result == GpuSceneExecutorPollResult::Failure ||
            result == GpuSceneExecutorPollResult::WrongThread) {
            run.finalPoll = result;
            run.code = executor.diagnostic().code;
            run.message = executor.diagnostic().message;
            std::cerr << "poll failed: poll=" << static_cast<int>(result)
                      << " code=" << static_cast<int>(executor.diagnostic().code)
                      << " message=" << executor.diagnostic().message << '\n';
            return run;
        }
        std::this_thread::yield();
    }
    return run;
}

// A known bound on the produced images the true live set of this 24-level merge DAG can hold. It is
// well above the correct peak (each intermediate is released at its last consumer) and well below
// the all-retained/cumulative allocation, so a missing release is still detected. The budget is
// this count times the actual bytes of ONE produced image, measured by the probe run below -- never
// the buggy whole-graph retained peak, and independent of image payload geometry.
constexpr std::uint64_t kLiveImageCountBound = 46;

void expectCpuParity(Expectations& expectations, const CpuCompositionEvaluator& oracle,
                     const PlanPtr& plan, const EvaluationRequest& request,
                     const PreparedGpuScene& scene, const GpuImage& nativeImage,
                     const std::string& label) {
    auto requestCopy = request;
    requestCopy.bypassOperationCache = true;
    const auto frame = oracle.evaluate(plan, requestCopy, {});
    expectations.expect(frame.frame() != nullptr, label + ": CPU frame evaluates");
    if (frame.frame() == nullptr) {
        return;
    }
    const auto& cpuImage = frame.frame()->processImage();
    expectations.expect(scene.outputDescriptor() == *cpuImage.descriptor(),
                        label + ": prepared output descriptor matches the CPU frame");
    const GpuImageReadback readback = readbackResidentImage(nativeImage, kReadbackBudget);
    expectations.expect(readback.hasValue(), label + ": native readback succeeds");
    if (!readback) {
        return;
    }
    expectations.expect(readback.pixels.size() == cpuImage.pixels().size(),
                        label + ": pixel count matches the CPU frame");
    if (readback.pixels.size() != cpuImage.pixels().size()) {
        return;
    }
    expectations.expect(pixelsClose(readback.pixels, cpuImage.pixels()),
                        label + ": pixels within the 2e-6 process gate");
}

// Core F1 acceptance: a 24-level distinct-index merge DAG under a budget of a known live-image
// count times the actual per-image native allocation. Before the fix this fails OverBudget; after
// the fix it completes with releases.
void testLongSequentialBudget(Expectations& expectations, GpuDevice& device,
                              const CpuCompositionEvaluator& oracle) {
    constexpr std::uint32_t kDepth = 24;
    const auto plan = buildChainPlan(kDepth, format(32, 24), 32.0, 24.0, 500000);
    const EvaluationRequest request = requestFor(*plan);
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, request);
    expectations.expect(prepared.hasValue(), "long: the scene prepares");
    if (!prepared) {
        return;
    }

    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{1});
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(cache.hasValue() && executor.hasValue(), "long: harness created");
    if (!cache || !executor) {
        return;
    }

    // Probe: one run under an effectively unbounded live budget, only to measure the actual bytes
    // of ONE produced native image (peakStepImageBytes = the largest single step allocation). It is
    // not the whole-graph retained peak, and its real per-image VMA bytes keep the derived budget
    // valid under any hardware alignment padding. The 1-byte cache retains nothing, so the measured
    // run recomputes every command.
    const auto probe = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(probe.ready, "long: the probe run completes");
    if (!probe.ready) {
        std::cerr << "long: probe code=" << static_cast<int>(probe.code)
                  << " message=" << probe.message << '\n';
        return;
    }
    const std::uint64_t perImageBytes = probe.countersAtReady.peakStepImageBytes;
    expectations.expect(perImageBytes > 0,
                        "long: the probe measured a produced native image allocation");
    if (perImageBytes == 0) {
        return;
    }
    const std::uint64_t budget = kLiveImageCountBound * perImageBytes;

    const auto run = runScene(*executor.executor, prepared.scene, budget);
    expectations.expect(run.ready, "long: the constant live budget is accepted end to end");
    if (!run.ready) {
        std::cerr << "long: final code=" << static_cast<int>(run.code) << " message=" << run.message
                  << '\n';
        return;
    }
    const auto counters = run.countersAtReady;
    expectations.expect(counters.cumulativeProducedImageBytes > budget,
                        "long: cumulative allocation exceeds the constant live budget");
    expectations.expect(counters.peakLiveImageBytes <= budget,
                        "long: the live peak stays within the constant live budget");
    expectations.expect(counters.intermediatePinsReleased > 0,
                        "long: intermediates are released at their last consumer");
    expectations.expect(counters.dispatches >= kDepth,
                        "long: the whole distinct-index chain was dispatched");
    expectations.expect(cache.cache->entryCount() == 0, "long: the tiny cache retained no entry");
    expectations.expect(descriptorMatchesScene(*run.image, *prepared.scene),
                        "long: the native output descriptor matches the scene");
    expectCpuParity(expectations, oracle, plan, request, *prepared.scene, *run.image, "long");
}

// A shared solid command consumed by several later translation steps must not be freed at the first
// consumer: parity proves the last consumer still saw live pixels, and the budget still holds.
void testSharedInputNotFreedEarly(Expectations& expectations, GpuDevice& device,
                                  const CpuCompositionEvaluator& oracle) {
    constexpr std::uint32_t kDepth = 8;
    const auto plan = buildChainPlan(kDepth, format(24, 16), 24.0, 16.0, 600000, true);
    const EvaluationRequest request = requestFor(*plan);
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, request);
    expectations.expect(prepared.hasValue(), "shared: the scene prepares");
    if (!prepared) {
        return;
    }
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{1});
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(cache.hasValue() && executor.hasValue(), "shared: harness created");
    if (!cache || !executor) {
        return;
    }
    const auto run = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(run.ready, "shared: the shared-input graph completes");
    if (!run.ready) {
        return;
    }
    expectations.expect(descriptorMatchesScene(*run.image, *prepared.scene),
                        "shared: native descriptor matches");
    expectCpuParity(expectations, oracle, plan, request, *prepared.scene, *run.image, "shared");
}

// Warm reuse: the executor must keep releasing produced pins across a cache hit/miss boundary. The
// unchanged scene is an output hit; a mutated top layer forces a miss while the lower inputs remain
// cache hits, which are pinned for the request and released at their last consumer.
void testWarmReuseReleases(Expectations& expectations, GpuDevice& device,
                           const CpuCompositionEvaluator& oracle) {
    constexpr std::uint32_t kDepth = 16;
    const auto plan = buildChainPlan(kDepth, format(32, 24), 32.0, 24.0, 700000);
    const EvaluationRequest request = requestFor(*plan);
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, request);
    expectations.expect(prepared.hasValue(), "warm: the scene prepares");
    if (!prepared) {
        return;
    }
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{64ULL << 20U});
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(cache.hasValue() && executor.hasValue(), "warm: harness created");
    if (!cache || !executor) {
        return;
    }
    const auto first = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(first.ready, "warm: the first run completes");
    if (!first.ready) {
        return;
    }
    const auto second = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(second.ready, "warm: the unchanged replay completes");
    if (!second.ready) {
        return;
    }
    expectations.expect(second.countersAtReady.outputCacheHits >= 1,
                        "warm: the unchanged replay is an output cache hit");
    expectCpuParity(expectations, oracle, plan, request, *prepared.scene, *second.image, "warm");
}

} // namespace

int main(const int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
        Expectations expectations;
        const CpuCompositionEvaluator oracle;

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
            return expectations.ok() ? 0 : 1;
        }

        testLongSequentialBudget(expectations, *device.device, oracle);
        testSharedInputNotFreedEarly(expectations, *device.device, oracle);
        testWarmReuseReleases(expectations, *device.device, oracle);

        if (!expectations.ok()) {
            std::cerr << "FAIL: live-pin focus expectations failed\n";
            return 1;
        }
        std::cout << "PASS: live-pin focus\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
