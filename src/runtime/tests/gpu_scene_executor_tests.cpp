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
using bloom::render::readbackResidentImage;
using bloom::render::Rgba32f;
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
using bloom::runtime::executor_test::pixelAspect;
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
        std::cerr << "executor begin refused: code=" << static_cast<int>(run.begin.code)
                  << " message=" << run.begin.message << '\n';
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
    const auto preparedInteger = CpuGpuSceneBuilder{}.build(integerPlan, requestFor(*integerPlan));
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

    const auto proxyFormat = format(9, 6, pixelAspect(4, 3));
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

    const auto transparentTop =
        twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}, .opacity = 0.0},
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
    const auto plan =
        twoSolidPlan(format(16, 12), Color4d{0.4, 0.6, 0.2, 0.5},
                     LayerValues{.position = {4.3, 3.1}}, Color4d{0.9, 0.1, 0.7, 0.5},
                     LayerValues{.position = {7.7, 5.4}, .opacity = 0.0}, 6.0, 5.0, 24000);
    expectParity(expectations, executor, oracle, plan, requestFor(*plan),
                 "covered fill byte-exact behind a transparent top", true, true);
}

#include "gpu_scene_executor_cache_tests.ipp"

#include "gpu_scene_executor_execution_tests.ipp"

#include "gpu_scene_executor_native_tests.ipp"

#include "gpu_scene_executor_benchmark_tests.ipp"

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
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
