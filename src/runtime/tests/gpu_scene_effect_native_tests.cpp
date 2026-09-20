// Real native proof that the production CpuGpuSceneBuilder emits genuine GPU OCIO image-effect
// commands (non-identity CST and, on Linux, a real isolated FileTransform) that the production
// GpuSceneExecutor runs on the device, with the readback compared against the unchanged
// CpuCompositionEvaluator oracle under the documented strict 2e-6 absolute-or-relative gate,
// byte-exact alpha, and the CPU process descriptor (data/display window and pixel aspect).
//
// The scene is built by the REAL production builder from a real compiled plan; no hand-built
// PreparedGpuScene is used. A warm unchanged run must perform zero new OCIO dispatch, and a changed
// effect must invalidate only the effect while its upstream solid stays cached.
//
// Without a device the test reports an explicit SKIP (exit 77); --require-device makes that a
// failure. An explicit --loader selects the Vulkan loader. Without the pinned tools the real arms
// are skipped.

#include "gpu_composite_native_support.hpp"

#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/input_color_context.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
using bloom::document::AssetId;
using bloom::document::AssetKind;
using bloom::document::AssetRecord;
using bloom::document::CompositionFormat;
using bloom::document::CompositionId;
using bloom::document::LayerId;
using bloom::document::LayerSlotId;
using bloom::document::NodeId;
using bloom::document::ParameterId;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::ImageWindow;
using bloom::render::readbackResidentImage;
using bloom::render::Rgba32f;
using bloom::render::composite_proof::Expectations;
using bloom::render::composite_proof::Options;
using bloom::render::composite_proof::parseOptions;
using bloom::runtime::CompiledCompositionOutput;
using bloom::runtime::CompiledCompositionPlan;
using bloom::runtime::CompiledCompositionPlanDefinition;
using bloom::runtime::CompiledImageEffect;
using bloom::runtime::CompiledLayerOutput;
using bloom::runtime::CompiledMerge;
using bloom::runtime::CompiledMergeInput;
using bloom::runtime::CompiledOperation;
using bloom::runtime::CompiledScalarParameter;
using bloom::runtime::CompiledSolid;
using bloom::runtime::CompiledVec2Parameter;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::CstKernel;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::FileTransformKernel;
using bloom::runtime::GpuOcioCompileOptions;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneExecutorDiagnosticCode;
using bloom::runtime::GpuSceneExecutorPollResult;
using bloom::runtime::GpuSceneMediaContext;
using bloom::runtime::GpuSceneOcioContext;
using bloom::runtime::OperationIndex;

constexpr std::uint64_t kRevision = 7;
constexpr auto kProject = bloom::document::ProjectId::fromRaw(1);
constexpr std::uint64_t kSceneBudget = 64ULL * 1024ULL * 1024ULL;

[[nodiscard]] CompositionFormat format(const std::uint32_t width, const std::uint32_t height) {
    const auto value = CompositionFormat::create(width, height);
    if (!value.has_value()) {
        throw std::logic_error("native effect fixture format is invalid");
    }
    return *value;
}

// A solid -> OCIO effect -> layer -> Normal merge -> output plan. The solid is negative-HDR with a
// translucent alpha so the effect's HDR and alpha handling are genuinely exercised.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
effectPlan(const std::variant<CstKernel, FileTransformKernel>& kernel, const std::uint64_t idBase,
           const std::optional<AssetRecord>& lutAsset = std::nullopt,
           const Color4d color = Color4d{-0.25, 1.75, 0.5, 0.625}) {
    constexpr std::uint64_t layerRaw = 1;
    std::vector<CompiledOperation> operations;
    operations.emplace_back(CompiledSolid{NodeId::fromRaw(idBase + 1),
                                          {ParameterId::fromRaw(idBase + 2), color},
                                          {ParameterId::fromRaw(idBase + 3), 12.0},
                                          {ParameterId::fromRaw(idBase + 4), 10.0}});
    if (const auto* cst = std::get_if<CstKernel>(&kernel)) {
        operations.emplace_back(CompiledImageEffect{
            NodeId::fromRaw(idBase + 5), OperationIndex::fromRaw(0), *cst, false, false});
    } else {
        const auto* file = std::get_if<FileTransformKernel>(&kernel);
        FileTransformKernel copy = *file;
        copy.asset = lutAsset;
        operations.emplace_back(CompiledImageEffect{NodeId::fromRaw(idBase + 5),
                                                    OperationIndex::fromRaw(0), std::move(copy),
                                                    false, false});
    }
    operations.emplace_back(CompiledLayerOutput{
        NodeId::fromRaw(idBase + 6), LayerId::fromRaw(layerRaw), OperationIndex::fromRaw(1),
        CompiledVec2Parameter{ParameterId::fromRaw(idBase + 7), bloom::document::Vec2d{6.0, 5.0}},
        CompiledVec2Parameter{ParameterId::fromRaw(idBase + 8), bloom::document::Vec2d{0.0, 0.0}},
        CompiledVec2Parameter{ParameterId::fromRaw(idBase + 9), bloom::document::Vec2d{1.0, 1.0}},
        CompiledScalarParameter{ParameterId::fromRaw(idBase + 10), 0.0},
        CompiledScalarParameter{ParameterId::fromRaw(idBase + 11), 1.0},
        ParameterId::fromRaw(idBase + 12), bloom::core::kDefaultBlendMode});
    operations.emplace_back(CompiledMerge{
        NodeId::fromRaw(idBase + 13),
        std::vector<CompiledMergeInput>{CompiledMergeInput{LayerSlotId::fromRaw(idBase + 14),
                                                           LayerId::fromRaw(layerRaw),
                                                           OperationIndex::fromRaw(2)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 15), OperationIndex::fromRaw(3)});
    CompiledCompositionPlanDefinition definition{bloom::document::Revision::fromRaw(kRevision),
                                                 kProject,
                                                 CompositionId::fromRaw(idBase + 100),
                                                 format(12, 10),
                                                 std::move(operations),
                                                 OperationIndex::fromRaw(4)};
    definition.duration = RationalTime::fromInteger(100);
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

// RGB within the strict 2e-6 abs-or-rel gate; alpha byte-exact.
[[nodiscard]] bool parity(const std::vector<Rgba32f>& actual,
                          const std::span<const Rgba32f> expected, std::string& detail) {
    if (actual.size() != expected.size()) {
        detail = "size";
        return false;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i].alpha() != expected[i].alpha()) {
            detail = "alpha at " + std::to_string(i);
            return false;
        }
        for (std::size_t c = 0; c < 3; ++c) {
            const float a = actual[i].components()[c];
            const float e = expected[i].components()[c];
            if (!std::isfinite(a) || !std::isfinite(e)) {
                detail = "non-finite at " + std::to_string(i);
                return false;
            }
            const float scale = std::max(std::abs(a), std::abs(e));
            if (std::abs(a - e) > 2e-6F * std::max(1.0F, scale)) {
                detail = "rgb c" + std::to_string(c) + " at " + std::to_string(i) + " got " +
                         std::to_string(a) + " want " + std::to_string(e);
                return false;
            }
        }
    }
    return true;
}

#ifndef BLOOM_GPUSHADER_TOOLS_DIR
int runNoTools() {
    std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
    return 77;
}
#else
[[nodiscard]] GpuSceneOcioContext ocioContext() {
    GpuOcioCompileOptions options;
    options.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    options.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    GpuSceneOcioContext context;
    context.preparer = std::make_shared<GpuOcioProgramPreparer>();
    context.compileOptions = options;
    return context;
}

void runCst(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache,
            const std::string& from, const std::string& to) {
    const auto context = ocioContext();
    const auto planA = effectPlan(CstKernel{from, to}, 1000);
    const CpuGpuSceneBuilder builder(nullptr, {}, context);
    const auto requestA = requestFor(*planA);
    const auto preparedA = builder.build(planA, requestA);
    expectations.expect(preparedA.hasValue(), "effect native: the real CST scene prepares");
    if (!preparedA) {
        std::cerr << "prepare diagnostic: " << preparedA.diagnostic.message << '\n';
        return;
    }
    const CpuCompositionEvaluator evaluator;
    auto oracleRequest = requestA;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(planA, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, "effect native: the CPU oracle evaluates");
    if (!frame.frame()) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, cache);
    expectations.expect(executor.hasValue(), "effect native: the executor creates");
    if (!executor) {
        return;
    }

    const auto cold = runScene(*executor.executor, preparedA.scene);
    expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready,
                        "effect native: the cold run completes: " +
                            executor.executor->diagnostic().message);
    if (cold.pollResult != GpuSceneExecutorPollResult::Ready) {
        return;
    }
    const auto coldCounters = executor.executor->counters();
    expectations.expect(coldCounters.ocioEffectDispatches == 1,
                        "effect native: the cold run dispatches exactly one OCIO effect");
    expectations.expect(coldCounters.readbacks == 0,
                        "effect native: the executor performs no full-frame readback");
    std::string detail;
    expectations.expect(parity(cold.pixels, frame.frame()->processImage().pixels(), detail),
                        "effect native: GPU pixels match the CPU oracle (2e-6, exact alpha): " +
                            detail);
    const auto& oracleDescriptor = *frame.frame()->processImage().descriptor();
    expectations.expect(cold.dataWindow.has_value() &&
                            *cold.dataWindow == oracleDescriptor.dataWindow() &&
                            cold.displayWindow.has_value() &&
                            *cold.displayWindow == oracleDescriptor.displayWindow() &&
                            cold.pixelAspect == oracleDescriptor.pixelAspect(),
                        "effect native: the GPU descriptor matches the CPU process descriptor");

    // Warm unchanged scene: zero new OCIO dispatch.
    const auto warm = runScene(*executor.executor, preparedA.scene);
    expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready,
                        "effect native: the warm run completes");
    const auto warmCounters = executor.executor->counters();
    expectations.expect(warmCounters.ocioEffectDispatches == coldCounters.ocioEffectDispatches,
                        "effect native: a warm unchanged effect runs zero new OCIO dispatch");
    expectations.expect(parity(warm.pixels, frame.frame()->processImage().pixels(), detail),
                        "effect native: warm pixels still match the CPU oracle");

    // A changed upstream solid invalidates only its own command: the OCIO program is reused (no new
    // native program) but the effect re-dispatches over the new input.
    const auto planC =
        effectPlan(CstKernel{from, to}, 3000, std::nullopt, Color4d{0.4, -0.5, 0.9, 0.25});
    const auto requestC = requestFor(*planC);
    const auto preparedC = builder.build(planC, requestC);
    expectations.expect(preparedC.hasValue(), "effect native: the changed scene prepares");
    if (preparedC) {
        const auto changed = runScene(*executor.executor, preparedC.scene);
        expectations.expect(changed.pollResult == GpuSceneExecutorPollResult::Ready,
                            "effect native: the changed-upstream run completes");
        const auto changedCounters = executor.executor->counters();
        expectations.expect(changedCounters.ocioEffectDispatches ==
                                warmCounters.ocioEffectDispatches + 1,
                            "effect native: a changed upstream re-dispatches the effect");
        expectations.expect(changedCounters.ocioProgramCreations ==
                                coldCounters.ocioProgramCreations,
                            "effect native: a changed upstream reuses the prepared OCIO program");
        expectations.expect(changedCounters.solidDispatches > coldCounters.solidDispatches,
                            "effect native: the changed solid really re-dispatches");
    }
}

#if defined(__linux__)
void runFileTransform(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache,
                      const std::string& working, const std::string& process) {
    const auto base = std::filesystem::temp_directory_path() / "bloom_gpu_effect_native";
    std::error_code directoryError;
    std::filesystem::create_directories(base, directoryError);
    expectations.expect(!directoryError, "effect native: the LUT fixture directory is creatable");
    if (directoryError) {
        return;
    }
    const auto path = base / "curve.cube";
    {
        std::ofstream file(path);
        file << "TITLE \"test\"\nLUT_1D_SIZE 4\n";
        file << "0.0 0.0 0.0\n0.2 0.3 0.4\n0.6 0.5 0.7\n1.0 1.0 1.0\n";
    }
    const auto resource = bloom::color::readLutFile(path);
    expectations.expect(resource.error == bloom::color::LutError::None,
                        "effect native: the LUT fixture reads");
    if (resource.error != bloom::color::LutError::None) {
        return;
    }
    AssetRecord asset;
    asset.id = AssetId::fromRaw(77);
    asset.kind = AssetKind::Lut;
    asset.locator.path = "curve.cube";
    asset.locator.relinkHint = "file://curve.cube";
    asset.contentDigest = resource.digest;
    FileTransformKernel file;
    file.lutAssetId = asset.id;

    const auto context = ocioContext();
    GpuSceneMediaContext media;
    media.assetBaseDirectory = base;
    const auto plan = effectPlan(file, 5000, asset);
    const CpuGpuSceneBuilder builder(nullptr, media, context);
    const auto request = requestFor(*plan);
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), "effect native: the FileTransform scene prepares");
    if (!prepared) {
        std::cerr << "FileTransform prepare diagnostic: " << prepared.diagnostic.message << '\n';
        return;
    }
    const CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(base);
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr,
                        "effect native: the FileTransform oracle evaluates");
    if (!frame.frame()) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, cache);
    expectations.expect(executor.hasValue(), "effect native: the FileTransform executor creates");
    if (!executor) {
        return;
    }
    const auto run = runScene(*executor.executor, prepared.scene);
    expectations.expect(run.pollResult == GpuSceneExecutorPollResult::Ready,
                        "effect native: the FileTransform run completes: " +
                            executor.executor->diagnostic().message);
    if (run.pollResult != GpuSceneExecutorPollResult::Ready) {
        return;
    }
    expectations.expect(executor.executor->counters().ocioEffectDispatches == 1,
                        "effect native: the FileTransform dispatches one OCIO effect");
    std::string detail;
    expectations.expect(parity(run.pixels, frame.frame()->processImage().pixels(), detail),
                        "effect native: FileTransform pixels match the CPU oracle: " + detail);
    static_cast<void>(working);
    static_cast<void>(process);
}
#endif
#endif

void runNative(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache,
               const std::string& from, const std::string& to) {
#ifndef BLOOM_GPUSHADER_TOOLS_DIR
    static_cast<void>(expectations);
    static_cast<void>(device);
    static_cast<void>(cache);
    static_cast<void>(from);
    static_cast<void>(to);
#else
    runCst(expectations, device, cache, from, to);
#if defined(__linux__)
    runFileTransform(expectations, device, cache, from, to);
#endif
#endif
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            std::cerr << "invalid arguments\n";
            return 2;
        }
#ifndef BLOOM_GPUSHADER_TOOLS_DIR
        return runNoTools();
#else
        const auto config = bloom::runtime::detail::resolveInputColorConfig(
            bloom::runtime::EvaluationColorIntent::LinearRec709Scene);
        if (!config.has_value()) {
            std::cout << "SKIP: the Bloom Neutral OCIO config is unavailable\n";
            return 77;
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
        expectations.expect(cache.hasValue(), "effect native: the scene cache creates");
        if (!cache) {
            return 1;
        }
        runNative(expectations, *device.device, *cache.cache,
                  std::string{config->processColorSpaceId()},
                  std::string{config->sRgbTextureColorSpaceId()});
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " effect native expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: GPU image-effect executor vs CPU oracle\n";
        return 0;
#endif
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
