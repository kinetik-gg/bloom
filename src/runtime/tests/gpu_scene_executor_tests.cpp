// Real-device proof for the owner-thread non-blocking GPU scene executor.
//
// Every fixture is a real CompiledCompositionPlan -> CpuGpuSceneBuilder -> GpuSceneExecutor on a
// real Vulkan device, compared to a genuine uncached CpuCompositionEvaluator frame for every output
// pixel and the actual returned native descriptor. The image is read back only by this test oracle;
// the executor itself never reads back.
//
// Gate: absolute-or-relative 2e-6 per finite component, with byte-exact comparison where a
// transparent-backed covered fill is the only contributor (the covered fill is bit-exact by
// construction).

#include "gpu_scene_executor_test_support.hpp"

#include <bloom/render/gpu_composite.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>

// The fault-injection hook is proof-only. A main build that does not define
// BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION (and does not ship this header) still compiles and
// runs the parity/cache/live-budget tests; only the retirement fault tests are skipped.
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
#include "gpu_scene_executor_fault_injection.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
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
using bloom::core::PixelAspectRatio;
using bloom::core::RationalTime;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuImage;
using bloom::render::GpuImageReadback;
using bloom::render::GpuSolid;
using bloom::render::GpuSolidParameters;
using bloom::render::GpuSolidPollResult;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::readbackResidentImage;
using bloom::runtime::CompiledLayerOutput;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneCacheCreateResult;
using bloom::runtime::GpuSceneCommand;
using bloom::runtime::GpuSceneCompositionOutputCommand;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneExecutorBudgets;
using bloom::runtime::GpuSceneExecutorCounters;
using bloom::runtime::GpuSceneExecutorCreateResult;
using bloom::runtime::GpuSceneExecutorDiagnostic;
using bloom::runtime::GpuSceneExecutorDiagnosticCode;
using bloom::runtime::GpuSceneExecutorJobState;
using bloom::runtime::GpuSceneExecutorPollResult;
using bloom::runtime::GpuSceneMergeCommand;
using bloom::runtime::GpuSceneSolidCommand;
using bloom::runtime::GpuSceneTranslationCommand;
using bloom::runtime::PreparedGpuScene;
using bloom::runtime::ProxyResolution;

using bloom::runtime::executor_test::Expectations;
using bloom::runtime::executor_test::format;
using bloom::runtime::executor_test::LayerValues;
using bloom::runtime::executor_test::publish;
using bloom::runtime::executor_test::requestFor;
using bloom::runtime::executor_test::twoLayerPlan;
using bloom::runtime::executor_test::twoSolidPlan;

using PlanPtr = std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>;

constexpr std::uint64_t kSceneBudget = 1ULL << 32U;
constexpr std::uint64_t kReadbackBudget = 1ULL << 32U;
constexpr std::uint64_t kCacheBudget = 1ULL << 32U;
constexpr std::uint64_t kMaxPollIterations = 5'000'000;

struct Options final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool benchmark = false;
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
        } else if (argument == "--benchmark") {
            options.benchmark = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            options.valid = false;
            return options;
        }
    }
    return options;
}

[[nodiscard]] const std::string& keyOf(const GpuSceneCommand& command) {
    return std::visit([](const auto& item) -> const std::string& { return item.semanticKey; },
                      command);
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

[[nodiscard]] bool pixelsExact(const std::span<const Rgba32f> lhs,
                               const std::span<const Rgba32f> rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    return std::memcmp(lhs.data(), rhs.data(), lhs.size() * sizeof(Rgba32f)) == 0;
}

struct SceneRun final {
    GpuSceneExecutorDiagnostic begin;
    GpuSceneExecutorPollResult finalPoll = GpuSceneExecutorPollResult::Pending;
    std::shared_ptr<const GpuImage> image;
    bool ready = false;
    // Captured before takeImage(), which resets the per-request live/peak ledger.
    GpuSceneExecutorCounters countersAtReady;
};

[[nodiscard]] SceneRun runScene(GpuSceneExecutor& executor,
                                std::shared_ptr<const PreparedGpuScene> scene,
                                const std::uint64_t budget) {
    SceneRun run;
    run.begin = executor.begin(std::move(scene), budget);
    if (run.begin.code != GpuSceneExecutorDiagnosticCode::None) {
        std::cerr << "executor begin refused: code="
                  << static_cast<int>(run.begin.code) << " message=" << run.begin.message << '\n';
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
            std::cerr << "executor failed: poll=" << static_cast<int>(result)
                      << " code=" << static_cast<int>(executor.diagnostic().code)
                      << " message=" << executor.diagnostic().message << '\n';
            return run;
        }
        std::this_thread::yield();
    }
    return run;
}

[[nodiscard]] bool descriptorMatchesScene(const GpuImage& image,
                                          const PreparedGpuScene& scene) noexcept {
    const auto& descriptor = scene.outputDescriptor();
    return image.dataWindow().has_value() && *image.dataWindow() == descriptor.dataWindow() &&
           image.displayWindow().has_value() &&
           *image.displayWindow() == descriptor.displayWindow() &&
           image.pixelAspect() == descriptor.pixelAspect();
}

// Runs a real plan through the builder, the executor and the CPU oracle, comparing every pixel and
// the actual native descriptor.
void expectParity(Expectations& expectations, GpuSceneExecutor& executor,
                  const CpuCompositionEvaluator& oracle, const PlanPtr& plan,
                  const EvaluationRequest& request, const std::string& label,
                  const bool exact = false, const bool expectCovered = false) {
    const CpuGpuSceneBuilder builder;
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), label + ": prepares");
    if (!prepared) {
        return;
    }
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = oracle.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, label + ": CPU frame evaluates");
    if (frame.frame() == nullptr) {
        return;
    }
    const auto& cpuImage = frame.frame()->processImage();
    expectations.expect(prepared.scene->outputDescriptor() == *cpuImage.descriptor(),
                        label + ": prepared output descriptor matches the CPU frame");

    const auto run = runScene(executor, prepared.scene, kSceneBudget);
    expectations.expect(run.begin.code == GpuSceneExecutorDiagnosticCode::None,
                        label + ": executor accepts the scene");
    expectations.expect(run.ready, label + ": executor reaches Ready");
    if (!run.ready || run.image == nullptr) {
        return;
    }
    expectations.expect(descriptorMatchesScene(*run.image, *prepared.scene),
                        label + ": actual native descriptor matches the scene");
    const GpuImageReadback readback = readbackResidentImage(*run.image, kReadbackBudget);
    expectations.expect(readback.hasValue(), label + ": test readback succeeds");
    if (!readback) {
        return;
    }
    expectations.expect(readback.pixels.size() == cpuImage.pixels().size(),
                        label + ": pixel count matches the CPU frame");
    if (readback.pixels.size() != cpuImage.pixels().size()) {
        return;
    }
    if (exact) {
        expectations.expect(pixelsExact(readback.pixels, cpuImage.pixels()),
                            label + ": pixels are byte-exact");
    } else {
        expectations.expect(pixelsClose(readback.pixels, cpuImage.pixels()),
                            label + ": pixels are within the 2e-6 process gate");
    }
    if (expectCovered) {
        expectations.expect(executor.counters().coveredSolidDispatches > 0,
                            label + ": the covered-solid native path was used");
    }
}

// ---- fixtures ----------------------------------------------------------------------------------

[[nodiscard]] PlanPtr basicPlan() {
    return twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}, .opacity = 1.0},
                        LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0, 1000);
}

[[nodiscard]] PlanPtr fractionalPlan() {
    return twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                        LayerValues{.position = {2.7, 2.4}}, 6.0, 5.0, 12000);
}

// ---- individual tests --------------------------------------------------------------------------

void testFixtures(Expectations& expectations, GpuSceneExecutor& executor,
                  const CpuCompositionEvaluator& oracle) {
    expectParity(expectations, executor, oracle, basicPlan(), requestFor(*basicPlan()),
                 "basic 2D nonuniform merged solids");
    expectParity(expectations, executor, oracle, fractionalPlan(), requestFor(*fractionalPlan()),
                 "fractional +0.3/-0.3 coverage", false, true);

    const auto integerPlan =
        twoLayerPlan(format(16, 12), LayerValues{.position = {4.0, 3.5}, .opacity = 1.0},
                     LayerValues{.position = {9.0, 7.5}, .opacity = 0.5}, 6.0, 5.0, 11000);
    const auto preparedInteger =
        CpuGpuSceneBuilder{}.build(integerPlan, requestFor(*integerPlan));
    expectations.expect(preparedInteger.hasValue(), "integer grid plan prepares");
    if (preparedInteger) {
        bool sawTranslation = false;
        for (const auto& command : preparedInteger.scene->commands()) {
            sawTranslation =
                sawTranslation || std::holds_alternative<GpuSceneTranslationCommand>(command);
        }
        expectations.expect(sawTranslation, "the integer grid uses the native translation command");
    }
    expectParity(expectations, executor, oracle, integerPlan, requestFor(*integerPlan),
                 "integer native grid");

    const auto hdrPlan =
        twoSolidPlan(format(16, 12), Color4d{4.0, -0.5, 2.0, 0.5},
                     LayerValues{.position = {5.3, 4.1}}, Color4d{-1.0, 3.0, 0.25, 0.75},
                     LayerValues{.position = {8.7, 6.2}, .opacity = 0.8}, 6.0, 5.0, 22000);
    expectParity(expectations, executor, oracle, hdrPlan, requestFor(*hdrPlan), "HDR signed alpha",
                 false, true);

    const auto oddPlan =
        twoLayerPlan(format(7, 5), LayerValues{.position = {2.3, 1.4}},
                     LayerValues{.position = {-0.7, 3.1}, .opacity = 0.6}, 3.0, 2.0, 23000);
    expectParity(expectations, executor, oracle, oddPlan, requestFor(*oddPlan),
                 "odd nonzero-origin windows", false, true);

    const auto proxyFormat = format(9, 6, *PixelAspectRatio::create(4, 3));
    const auto proxyPlan = twoLayerPlan(
        proxyFormat, LayerValues{.position = {4.5, 3.0}, .opacity = 1.0},
        LayerValues{.position = {2.7, 4.9}, .anchor = {1.0, -0.5}, .opacity = 0.5}, 4.0, 3.0, 2000);
    const auto extent = bloom::render::ImageExtent::create(5, 4);
    expectations.expect(static_cast<bool>(extent), "the proxy extent builds");
    if (extent) {
        auto request = requestFor(*proxyPlan);
        request.resolution = ProxyResolution{*extent.value()};
        expectParity(expectations, executor, oracle, proxyPlan, request,
                     "proxy with non-square PAR");
    }

    const auto transparentTop = twoLayerPlan(
        format(16, 12), LayerValues{.position = {4.3, 3.1}, .opacity = 0.0},
        LayerValues{.position = {2.7, 2.4}, .opacity = 1.0}, 6.0, 5.0, 13000);
    expectParity(expectations, executor, oracle, transparentTop, requestFor(*transparentTop),
                 "opacity endpoints 0/1");
}

void testEmptyInactive(Expectations& expectations, GpuSceneExecutor& executor,
                       const CpuCompositionEvaluator& oracle) {
    auto definition = basicPlan()->copyDefinition();
    std::get<CompiledLayerOutput>(definition.operations[1]).inPoint = RationalTime::fromInteger(2);
    std::get<CompiledLayerOutput>(definition.operations[3]).inPoint = RationalTime::fromInteger(2);
    const auto plan = publish(std::move(definition));
    expectParity(expectations, executor, oracle, plan,
                 requestFor(*plan, RationalTime::fromInteger(0)),
                 "empty/inactive after the builder gate", true);
}

// A fractional covered bottom layer with a fully transparent top layer: the merge reduces to the
// covered fill exactly, so the output is byte-exact.
void testCoveredByteExact(Expectations& expectations, GpuSceneExecutor& executor,
                          const CpuCompositionEvaluator& oracle) {
    const auto plan = twoSolidPlan(format(16, 12), Color4d{0.4, 0.6, 0.2, 0.5},
                                   LayerValues{.position = {4.3, 3.1}},
                                   Color4d{0.9, 0.1, 0.7, 0.5},
                                   LayerValues{.position = {7.7, 5.4}, .opacity = 0.0}, 6.0, 5.0,
                                   24000);
    expectParity(expectations, executor, oracle, plan, requestFor(*plan),
                 "covered fill byte-exact behind a transparent top", true, true);
}

void testWarmCache(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "warm: cache created");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "warm: executor created");
    if (!executor) {
        return;
    }
    const auto plan = basicPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "warm: scene prepares");
    if (!prepared) {
        return;
    }
    const auto first = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(first.ready, "warm: the first scene completes");
    if (!first.ready) {
        return;
    }
    const auto afterFirst = executor.executor->counters();
    expectations.expect(afterFirst.outputCacheMisses == 1,
                        "warm: the first run is an output cache miss");
    expectations.expect(afterFirst.dispatches > 0, "warm: the first run dispatches");

    const auto equivalent = basicPlan();
    const auto secondPrepared = CpuGpuSceneBuilder{}.build(equivalent, requestFor(*equivalent));
    const auto second = runScene(*executor.executor, secondPrepared.scene, kSceneBudget);
    expectations.expect(second.ready, "warm: the second scene completes");
    const auto afterSecond = executor.executor->counters();
    expectations.expect(afterSecond.outputCacheHits == 1,
                        "warm: the unchanged output is a cache hit");
    expectations.expect(afterSecond.dispatches == afterFirst.dispatches,
                        "warm: an unchanged output performs zero native dispatches");
    expectations.expect(second.image != nullptr && first.image != nullptr &&
                            second.image.get() == first.image.get(),
                        "warm: the returned image is the cached resident image");
}

// The image of the merge's bottom-most foreground: for a fractional layer this is the covered
// command's image (the original solid command is deliberately unused).
[[nodiscard]] std::shared_ptr<const GpuImage> bottomLayerImage(GpuSceneCache& cache,
                                                               const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* merge = std::get_if<GpuSceneMergeCommand>(&command)) {
            if (merge->foregrounds.empty()) {
                continue;
            }
            const auto bottom = merge->foregrounds.front();
            if (bottom != bloom::runtime::kInvalidGpuSceneCommand &&
                static_cast<std::size_t>(bottom) < scene.commands().size()) {
                return cache.find(keyOf(scene.commands()[bottom]));
            }
        }
    }
    return nullptr;
}

void testChangedTopRetainsLower(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "changed: cache created");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "changed: executor created");
    if (!executor) {
        return;
    }
    const auto firstPlan = basicPlan();
    const auto firstPrepared = CpuGpuSceneBuilder{}.build(firstPlan, requestFor(*firstPlan));
    expectations.expect(firstPrepared.hasValue(), "changed: the first scene prepares");
    if (!firstPrepared) {
        return;
    }
    const auto first = runScene(*executor.executor, firstPrepared.scene, kSceneBudget);
    expectations.expect(first.ready, "changed: the first scene completes");
    if (!first.ready) {
        return;
    }
    const auto lowerImage = bottomLayerImage(*cache.cache, *firstPrepared.scene);
    expectations.expect(lowerImage != nullptr, "changed: the lower layer image is cached");

    // The merge foregrounds are bottom-to-top, so the FIRST merge entry is the top layer. Move only
    // that top layer; the second (bottom) entry is unchanged.
    const auto movedPlan =
        twoLayerPlan(format(16, 12), LayerValues{.position = {13.0, 9.5}, .opacity = 1.0},
                     LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0, 1000);
    const auto movedPrepared = CpuGpuSceneBuilder{}.build(movedPlan, requestFor(*movedPlan));
    expectations.expect(movedPrepared.hasValue(), "changed: the moved scene prepares");
    if (!movedPrepared) {
        return;
    }
    const auto before = executor.executor->counters();
    const auto moved = runScene(*executor.executor, movedPrepared.scene, kSceneBudget);
    expectations.expect(moved.ready, "changed: the moved scene completes");
    const auto after = executor.executor->counters();
    expectations.expect(after.commandCacheHits > before.commandCacheHits,
                        "changed: the unchanged lower subtree is served from cache");
    expectations.expect(after.dispatches - before.dispatches < 8,
                        "changed: the cached lower subtree saves at least one dispatch");
    const auto lowerAfter = bottomLayerImage(*cache.cache, *movedPrepared.scene);
    expectations.expect(lowerAfter != nullptr && lowerImage != nullptr &&
                            lowerAfter.get() == lowerImage.get(),
                        "changed: the lower cached image is retained unchanged");
}

void testSameContentDifferentIdentities(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "identity: cache created");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "identity: executor created");
    if (!executor) {
        return;
    }
    const auto firstPlan = basicPlan();
    const auto secondPlan =
        twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}, .opacity = 1.0},
                     LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0, 900000);
    const auto firstPrepared = CpuGpuSceneBuilder{}.build(firstPlan, requestFor(*firstPlan));
    const auto secondPrepared = CpuGpuSceneBuilder{}.build(secondPlan, requestFor(*secondPlan));
    expectations.expect(firstPrepared.hasValue() && secondPrepared.hasValue(),
                        "identity: both plans prepare");
    if (!firstPrepared || !secondPrepared) {
        return;
    }
    std::vector<std::string> firstKeys;
    std::vector<std::string> secondKeys;
    for (const auto& command : firstPrepared.scene->commands()) {
        firstKeys.push_back(keyOf(command));
    }
    for (const auto& command : secondPrepared.scene->commands()) {
        secondKeys.push_back(keyOf(command));
    }
    std::ranges::sort(firstKeys);
    std::ranges::sort(secondKeys);
    expectations.expect(firstKeys == secondKeys,
                        "identity: different node/parameter IDs keep identical semantic keys");

    const auto first = runScene(*executor.executor, firstPrepared.scene, kSceneBudget);
    expectations.expect(first.ready, "identity: the first scene completes");
    if (!first.ready) {
        return;
    }
    const auto before = executor.executor->counters();
    const auto second = runScene(*executor.executor, secondPrepared.scene, kSceneBudget);
    expectations.expect(second.ready, "identity: the same-content scene completes");
    const auto after = executor.executor->counters();
    expectations.expect(after.dispatches == before.dispatches,
                        "identity: same content with different IDs performs zero dispatches");
    expectations.expect(after.outputCacheHits > before.outputCacheHits,
                        "identity: the scene metadata is current while pixels are cached");
}

void testCancellation(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "cancel: cache created");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "cancel: executor created");
    if (!executor) {
        return;
    }
    {
        const auto plan = basicPlan();
        const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
        expectations.expect(prepared.hasValue(), "cancel: the scene prepares");
        if (!prepared) {
            return;
        }
        const auto begun = executor.executor->begin(prepared.scene, kSceneBudget);
        expectations.expect(begun.code == GpuSceneExecutorDiagnosticCode::None,
                            "cancel: begin accepts");
        executor.executor->cancel();
        const auto result = executor.executor->poll();
        expectations.expect(result == GpuSceneExecutorPollResult::Failure,
                            "cancel: a cancelled job fails closed");
        expectations.expect(executor.executor->diagnostic().code ==
                                GpuSceneExecutorDiagnosticCode::Cancelled,
                            "cancel: the diagnostic is Cancelled");
        expectations.expect(executor.executor->takeImage() == nullptr,
                            "cancel: no image is published after cancel");
        expectations.expect(executor.executor->counters().dispatches == 0,
                            "cancel: cancelling before the first poll never dispatches");
    }
    {
        const auto plan = fractionalPlan();
        const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
        expectations.expect(prepared.hasValue(), "cancel-live: the scene prepares");
        if (!prepared) {
            return;
        }
        const auto begun = executor.executor->begin(prepared.scene, kSceneBudget);
        expectations.expect(begun.code == GpuSceneExecutorDiagnosticCode::None,
                            "cancel-live: begin accepts");
        const auto firstPoll = executor.executor->poll();
        expectations.expect(firstPoll == GpuSceneExecutorPollResult::Pending,
                            "cancel-live: the first poll submits and stays pending");
        executor.executor->cancel();
        GpuSceneExecutorPollResult final = GpuSceneExecutorPollResult::Pending;
        for (std::uint64_t iteration = 0; iteration < kMaxPollIterations; ++iteration) {
            final = executor.executor->poll();
            if (final != GpuSceneExecutorPollResult::Pending) {
                break;
            }
            std::this_thread::yield();
        }
        expectations.expect(final == GpuSceneExecutorPollResult::Failure,
                            "cancel-live: the cancelled job fails closed");
        expectations.expect(executor.executor->diagnostic().code ==
                                GpuSceneExecutorDiagnosticCode::Cancelled,
                            "cancel-live: the diagnostic is Cancelled");
        expectations.expect(executor.executor->takeImage() == nullptr,
                            "cancel-live: no image is published");
        expectations.expect(!executor.executor->ownerDrainRequired() &&
                                !executor.executor->deviceLost(),
                            "cancel-live: a proven cancellation needs no owner drain");
    }
    const auto plan = basicPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    const auto recovered = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(recovered.ready, "cancel: the executor is usable after cancellation");
}

void testTinyBudget(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "budget: cache created");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "budget: executor created");
    if (!executor) {
        return;
    }
    const auto plan = basicPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "budget: the scene prepares");
    if (!prepared) {
        return;
    }
    const auto refused = executor.executor->begin(prepared.scene, 1);
    expectations.expect(refused.code == GpuSceneExecutorDiagnosticCode::OverBudget,
                        "budget: a tiny request budget is refused before Vulkan");
    expectations.expect(executor.executor->state() == GpuSceneExecutorJobState::Idle,
                        "budget: a refusal leaves the executor idle");
    expectations.expect(executor.executor->counters().budgetRefusals >= 1,
                        "budget: the refusal is counted");
    const auto retried = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(retried.ready, "budget: the executor remains usable with a real budget");
}

void testStructureRefusal(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "structure: cache created");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "structure: executor created");
    if (!executor) {
        return;
    }
    const auto nullResult = executor.executor->begin(nullptr, kSceneBudget);
    expectations.expect(nullResult.code == GpuSceneExecutorDiagnosticCode::InvalidArgument,
                        "structure: a null scene is refused");

    const auto plan = basicPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "structure: the scene prepares");
    if (!prepared) {
        return;
    }
    GpuSceneExecutorBudgets tight;
    tight.maxCommands = 1;
    auto limitedCache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(limitedCache.hasValue(), "structure: the limited cache is created");
    if (!limitedCache) {
        return;
    }
    auto limited = GpuSceneExecutor::create(device, *limitedCache.cache, tight);
    expectations.expect(limited.hasValue(), "structure: the limited executor is created");
    if (limited) {
        const auto refused = limited.executor->begin(prepared.scene, kSceneBudget);
        expectations.expect(refused.code == GpuSceneExecutorDiagnosticCode::TooManyCommands,
                            "structure: the command ceiling is enforced before Vulkan");
    }
}

void testForeignDeviceAndThread(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache,
                                GpuDevice* foreignDevice) {
    auto created = GpuSceneExecutor::create(device, cache);
    expectations.expect(created.hasValue(), "foreign: the owner thread creates an executor");
    if (!created) {
        return;
    }
    if (foreignDevice != nullptr) {
        expectations.expect(!created.executor->isBoundTo(*foreignDevice),
                            "foreign: a different device generation is not bound");
        auto foreignCache = GpuSceneCache::create(*foreignDevice, GpuSceneCacheBudgets{kCacheBudget});
        expectations.expect(foreignCache.hasValue(), "foreign: a foreign cache is created");
        if (foreignCache) {
            auto mismatched = GpuSceneExecutor::create(*foreignDevice, cache);
            expectations.expect(mismatched.diagnostic.code ==
                                    GpuSceneExecutorDiagnosticCode::InvalidArgument,
                                "foreign: a cache from another device is refused");
        }
    }

    const auto plan = basicPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "foreign: the scene prepares");
    if (!prepared) {
        return;
    }
    std::atomic<int> observed{static_cast<int>(GpuSceneExecutorDiagnosticCode::None)};
    std::thread worker([&]() {
        const auto result = created.executor->begin(prepared.scene, kSceneBudget);
        observed.store(static_cast<int>(result.code));
    });
    worker.join();
    expectations.expect(static_cast<GpuSceneExecutorDiagnosticCode>(observed.load()) ==
                            GpuSceneExecutorDiagnosticCode::WrongThread,
                        "foreign: a foreign-thread begin is WrongThread");
}

// A cached output whose actual descriptor does not match the scene must never be published: the
// executor drops it and recomputes.
void testOutputCacheHitDescriptorValidation(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "descriptor: cache created");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "descriptor: executor created");
    if (!executor) {
        return;
    }
    const auto plan = basicPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "descriptor: the scene prepares");
    if (!prepared) {
        return;
    }
    const auto first = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(first.ready, "descriptor: the first scene completes");
    if (!first.ready) {
        return;
    }
    const auto outputIndex = prepared.scene->outputCommand();
    const std::string outputKey = keyOf(prepared.scene->commands()[outputIndex]);
    static_cast<void>(cache.cache->erase(outputKey));

    auto solid = GpuSolid::create(device);
    expectations.expect(solid.hasValue(), "descriptor: a fixture solid is created");
    if (!solid) {
        return;
    }
    const auto window = ImageWindow::create(0, 0, 4, 4);
    expectations.expect(static_cast<bool>(window), "descriptor: the fixture window builds");
    if (!window) {
        return;
    }
    const GpuSolidParameters parameters{Rgba32f::transparent(), *window.value(), *window.value(),
                                        PixelAspectRatio::square()};
    const auto begun = solid.solid->begin(parameters, 1ULL << 20U);
    expectations.expect(begun.code == bloom::render::GpuSolidDiagnosticCode::None,
                        "descriptor: the fixture solid submits");
    if (begun.code != bloom::render::GpuSolidDiagnosticCode::None) {
        return;
    }
    for (std::uint64_t iteration = 0; iteration < kMaxPollIterations; ++iteration) {
        const auto result = solid.solid->poll();
        if (result != GpuSolidPollResult::Pending) {
            break;
        }
        std::this_thread::yield();
    }
    auto wrongImage = std::make_shared<const GpuImage>(solid.solid->takeImage());
    expectations.expect(wrongImage->isValid(), "descriptor: the wrong-descriptor fixture is valid");
    const auto inserted = cache.cache->insert(outputKey, wrongImage);
    expectations.expect(inserted == bloom::runtime::GpuSceneCacheInsertResult::Inserted,
                        "descriptor: the wrong-descriptor entry inserts");

    const auto before = executor.executor->counters();
    const auto second = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(second.ready, "descriptor: the executor recovers and completes");
    expectations.expect(descriptorMatchesScene(*second.image, *prepared.scene),
                        "descriptor: the recomputed output has the exact scene descriptor");
    expectations.expect(executor.executor->counters().outputCacheMisses > before.outputCacheMisses,
                        "descriptor: a mismatched output cache hit is recomputed, not published");
}

// ---- live-budget accounting -------------------------------------------------------------------

[[nodiscard]] std::optional<std::string> firstSolidKey(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* solid = std::get_if<GpuSceneSolidCommand>(&command)) {
            return solid->semanticKey;
        }
    }
    return std::nullopt;
}

// A long sequential graph with a tiny cache: cumulative allocation exceeds the budget but the live
// peak fits, so it must be accepted. A one-byte budget is refused and leaves the executor usable.
void testLiveBudgetLongGraph(Expectations& expectations, GpuDevice& device) {
    const auto plan = basicPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "live: the scene prepares");
    if (!prepared) {
        return;
    }
    auto measureCache = GpuSceneCache::create(device, GpuSceneCacheBudgets{1});
    auto measureExec = GpuSceneExecutor::create(device, *measureCache.cache);
    expectations.expect(measureCache.hasValue() && measureExec.hasValue(), "live: harness created");
    if (!measureCache || !measureExec) {
        return;
    }
    const auto measured = runScene(*measureExec.executor, prepared.scene, kSceneBudget);
    expectations.expect(measured.ready, "live: the measuring run completes");
    if (!measured.ready) {
        return;
    }
    const auto peak = measured.countersAtReady.peakLiveImageBytes;
    const auto cumulative = measured.countersAtReady.cumulativeProducedImageBytes;
    expectations.expect(peak > 0, "live: the live peak is non-zero");
    expectations.expect(cumulative > peak, "live: cumulative allocation exceeds the live peak");
    expectations.expect(measureCache.cache->entryCount() == 0,
                        "live: the tiny cache retained nothing");

    auto tightCache = GpuSceneCache::create(device, GpuSceneCacheBudgets{1});
    auto tightExec = GpuSceneExecutor::create(device, *tightCache.cache);
    expectations.expect(tightCache.hasValue() && tightExec.hasValue(), "live: tight harness created");
    if (!tightCache || !tightExec) {
        return;
    }
    const auto tight = runScene(*tightExec.executor, prepared.scene, peak);
    expectations.expect(tight.ready, "live: a graph whose live peak fits is accepted");
    expectations.expect(tight.countersAtReady.cumulativeProducedImageBytes > peak,
                        "live: the accepted run allocated cumulatively beyond the budget");
    expectations.expect(tight.countersAtReady.peakLiveImageBytes <= peak,
                        "live: the accepted run's live peak stayed within the budget");

    const auto refused = tightExec.executor->begin(prepared.scene, 1);
    expectations.expect(refused.code == GpuSceneExecutorDiagnosticCode::OverBudget,
                        "live: a one-byte budget is refused before Vulkan");
    expectations.expect(tightExec.executor->counters().budgetRefusals >= 1,
                        "live: the refusal is counted");
    const auto recovered = runScene(*tightExec.executor, prepared.scene, peak);
    expectations.expect(recovered.ready, "live: the executor is usable after the refusal");
}

// A tight budget must refuse when a cached input pinned for this request already exceeds it, before
// any native work.
void testTightBudgetWithCachedInputs(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto exec = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(cache.hasValue() && exec.hasValue(), "cached-budget: harness created");
    if (!cache || !exec) {
        return;
    }
    const auto firstPlan = basicPlan();
    const auto firstPrepared = CpuGpuSceneBuilder{}.build(firstPlan, requestFor(*firstPlan));
    expectations.expect(firstPrepared.hasValue(), "cached-budget: the warming scene prepares");
    if (!firstPrepared) {
        return;
    }
    const auto firstRun = runScene(*exec.executor, firstPrepared.scene, kSceneBudget);
    expectations.expect(firstRun.ready, "cached-budget: the warming scene completes");
    if (!firstRun.ready) {
        return;
    }
    // The warmed output is now a cached pin this request would hold: a one-byte budget must refuse
    // it at begin, before any Vulkan work.
    const auto cachedRefused = exec.executor->begin(firstPrepared.scene, 1);
    expectations.expect(
        cachedRefused.code == GpuSceneExecutorDiagnosticCode::OverBudget,
        "cached-budget: a cached output pin over a one-byte budget is refused at begin");
    expectations.expect(exec.executor->state() == GpuSceneExecutorJobState::Idle,
                        "cached-budget: the cached refusal leaves the executor idle");

    // Move the top layer so the output misses while the bottom layer is a cached input.
    const auto movedPlan =
        twoLayerPlan(format(16, 12), LayerValues{.position = {13.0, 9.5}, .opacity = 1.0},
                     LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0, 1000);
    const auto moved = CpuGpuSceneBuilder{}.build(movedPlan, requestFor(*movedPlan));
    expectations.expect(moved.hasValue(), "cached-budget: the moved scene prepares");
    if (!moved) {
        return;
    }
    const auto refused = exec.executor->begin(moved.scene, 1);
    expectations.expect(refused.code == GpuSceneExecutorDiagnosticCode::OverBudget,
                        "cached-budget: a cached input under a one-byte budget is refused at begin");
    expectations.expect(exec.executor->state() == GpuSceneExecutorJobState::Idle,
                        "cached-budget: the refusal leaves the executor idle");
    const auto recovered = runScene(*exec.executor, moved.scene, kSceneBudget);
    expectations.expect(recovered.ready, "cached-budget: the executor is usable with a real budget");
}

// Two identical solids share one semantic key and resolve to the same cached image: the alias is
// pinned under two command indexes but charged once, and the alias bytes are reported.
void testAliasedInputChargedOnce(Expectations& expectations, GpuDevice& device) {
    const Color4d aliasedColor{0.4, 0.6, 0.2, 0.5};
    const auto warmPlan =
        twoSolidPlan(format(16, 12), aliasedColor, LayerValues{.position = {4.0, 3.5}},
                     aliasedColor, LayerValues{.position = {9.0, 7.5}}, 6.0, 5.0, 31000);
    const auto runPlan =
        twoSolidPlan(format(16, 12), aliasedColor, LayerValues{.position = {5.0, 3.5}},
                     aliasedColor, LayerValues{.position = {9.0, 6.5}}, 6.0, 5.0, 31000);
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto exec = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(cache.hasValue() && exec.hasValue(), "alias: harness created");
    if (!cache || !exec) {
        return;
    }
    const auto warmPrepared = CpuGpuSceneBuilder{}.build(warmPlan, requestFor(*warmPlan));
    const auto runPrepared = CpuGpuSceneBuilder{}.build(runPlan, requestFor(*runPlan));
    expectations.expect(warmPrepared.hasValue() && runPrepared.hasValue(),
                        "alias: both plans prepare");
    if (!warmPrepared || !runPrepared) {
        return;
    }
    std::size_t translationCommands = 0;
    for (const auto& command : runPrepared.scene->commands()) {
        if (std::holds_alternative<GpuSceneTranslationCommand>(command)) {
            ++translationCommands;
        }
    }
    const bool runUsesSolids = translationCommands >= 2;
    const auto warm = runScene(*exec.executor, warmPrepared.scene, kSceneBudget);
    expectations.expect(warm.ready, "alias: the warming run completes");
    if (!warm.ready) {
        return;
    }
    const auto key = firstSolidKey(*runPrepared.scene);
    expectations.expect(key.has_value(), "alias: a solid key is present");
    const auto solidImage = key.has_value() ? cache.cache->find(*key) : nullptr;
    expectations.expect(solidImage != nullptr, "alias: the shared solid image is cached");
    const std::uint64_t solidBytes = solidImage != nullptr ? solidImage->allocationBytes() : 0;
    expectations.expect(runUsesSolids && solidBytes > 0,
                        "alias: the identical solids are reachable and cached");

    const auto aliased = runScene(*exec.executor, runPrepared.scene, kSceneBudget);
    expectations.expect(aliased.ready, "alias: the aliased run completes");
    expectations.expect(aliased.countersAtReady.aliasedImagePinBytes >= solidBytes,
                        "alias: the shared image was charged once and the duplicate recorded");
    const auto aliasedPeak = aliased.countersAtReady.peakLiveImageBytes;
    const auto rerun = runScene(*exec.executor, runPrepared.scene, aliasedPeak);
    expectations.expect(rerun.ready, "alias: the aliased scene fits its single-count live peak");

    // Control: two distinct solids never alias.
    const auto distinctWarm =
        twoSolidPlan(format(16, 12), Color4d{0.5, 0.25, 0.125, 1.0},
                     LayerValues{.position = {4.0, 3.5}}, Color4d{0.125, 0.375, 0.75, 0.5},
                     LayerValues{.position = {9.0, 7.5}}, 6.0, 5.0, 32000);
    const auto distinctRun =
        twoSolidPlan(format(16, 12), Color4d{0.5, 0.25, 0.125, 1.0},
                     LayerValues{.position = {5.0, 3.5}}, Color4d{0.125, 0.375, 0.75, 0.5},
                     LayerValues{.position = {9.0, 6.5}}, 6.0, 5.0, 32000);
    auto cache2 = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto exec2 = GpuSceneExecutor::create(device, *cache2.cache);
    if (cache2 && exec2) {
        const auto warm2 = CpuGpuSceneBuilder{}.build(distinctWarm, requestFor(*distinctWarm));
        const auto run2 = CpuGpuSceneBuilder{}.build(distinctRun, requestFor(*distinctRun));
        if (warm2 && run2) {
            expectations.expect(runScene(*exec2.executor, warm2.scene, kSceneBudget).ready,
                                "alias: the distinct warming run completes");
            const auto distinct = runScene(*exec2.executor, run2.scene, kSceneBudget);
            expectations.expect(distinct.ready, "alias: the distinct run completes");
            expectations.expect(distinct.countersAtReady.aliasedImagePinBytes == 0,
                                "alias: distinct inputs are never counted as an alias");
        }
    }
}

// The executor pins a cached input for a composite job; the native op retains the same image while
// the submission is in flight. Owner-thread, native-first destruction must drain (or quarantine) the
// entire native Impl, which owns that input pin, before the executor releases its own pins. This
// reads the real shared ownership to prove it: the input's use_count rises above the cache+probe
// baseline while the job is live and falls back after teardown, with no executor-global leak list.
void testNativeOwnershipDuringTeardown(Expectations& expectations, GpuDevice& device) {
    const Color4d colorA{0.5, 0.25, 0.125, 1.0};
    const Color4d colorB{0.125, 0.375, 0.75, 0.5};
    const auto warmPlan =
        twoSolidPlan(format(16, 12), colorA, LayerValues{.position = {4.0, 3.5}}, colorB,
                     LayerValues{.position = {9.0, 7.5}}, 6.0, 5.0, 41000);
    const auto runPlan =
        twoSolidPlan(format(16, 12), colorA, LayerValues{.position = {5.0, 3.5}}, colorB,
                     LayerValues{.position = {9.0, 6.5}}, 6.0, 5.0, 41000);
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "ownership: cache created");
    if (!cache) {
        return;
    }
    const auto warmPrepared = CpuGpuSceneBuilder{}.build(warmPlan, requestFor(*warmPlan));
    const auto runPrepared = CpuGpuSceneBuilder{}.build(runPlan, requestFor(*runPlan));
    expectations.expect(warmPrepared.hasValue() && runPrepared.hasValue(),
                        "ownership: both plans prepare");
    if (!warmPrepared || !runPrepared) {
        return;
    }
    // The first dispatched step is the bottom merge foreground's translation, which consumes its
    // (cached) solid input.
    std::optional<std::string> firstStepInputKey;
    for (const auto& command : runPrepared.scene->commands()) {
        const auto* merge = std::get_if<GpuSceneMergeCommand>(&command);
        if (merge == nullptr || merge->foregrounds.empty()) {
            continue;
        }
        const auto bottom = merge->foregrounds.front();
        if (bottom == bloom::runtime::kInvalidGpuSceneCommand ||
            static_cast<std::size_t>(bottom) >= runPrepared.scene->commands().size()) {
            continue;
        }
        if (const auto* translation = std::get_if<GpuSceneTranslationCommand>(
                &runPrepared.scene->commands()[bottom]);
            translation != nullptr) {
            firstStepInputKey = keyOf(runPrepared.scene->commands()[translation->input]);
        }
        break;
    }
    expectations.expect(firstStepInputKey.has_value(),
                        "ownership: the first native step has a cached input key");
    if (!firstStepInputKey.has_value()) {
        return;
    }

    auto exec = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(exec.hasValue(), "ownership: executor created");
    if (!exec) {
        return;
    }
    const auto warm = runScene(*exec.executor, warmPrepared.scene, kSceneBudget);
    expectations.expect(warm.ready, "ownership: the warming run completes");
    if (!warm.ready) {
        return;
    }
    auto probe = cache.cache->find(*firstStepInputKey);
    expectations.expect(probe != nullptr, "ownership: the input is cached");
    if (probe == nullptr) {
        return;
    }
    const auto baseline = probe.use_count();

    expectations.expect(exec.executor->begin(runPrepared.scene, kSceneBudget).code ==
                            GpuSceneExecutorDiagnosticCode::None,
                        "ownership: begin accepts the run scene");
    expectations.expect(exec.executor->poll() == GpuSceneExecutorPollResult::Pending,
                        "ownership: the first poll dispatches the composite step");
    const auto during = probe.use_count();
    expectations.expect(during >= baseline + 2,
                        "ownership: the executor AND the native op retain the in-flight input");

    // Owner-thread native-first destruction while the composite job is live.
    exec.executor.reset();
    expectations.expect(!GpuSceneExecutor::teardownDrainIncomplete(),
                        "ownership: the healthy native drain is not reported incomplete");
    const auto after = probe.use_count();
    expectations.expect(after < during,
                        "ownership: the native drain released the input before executor pins");
}

// ---- native retirement / drain contracts ------------------------------------------------------

void testNativeRetirementContracts(Expectations& expectations, GpuDevice& device) {
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
    namespace fault = bloom::render::gpu_scene_executor_fault;
    using Fault = fault::PollFault;
    fault::clear();

    const auto plan = basicPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "drain: the scene prepares");
    if (!prepared) {
        return;
    }

    // 1) Deadline while a real submitted job's poll is stalled: fail the logical request promptly,
    //    retain the unproven submission, refuse reuse, then drain and reuse.
    {
        GpuSceneExecutorBudgets budgets;
        budgets.jobDeadlineMilliseconds = 0;
        auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
        auto exec = GpuSceneExecutor::create(device, *cache.cache, budgets);
        expectations.expect(exec.hasValue(), "drain: the deadline executor is created");
        if (exec) {
            const auto begun = exec.executor->begin(prepared.scene, kSceneBudget);
            expectations.expect(begun.code == GpuSceneExecutorDiagnosticCode::None,
                                "drain: begin accepts");
            expectations.expect(exec.executor->poll() == GpuSceneExecutorPollResult::Pending,
                                "drain: the first poll submits and stays pending");
            fault::set(Fault::StallPending);
            const auto timedOut = exec.executor->poll();
            expectations.expect(timedOut == GpuSceneExecutorPollResult::Failure,
                                "drain: the deadline fails the request promptly");
            expectations.expect(exec.executor->diagnostic().code ==
                                    GpuSceneExecutorDiagnosticCode::NativeTimeout,
                                "drain: the diagnostic is NativeTimeout");
            expectations.expect(exec.executor->ownerDrainRequired(),
                                "drain: the unproven submission requires owner drain");
            expectations.expect(exec.executor->takeImage() == nullptr,
                                "drain: no image is published at the deadline");
            expectations.expect(
                exec.executor->begin(prepared.scene, kSceneBudget).code ==
                    GpuSceneExecutorDiagnosticCode::OwnerDrainRequired,
                "drain: reuse is refused while the submission is unproven");
            fault::clear();
            for (std::uint64_t i = 0; i < kMaxPollIterations && exec.executor->ownerDrainRequired();
                 ++i) {
                static_cast<void>(exec.executor->poll());
                std::this_thread::yield();
            }
            expectations.expect(!exec.executor->ownerDrainRequired(),
                                "drain: the owner drained the submission");
            expectations.expect(exec.executor->begin(prepared.scene, kSceneBudget).code ==
                                    GpuSceneExecutorDiagnosticCode::None,
                                "drain: the executor is reusable after the drain");
            exec.executor->cancel();
            static_cast<void>(exec.executor->poll());
        }
    }

    // 2) Unknown fence: a Failure that does not prove retirement retains and poisons, then drains.
    {
        auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
        auto exec = GpuSceneExecutor::create(device, *cache.cache);
        expectations.expect(exec.hasValue(), "unknown: the executor is created");
        if (exec) {
            static_cast<void>(exec.executor->begin(prepared.scene, kSceneBudget));
            static_cast<void>(exec.executor->poll());
            fault::set(Fault::UnknownFence);
            const auto failed = exec.executor->poll();
            expectations.expect(failed == GpuSceneExecutorPollResult::Failure,
                                "unknown: the unproven failure fails the request");
            expectations.expect(exec.executor->diagnostic().code ==
                                    GpuSceneExecutorDiagnosticCode::NativeUnproven,
                                "unknown: the diagnostic is NativeUnproven");
            expectations.expect(exec.executor->ownerDrainRequired(),
                                "unknown: the unproven submission requires owner drain");
            expectations.expect(exec.executor->takeImage() == nullptr,
                                "unknown: no image is published");
            expectations.expect(
                exec.executor->begin(prepared.scene, kSceneBudget).code ==
                    GpuSceneExecutorDiagnosticCode::OwnerDrainRequired,
                "unknown: reuse is refused while unproven");
            for (std::uint64_t i = 0; i < kMaxPollIterations && exec.executor->ownerDrainRequired();
                 ++i) {
                static_cast<void>(exec.executor->poll());
                std::this_thread::yield();
            }
            expectations.expect(!exec.executor->ownerDrainRequired(),
                                "unknown: the owner drained the submission");
        }
    }

    // 3) Device loss is mapped distinctly and is terminal.
    {
        auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
        auto exec = GpuSceneExecutor::create(device, *cache.cache);
        expectations.expect(exec.hasValue(), "lost: the executor is created");
        if (exec) {
            static_cast<void>(exec.executor->begin(prepared.scene, kSceneBudget));
            static_cast<void>(exec.executor->poll());
            fault::set(Fault::DeviceLost);
            const auto failed = exec.executor->poll();
            expectations.expect(failed == GpuSceneExecutorPollResult::Failure,
                                "lost: the request fails");
            expectations.expect(exec.executor->diagnostic().code ==
                                    GpuSceneExecutorDiagnosticCode::DeviceLost,
                                "lost: the diagnostic is DeviceLost");
            expectations.expect(exec.executor->deviceLost() &&
                                    !exec.executor->ownerDrainRequired(),
                                "lost: the executor is terminal without an unproven drain");
            expectations.expect(exec.executor->takeImage() == nullptr,
                                "lost: no image is published");
            expectations.expect(exec.executor->begin(prepared.scene, kSceneBudget).code ==
                                    GpuSceneExecutorDiagnosticCode::DeviceLost,
                                "lost: reuse is refused on a lost device");
        }
        // The seam proved the REAL fence retired before injecting loss, so destroying the poisoned
        // pipeline must not report a failed bounded native drain.
        expectations.expect(!GpuSceneExecutor::teardownDrainIncomplete(),
                            "lost: no failed native bounded drain is reported");
    }

    // 4) A real submitted job cancelled and proven retired is immediately reusable.
    {
        auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
        auto exec = GpuSceneExecutor::create(device, *cache.cache);
        expectations.expect(exec.hasValue(), "reuse: the executor is created");
        if (exec) {
            static_cast<void>(exec.executor->begin(prepared.scene, kSceneBudget));
            static_cast<void>(exec.executor->poll());
            exec.executor->cancel();
            for (std::uint64_t i = 0; i < kMaxPollIterations; ++i) {
                if (exec.executor->poll() != GpuSceneExecutorPollResult::Pending) {
                    break;
                }
                std::this_thread::yield();
            }
            expectations.expect(exec.executor->diagnostic().code ==
                                    GpuSceneExecutorDiagnosticCode::Cancelled,
                                "reuse: the proven cancellation is Cancelled");
            expectations.expect(!exec.executor->ownerDrainRequired() &&
                                    !exec.executor->deviceLost(),
                                "reuse: a proven cancellation needs no drain");
            expectations.expect(exec.executor->begin(prepared.scene, kSceneBudget).code ==
                                    GpuSceneExecutorDiagnosticCode::None,
                                "reuse: the executor is reusable after a proven cancellation");
            exec.executor->cancel();
            static_cast<void>(exec.executor->poll());
        }
    }

    // 5) Destroying while a fault-injected poll still reports Pending must not falsely report an
    //    incomplete teardown: the REAL healthy job is still retired by the owned pipeline's bounded
    //    native drain (which owns the input pins), and only a genuine drain failure would be fused.
    {
        fault::clear();
        auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
        {
            auto exec = GpuSceneExecutor::create(device, *cache.cache);
            if (exec) {
                static_cast<void>(exec.executor->begin(prepared.scene, kSceneBudget));
                static_cast<void>(exec.executor->poll());
                fault::set(Fault::StallPending);
                exec.executor.reset();
                expectations.expect(
                    !GpuSceneExecutor::teardownDrainIncomplete(),
                    "destroy: a healthy bounded native drain is not reported incomplete");
            }
        }
        fault::clear();
    }
#else
    static_cast<void>(expectations);
    static_cast<void>(device);
    std::cout << "NOTE: native retirement fault tests skipped (fault injection not compiled in)\n";
#endif
}

// ---- benchmark ---------------------------------------------------------------------------------

[[nodiscard]] double medianMilliseconds(std::vector<double> samples) {
    if (samples.empty()) {
        return 0.0;
    }
    std::ranges::sort(samples);
    const std::size_t middle = samples.size() / 2;
    if (samples.size() % 2 == 0) {
        return (samples[middle - 1] + samples[middle]) / 2.0;
    }
    return samples[middle];
}

void runBenchmark(Expectations& expectations, GpuDevice& device) {
    const CpuCompositionEvaluator oracle;
    struct Size final {
        std::uint32_t width;
        std::uint32_t height;
    };
    for (const Size size : {Size{1280, 720}, Size{1920, 1080}}) {
        const auto compositionFormat = format(size.width, size.height);
        const auto halfWidth = static_cast<double>(size.width) / 2.0;
        const auto halfHeight = static_cast<double>(size.height) / 2.0;
        const auto plan =
            twoLayerPlan(compositionFormat,
                         LayerValues{.position = {halfWidth * 0.45, halfHeight * 0.5}},
                         LayerValues{.position = {halfWidth * 0.62, halfHeight * 0.42},
                                     .opacity = 0.75},
                         halfWidth, halfHeight, 77000);
        const auto request = requestFor(*plan);
        const CpuGpuSceneBuilder builder;

        std::vector<double> prepareSamples;
        std::vector<double> cpuSamples;
        for (int iteration = 0; iteration < 15; ++iteration) {
            const auto start = std::chrono::steady_clock::now();
            const auto prepared = builder.build(plan, request);
            const auto afterPrepare = std::chrono::steady_clock::now();
            if (!prepared) {
                expectations.expect(false, "benchmark: the scene prepares");
                return;
            }
            if (iteration >= 5) {
                prepareSamples.push_back(
                    std::chrono::duration<double, std::milli>(afterPrepare - start).count());
            }
            auto cpuRequest = request;
            cpuRequest.bypassOperationCache = true;
            const auto frame = oracle.evaluate(plan, cpuRequest, {});
            const auto afterCpu = std::chrono::steady_clock::now();
            expectations.expect(frame.frame() != nullptr, "benchmark: the CPU frame evaluates");
            if (iteration >= 5) {
                cpuSamples.push_back(
                    std::chrono::duration<double, std::milli>(afterCpu - afterPrepare).count());
            }
        }

        auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
        expectations.expect(cache.hasValue(), "benchmark: cache created");
        if (!cache) {
            return;
        }
        auto executor = GpuSceneExecutor::create(device, *cache.cache);
        expectations.expect(executor.hasValue(), "benchmark: executor created");
        if (!executor) {
            return;
        }
        const auto prepared = builder.build(plan, request);
        if (!prepared) {
            expectations.expect(false, "benchmark: the reusable scene prepares");
            return;
        }
        std::vector<double> uncachedSamples;
        std::vector<double> warmSamples;
        for (int iteration = 0; iteration < 15; ++iteration) {
            cache.cache->clear();
            const auto start = std::chrono::steady_clock::now();
            const auto run = runScene(*executor.executor, prepared.scene, kSceneBudget);
            const auto elapsed = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            expectations.expect(run.ready, "benchmark: the native scene completes");
            if (!run.ready) {
                return;
            }
            if (iteration >= 5) {
                uncachedSamples.push_back(elapsed);
            }
            const auto warmStart = std::chrono::steady_clock::now();
            const auto warm = runScene(*executor.executor, prepared.scene, kSceneBudget);
            const auto warmElapsed = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - warmStart)
                                         .count();
            expectations.expect(warm.ready, "benchmark: the warm scene completes");
            if (!warm.ready) {
                return;
            }
            if (iteration >= 5) {
                warmSamples.push_back(warmElapsed);
            }
            cache.cache->clear();
        }

        std::cout << std::fixed << std::setprecision(3) << size.width << 'x' << size.height
                  << " CPU-prepare=" << medianMilliseconds(prepareSamples)
                  << "ms CPU-eval=" << medianMilliseconds(cpuSamples)
                  << "ms native-uncached=" << medianMilliseconds(uncachedSamples)
                  << "ms native-warm-cache=" << medianMilliseconds(warmSamples) << "ms\n";
    }
    std::cout << "Boundaries: CPU preparation (CpuGpuSceneBuilder) and CPU evaluation are measured "
                 "separately from native begin-to-Ready; viewer/presentation excluded. No "
                 "application FPS claim.\n";
}

} // namespace

int main(const int argc, char** argv) {
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
            std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message << '\n';
            return 1;
        }
        std::cout << "SKIP: no compatible Vulkan device available: " << device.diagnostic.message
                  << '\n';
        return expectations.ok() ? 0 : 1;
    }
    auto foreignDevice = GpuDevice::create(createOptions);

    if (options.benchmark) {
        runBenchmark(expectations, *device.device);
        if (!expectations.ok()) {
            std::cerr << "FAIL: GPU scene executor benchmark expectations failed\n";
            return 1;
        }
        return 0;
    }

    auto cache = GpuSceneCache::create(*device.device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "the parity cache is created");
    if (!cache) {
        std::cerr << "FAIL: the parity cache could not be created\n";
        return 1;
    }
    auto executor = GpuSceneExecutor::create(*device.device, *cache.cache);
    expectations.expect(executor.hasValue(), "the parity executor is created");
    if (!executor) {
        std::cerr << "FAIL: the parity executor could not be created\n";
        return 1;
    }

    testFixtures(expectations, *executor.executor, oracle);
    testEmptyInactive(expectations, *executor.executor, oracle);
    testCoveredByteExact(expectations, *executor.executor, oracle);
    testWarmCache(expectations, *device.device);
    testChangedTopRetainsLower(expectations, *device.device);
    testSameContentDifferentIdentities(expectations, *device.device);
    testCancellation(expectations, *device.device);
    testTinyBudget(expectations, *device.device);
    testStructureRefusal(expectations, *device.device);
    testForeignDeviceAndThread(expectations, *device.device, *cache.cache,
                               foreignDevice ? foreignDevice.device.get() : nullptr);
    testOutputCacheHitDescriptorValidation(expectations, *device.device);
    testLiveBudgetLongGraph(expectations, *device.device);
    testTightBudgetWithCachedInputs(expectations, *device.device);
    testAliasedInputChargedOnce(expectations, *device.device);
    testNativeOwnershipDuringTeardown(expectations, *device.device);
    testNativeRetirementContracts(expectations, *device.device);

    if (!expectations.ok()) {
        std::cerr << "FAIL: GPU scene executor expectations failed\n";
        return 1;
    }
    std::cout << "PASS: GPU scene executor\n";
    return 0;
}
