// ADR 0028 animated layer masks on the GPU: a keyed mask path must play without compile stalls.
//
// A mask path keyed on 24 consecutive frames (24 distinct sampled outlines, every mode and control
// in the stack) is prepared by the production CpuGpuSceneBuilder through the one shared preparer
// resolved from the packaged shader tools. The geometry is data: every frame is its own command
// (distinct identity and cached output) while all 24 share one compiled SPIR-V module and one
// native pipeline. The test asserts at most one shader compile and at most one native program for
// the whole range, then runs the frames on the production GpuSceneExecutor: the cold pass
// dispatches every frame and matches the unchanged CPU oracle (exact alpha, which is the mask
// coverage), and the warm pass over the same range dispatches nothing and is served by the output
// cache. Without a device the preparation half still runs and the native half reports SKIP (77),
// or FAIL under --require-device.

#include "gpu_coverage_contract_plans.hpp"
#include "gpu_coverage_native_support.hpp"
#include "gpu_coverage_ocio_support.hpp"

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_command.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

using bloom::core::RationalTime;
using bloom::document::PathAnchor;
using bloom::document::PathValue;
using bloom::document::Vec2d;

constexpr std::int64_t kFrames = 24;
constexpr std::uint64_t kIdBase = 113000;
constexpr std::uint64_t kSceneBudget = 1U << 28U;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

// The coverage fixture's full mask stack with its first path keyed on every frame: each key moves
// the outline and bends its curved handle, so all 24 sampled outlines differ.
[[nodiscard]] std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> animatedPlan() {
    auto kernel = bloom::gpu_coverage_plans::layerMaskKernel(kIdBase);
    auto& first = kernel.masks.front().path;
    const auto base = std::get<PathValue>(first.source);
    bloom::runtime::CompiledPathCurve curve{
        bloom::document::AnimationCurveId::fromRaw(kIdBase + 90), {}};
    for (std::int64_t frame = 0; frame < kFrames; ++frame) {
        auto path = base;
        const double shift = 0.0625 * static_cast<double>(frame);
        for (auto& anchor : path.anchors) {
            anchor.point.x += shift;
            anchor.point.y += shift * 0.5;
        }
        path.anchors[1].outHandle =
            Vec2d{path.anchors[1].point.x + 1.0 + shift, path.anchors[1].point.y + 1.0};
        curve.keyframes.push_back(bloom::runtime::CompiledPathKeyframe{
            bloom::document::KeyframeId::fromRaw(kIdBase + 100 + static_cast<std::uint64_t>(frame)),
            RationalTime::fromInteger(frame), std::move(path),
            bloom::runtime::CompiledKeyframeInterpolation::Linear});
    }
    first.source = std::move(curve);
    return bloom::gpu_coverage_plans::effectPlan(kernel, kIdBase);
}

[[nodiscard]] bloom::runtime::EvaluationRequest
requestAt(const bloom::runtime::CompiledCompositionPlan& plan, const std::int64_t frame) {
    auto request = bloom::gpu_coverage_fixtures::requestFor(plan);
    request.time = RationalTime::fromInteger(frame);
    return request;
}

[[nodiscard]] const bloom::runtime::GpuSceneOcioEffectCommand*
maskCommandOf(const bloom::runtime::PreparedGpuScene& scene) {
    const bloom::runtime::GpuSceneOcioEffectCommand* found = nullptr;
    for (const auto& command : scene.commands()) {
        if (const auto* ocio = std::get_if<bloom::runtime::GpuSceneOcioEffectCommand>(&command)) {
            if (found != nullptr)
                return nullptr;
            found = ocio;
        }
    }
    return found;
}

struct Options final {
    std::filesystem::path loader;
    bool requireDevice = false;
    bool valid = true;
};

[[nodiscard]] Options parse(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc)
            options.loader = argv[++index];
        else if (argument == "--require-device")
            options.requireDevice = true;
        else
            options.valid = false;
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parse(argc, argv);
    if (!options.valid) {
        std::cerr << "invalid arguments\n";
        return 2;
    }
    const auto context = bloom::gpu_coverage_ocio::sharedContext();
    if (context == nullptr || context->preparer == nullptr) {
        if (options.requireDevice) {
            std::cerr << "FAIL: the packaged shader tools are unavailable: "
                      << bloom::gpu_coverage_ocio::unavailableReason() << '\n';
            return 1;
        }
        std::cout << "SKIP: the packaged shader tools are unavailable: "
                  << bloom::gpu_coverage_ocio::unavailableReason() << '\n';
        return 77;
    }
    const auto plan = animatedPlan();
    const bloom::runtime::CpuGpuSceneBuilder builder(nullptr, {}, *context);
    auto& preparer = *context->preparer;

    // Preparation: 24 distinct commands, one compiled module, one native program key.
    const auto beforePrepare = preparer.counters();
    const auto prepareStart = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<const bloom::runtime::PreparedGpuScene>> scenes;
    std::set<bloom::core::Sha256Digest> identities;
    std::set<bloom::core::Sha256Digest> programKeys;
    std::set<bloom::core::Sha256Digest> artifacts;
    for (std::int64_t frame = 0; frame < kFrames; ++frame) {
        const auto prepared = builder.build(plan, requestAt(*plan, frame));
        expect(static_cast<bool>(prepared),
               "frame " + std::to_string(frame) +
                   " prepares through the production builder: " + prepared.diagnostic.message);
        if (!prepared)
            return 1;
        const auto* command = maskCommandOf(*prepared.scene);
        expect(command != nullptr && command->program != nullptr,
               "frame " + std::to_string(frame) + " carries exactly one mask program command");
        if (command == nullptr || command->program == nullptr)
            return 1;
        identities.insert(command->program->identity());
        programKeys.insert(command->program->nativeProgramKey());
        artifacts.insert(command->program->artifact().spirvDigest);
        scenes.push_back(prepared.scene);
    }
    const auto prepareMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - prepareStart)
                               .count();
    const auto afterPrepare = preparer.counters();
    const auto compilerRuns = afterPrepare.shaderCompilerRuns - beforePrepare.shaderCompilerRuns;
    expect(identities.size() == static_cast<std::size_t>(kFrames),
           "every keyed frame is its own mask command (distinct geometry identity)");
    expect(artifacts.size() == 1, "all keyed frames share one compiled SPIR-V module");
    expect(programKeys.size() == 1, "all keyed frames share one native program key");
    expect(compilerRuns <= 1,
           "24 distinct keyed frames ran the shader compiler at most once (ran " +
               std::to_string(compilerRuns) + ")");
    std::cout << "PREPARE frames=" << kFrames << " commands=" << identities.size()
              << " modules=" << artifacts.size() << " programKeys=" << programKeys.size()
              << " shaderCompilerRuns=" << compilerRuns << " compilerCacheHits="
              << (afterPrepare.shaderCompilerCacheHits - beforePrepare.shaderCompilerCacheHits)
              << " prepareMs=" << prepareMs << '\n';

    bloom::render::GpuDeviceCreationOptions deviceOptions;
    deviceOptions.loader_path = options.loader;
    auto device = bloom::render::GpuDevice::create(deviceOptions);
    if (!device) {
        if (options.requireDevice) {
            std::cerr << "FAIL: --require-device was requested but no native GPU device is "
                         "available: "
                      << device.diagnostic.message << '\n';
            return 1;
        }
        std::cout << "SKIP: no native GPU device available: " << device.diagnostic.message << '\n';
        return failures == 0 ? 77 : 1;
    }
    auto cacheResult = bloom::runtime::GpuSceneCache::create(*device.device);
    expect(static_cast<bool>(cacheResult), "the scene cache is created");
    if (!cacheResult)
        return 1;
    auto executorResult =
        bloom::runtime::GpuSceneExecutor::create(*device.device, *cacheResult.cache);
    expect(static_cast<bool>(executorResult), "the scene executor is created");
    if (!executorResult)
        return 1;
    auto& executor = *executorResult.executor;
    const bloom::runtime::CpuCompositionEvaluator oracle;

    const auto runFrame =
        [&](const std::int64_t frame,
            const std::shared_ptr<const bloom::runtime::PreparedGpuScene>& scene) {
            const auto label = "frame " + std::to_string(frame);
            if (const auto diagnostic = executor.begin(scene, kSceneBudget);
                diagnostic.code != bloom::runtime::GpuSceneExecutorDiagnosticCode::None) {
                expect(false, label + " begins: " + diagnostic.message);
                return;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            auto poll = executor.poll();
            while (poll == bloom::runtime::GpuSceneExecutorPollResult::Pending &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
                poll = executor.poll();
            }
            expect(poll == bloom::runtime::GpuSceneExecutorPollResult::Ready,
                   label + " reaches Ready: " + executor.diagnostic().message);
            if (poll != bloom::runtime::GpuSceneExecutorPollResult::Ready)
                return;
            const auto* image = executor.image();
            expect(image != nullptr, label + " publishes a resident image");
            if (image != nullptr) {
                auto oracleRequest = requestAt(*plan, frame);
                oracleRequest.bypassOperationCache = true;
                const auto cpu = oracle.evaluate(plan, oracleRequest, {});
                const auto readback = bloom::render::readbackResidentImage(*image, kSceneBudget);
                expect(cpu.frame() != nullptr && static_cast<bool>(readback),
                       label + " has a CPU oracle and a test readback");
                if (cpu.frame() != nullptr && readback) {
                    const auto& pixels = cpu.frame()->processImage().pixels();
                    expect(
                        readback.pixels.size() == pixels.size() &&
                            bloom::gpu_coverage_native::pixelsClose(readback.pixels, pixels),
                        label + " matches the CPU oracle with exact coverage" +
                            bloom::gpu_coverage_native::pixelParityDetail(readback.pixels, pixels));
                }
            }
            (void)executor.takeImage();
        };

    const auto beforeCold = executor.counters();
    const auto coldStart = std::chrono::steady_clock::now();
    for (std::int64_t frame = 0; frame < kFrames; ++frame)
        runFrame(frame, scenes[static_cast<std::size_t>(frame)]);
    const auto coldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - coldStart)
                            .count();
    const auto afterCold = executor.counters();
    const auto coldOcio = afterCold.ocioEffectDispatches - beforeCold.ocioEffectDispatches;
    const auto programs = afterCold.ocioProgramCreations - beforeCold.ocioProgramCreations;
    expect(coldOcio == static_cast<std::uint64_t>(kFrames),
           "the cold pass dispatched the mask program once per keyed frame (" +
               std::to_string(coldOcio) + ")");
    expect(programs == 1, "the cold pass created exactly one native mask program (" +
                              std::to_string(programs) + ")");

    // Warm: the same range, freshly prepared as a RAM-preview refill would, is answered from the
    // output cache with no dispatch and no compile.
    const auto beforeWarmPrepare = preparer.counters();
    const auto beforeWarm = executor.counters();
    const auto warmStart = std::chrono::steady_clock::now();
    for (std::int64_t frame = 0; frame < kFrames; ++frame) {
        const auto prepared = builder.build(plan, requestAt(*plan, frame));
        expect(static_cast<bool>(prepared), "warm frame " + std::to_string(frame) + " prepares");
        if (prepared)
            runFrame(frame, prepared.scene);
    }
    const auto warmMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - warmStart)
                            .count();
    const auto afterWarm = executor.counters();
    const auto afterWarmPrepare = preparer.counters();
    const auto warmDispatches = afterWarm.dispatches - beforeWarm.dispatches;
    const auto warmHits = afterWarm.outputCacheHits - beforeWarm.outputCacheHits;
    const auto warmCompiles =
        afterWarmPrepare.shaderCompilerRuns - beforeWarmPrepare.shaderCompilerRuns;
    expect(warmDispatches == 0,
           "the warm pass dispatched nothing (" + std::to_string(warmDispatches) + ")");
    expect(warmHits >= static_cast<std::uint64_t>(kFrames),
           "every warm frame was served by the output cache (" + std::to_string(warmHits) + ")");
    expect(warmCompiles == 0, "the warm pass compiled nothing");
    expect(afterWarm.ocioProgramCreations == afterCold.ocioProgramCreations,
           "the warm pass created no native program");

    std::cout << "COLD frames=" << kFrames << " maskDispatches=" << coldOcio
              << " dispatches=" << (afterCold.dispatches - beforeCold.dispatches)
              << " nativePrograms=" << programs << " ms=" << coldMs << '\n';
    std::cout << "WARM frames=" << kFrames << " dispatches=" << warmDispatches
              << " outputCacheHits=" << warmHits << " shaderCompilerRuns=" << warmCompiles
              << " ms=" << warmMs << '\n';
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: 24 keyed mask frames, " << compilerRuns << " shader compile(s), "
              << programs << " native program, exact coverage parity, warm zero-dispatch\n";
    return 0;
}
