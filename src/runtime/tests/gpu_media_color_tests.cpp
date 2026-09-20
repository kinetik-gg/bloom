// The bounded media-source GPU colour path end to end.
//
// A real AP0 (ACES2065-1 tagged) OpenEXR fixture is resolved through the production media pipeline.
// The production CpuGpuSceneBuilder (with the shared GpuSceneOcioContext) emits a RAW full-source
// upload plus a real input->working OCIO ProcessEffect command; the production GpuSceneExecutor
// runs it on a real device and the result is compared to the genuine, unchanged
// CpuCompositionEvaluator frame at the documented 2e-6 gate. There is no CPU OCIO pass on the GPU
// route: the source is decoded raw (file decode only) and the colour transform is the dispatched
// OCIO effect.
//
// Also asserted: the upload carries the FULL source dimensions and the composition display
// window/pixel aspect; the decode/upload identity is independent of the working space (a changed
// working space reuses the decoded upload); a warm build performs no decode and a warm executor run
// performs zero native dispatches; a missing/corrupt source fails with an honest diagnostic and no
// partial scene.
//
// Without the pinned shader tools the test prints SKIP. Without a device it skips cleanly unless
// --require-device is passed.

#include "gpu_media_executor_test_support.hpp"
#include "gpu_media_scene_preparation_test_support.hpp"
#include "gpu_media_video_scene_preparation_test_support.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

namespace document = bloom::document;

using bloom::render::ImageWindow;
using bloom::render::Rgba32fImage;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::EvaluationColorIntent;
using bloom::runtime::GpuOcioCompileOptions;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneExecutorBudgets;
using bloom::runtime::GpuSceneExecutorCounters;
using bloom::runtime::GpuSceneExecutorDiagnosticCode;
using bloom::runtime::GpuSceneMediaContext;
using bloom::runtime::GpuSceneOcioContext;
using bloom::runtime::GpuSceneOcioEffectCommand;
using bloom::runtime::GpuSceneUploadCommand;
using bloom::runtime::PreparedGpuScene;
using bloom::runtime::ProxyResolution;

using bloom::runtime::media_executor_test::descriptorMatchesScene;
using bloom::runtime::media_executor_test::kCacheBudget;
using bloom::runtime::media_executor_test::kReadbackBudget;
using bloom::runtime::media_executor_test::kSceneBudget;
using bloom::runtime::media_executor_test::Options;
using bloom::runtime::media_executor_test::parseOptions;
using bloom::runtime::media_executor_test::pixelsClose;
using bloom::runtime::media_executor_test::runScene;

constexpr std::string_view kAcesWorking = "ACEScg";
constexpr std::string_view kAcesAlternateWorking = "ACES2065-1";
constexpr std::string_view kAcesTextureInput = "sRGB - Texture";
[[maybe_unused]] constexpr int kSkipExit = 77;

[[nodiscard]] bloom::runtime::CancellationToken makeCancelledToken();

struct Fixture final {
    std::filesystem::path directory;
    std::filesystem::path path;
    document::AssetRecord asset;
};

[[nodiscard]] Fixture makeAcesFixture() {
    Fixture fixture;
    fixture.directory = std::filesystem::temp_directory_path() / "bloom_gpu_media_color_test";
    std::filesystem::remove_all(fixture.directory);
    std::filesystem::create_directories(fixture.directory);
    fixture.path = fixture.directory / "aces_ap0.exr";
    writeExrRgbaWithChromaticities(fixture.path, 3, 2, signedHdrPixels(), acesAp0Chromaticities());
    fixture.asset = imageAsset(fixture.path, "aces_ap0", 700);
    return fixture;
}

[[nodiscard]] const GpuSceneUploadCommand* firstUpload(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* upload = std::get_if<GpuSceneUploadCommand>(&command)) {
            return upload;
        }
    }
    return nullptr;
}

[[nodiscard]] const GpuSceneOcioEffectCommand* firstOcio(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* ocio = std::get_if<GpuSceneOcioEffectCommand>(&command)) {
            return ocio;
        }
    }
    return nullptr;
}

[[nodiscard]] const bloom::runtime::GpuScenePointResampleCommand*
firstResample(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* resample =
                std::get_if<bloom::runtime::GpuScenePointResampleCommand>(&command)) {
            return resample;
        }
    }
    return nullptr;
}

[[nodiscard]] bloom::runtime::EvaluationRequest acesRequest(const CompiledCompositionPlan& plan,
                                                            const std::string_view working) {
    auto request = requestFor(plan);
    request.colorIntent = EvaluationColorIntent{
        .workingColorSpaceId = working,
        .ocioConfigRevision = {},
        .ocioConfigUri = bloom::color::kAcesCgV1ConfigUri,
    };
    return request;
}

[[nodiscard]] bool expectMediaParity(Expectations& expectations, GpuSceneExecutor& executor,
                                     const CpuCompositionEvaluator& evaluator,
                                     const CpuGpuSceneBuilder& builder,
                                     const std::shared_ptr<const CompiledCompositionPlan>& plan,
                                     const bloom::runtime::EvaluationRequest& request,
                                     const std::uint64_t sourceWidth,
                                     const std::uint64_t sourceHeight, const std::string& label) {
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), label + ": prepares");
    if (!prepared) {
        return false;
    }
    const auto* upload = firstUpload(*prepared.scene);
    const auto* ocio = firstOcio(*prepared.scene);
    expectations.expect(upload != nullptr && ocio != nullptr,
                        label + ": emits a raw upload and a real OCIO colour command");
    if (upload == nullptr || ocio == nullptr) {
        return false;
    }
    expectations.expect(prepared.scene->mediaStatistics().ocioCommandPreparations == 1 &&
                            prepared.scene->mediaStatistics().uploadCacheMisses == 1 &&
                            prepared.scene->mediaStatistics().uploadCacheHits == 0,
                        label + ": the cold build decodes raw and prepares one OCIO command");
    const auto sourceWindow = ImageWindow::create(0, 0, sourceWidth, sourceHeight);
    expectations.expect(sourceWindow && upload->descriptor.dataWindow() == *sourceWindow.value(),
                        label + ": the upload keeps the full source dimensions");
    expectations.expect(
        upload->descriptor.displayWindow() == prepared.scene->outputDescriptor().displayWindow() &&
            upload->descriptor.pixelAspect() == prepared.scene->outputDescriptor().pixelAspect(),
        label + ": the upload keeps the composition display window and PAR");
    expectations.expect(ocio->inputKey == upload->semanticKey && ocio->program != nullptr &&
                            ocio->program->encoding() ==
                                bloom::runtime::GpuOcioOutputEncoding::FinalRgba32f,
                        label + ": the OCIO command consumes the raw upload as a ProcessEffect");
    expectations.expect(ocio->semanticKey != upload->semanticKey &&
                            ocio->outputWindow == upload->descriptor.dataWindow(),
                        label + ": the effect identity is independent of the decode identity");

    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, label + ": the CPU oracle evaluates");
    if (frame.frame() == nullptr) {
        return false;
    }
    expectations.expect(prepared.scene->processIdentity() == frame.frame()->identity(),
                        label + ": process identity matches the CPU frame");
    expectations.expect(prepared.scene->outputDescriptor() ==
                            *frame.frame()->processImage().descriptor(),
                        label + ": output descriptor matches the CPU frame");

    const auto run = runScene(executor, prepared.scene, kSceneBudget);
    if (!run.ready) {
        std::cerr << label
                  << ": executor diagnostic code=" << static_cast<int>(executor.diagnostic().code)
                  << " message=" << executor.diagnostic().message << '\n';
    }
    expectations.expect(run.ready, label + ": the executor reaches Ready");
    if (!run.ready || run.image == nullptr) {
        return false;
    }
    expectations.expect(descriptorMatchesScene(*run.image, *prepared.scene),
                        label + ": the native descriptor matches the scene");
    const auto counters = executor.counters();
    expectations.expect(counters.ocioEffectDispatches >= 1,
                        label + ": the colour conversion was a dispatched GPU OCIO effect");
    const auto readback = bloom::render::readbackResidentImage(*run.image, kReadbackBudget);
    expectations.expect(readback.hasValue(), label + ": the output reads back for the oracle");
    if (!readback) {
        return false;
    }
    const auto& cpuImage = frame.frame()->processImage();
    expectations.expect(readback.pixels.size() == cpuImage.pixels().size(),
                        label + ": the pixel count matches the CPU frame");
    if (readback.pixels.size() != cpuImage.pixels().size()) {
        return false;
    }
    expectations.expect(pixelsClose(readback.pixels, cpuImage.pixels()),
                        label + ": every pixel is within the 2e-6 process gate");
    return true;
}

void testColdBuilderToExecutor(Expectations& expectations, bloom::render::GpuDevice& device,
                               const CpuCompositionEvaluator& evaluator, const Fixture& fixture,
                               const GpuSceneOcioContext& ocioContext) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(device, *cache.cache, GpuSceneExecutorBudgets{});
    expectations.expect(cache.hasValue() && executor.hasValue(), "cold: cache and executor host");
    if (!cache || !executor) {
        return;
    }
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    const auto plan = mediaPlan(format(8, 8), fixture.asset,
                                LayerValues{.position = {4.3, 3.1}, .opacity = 0.9}, 4100);
    const auto request = acesRequest(*plan, kAcesWorking);
    expectations.expect(expectMediaParity(expectations, *executor.executor, evaluator, builder,
                                          plan, request, 3, 2, "ACES2065-1 -> ACEScg"),
                        "cold: ACES input-to-working parity");

    // Warm builder: the raw upload is reused and no decode happens.
    const auto warm = builder.build(plan, request);
    expectations.expect(warm.hasValue(), "warm: rebuild prepares");
    if (warm) {
        expectations.expect(warm.scene->mediaStatistics().uploadCacheHits == 1 &&
                                warm.scene->mediaStatistics().imageConversions == 0,
                            "warm: the decoded raw upload is reused without a re-decode");
        const auto run = runScene(*executor.executor, warm.scene, kSceneBudget);
        const auto counters = executor.executor->counters();
        expectations.expect(run.ready, "warm: the executor serves the cached scene");
        expectations.expect(counters.outputCacheHits >= 1, "warm: the unchanged output is a hit");
    }
}

// A proxied non-identity still: a full-resolution raw upload plus a GPU point-resample to the proxy
// window plus the OCIO transform over that window, compared to the CPU oracle at 2e-6. Asserts the
// exact command sequence, the small-proxy output window, a positive point-resample dispatch on a
// cold build, and zero additional resample/OCIO dispatch on the warm cached run.
void testProxiedStill(Expectations& expectations, bloom::render::GpuDevice& device,
                      const CpuCompositionEvaluator& evaluator, const Fixture& fixture,
                      const GpuSceneOcioContext& ocioContext) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(device, *cache.cache, GpuSceneExecutorBudgets{});
    expectations.expect(cache.hasValue() && executor.hasValue(), "proxy: cache and executor host");
    if (!cache || !executor) {
        return;
    }
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    const auto plan = mediaPlan(format(9, 6), fixture.asset,
                                LayerValues{.position = {4.5, 3.0}, .opacity = 0.9}, 4600);
    // A 6x4 proxy over a 9x6 composition: non-unit vertical/horizontal scales.
    const auto extent = bloom::render::ImageExtent::create(6, 4);
    expectations.expect(static_cast<bool>(extent), "proxy: the proxy extent builds");
    if (!extent) {
        return;
    }
    auto request = acesRequest(*plan, kAcesWorking);
    request.resolution = ProxyResolution{*extent.value()};

    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), "proxy: prepares");
    if (!prepared) {
        return;
    }
    const auto* upload = firstUpload(*prepared.scene);
    const auto* resample = firstResample(*prepared.scene);
    const auto* ocio = firstOcio(*prepared.scene);
    expectations.expect(upload != nullptr && resample != nullptr && ocio != nullptr,
                        "proxy: emits upload -> point-resample -> OCIO");
    if (upload == nullptr || resample == nullptr || ocio == nullptr) {
        return;
    }
    // The raw upload stays at the FULL source dimensions (3x2) even under a 6x4 proxy.
    const auto sourceWindow = ImageWindow::create(0, 0, 3, 2);
    expectations.expect(sourceWindow && upload->descriptor.dataWindow() == *sourceWindow.value(),
                        "proxy: the raw upload keeps the full source dimensions");
    // The proxy output is max(1, ceil(3 * 6/9)) x max(1, ceil(2 * 4/6)) = 2 x 2.
    const auto proxyWindow = ImageWindow::create(0, 0, 2, 2);
    expectations.expect(proxyWindow && resample->outputWindow == *proxyWindow.value(),
                        "proxy: the resample output is the CPU proxy window");
    expectations.expect(resample->inputKey == upload->semanticKey &&
                            resample->sourceWindow == upload->descriptor.dataWindow() &&
                            resample->horizontalScale > 0.0 && resample->horizontalScale < 1.0 &&
                            resample->verticalScale > 0.0 && resample->verticalScale < 1.0,
                        "proxy: the resample consumes the full-resolution upload at fractional "
                        "scales");

    // A changed proxy is a different resample/output but must NOT re-decode: the raw upload
    // identity is independent of the proxy scales and display descriptor.
    {
        const auto otherExtent = bloom::render::ImageExtent::create(3, 2);
        if (otherExtent) {
            auto otherRequest = acesRequest(*plan, kAcesWorking);
            otherRequest.resolution = ProxyResolution{*otherExtent.value()};
            const auto otherPrepared = builder.build(plan, otherRequest);
            const auto* otherUpload = otherPrepared ? firstUpload(*otherPrepared.scene) : nullptr;
            const auto* otherResample =
                otherPrepared ? firstResample(*otherPrepared.scene) : nullptr;
            expectations.expect(
                otherPrepared && otherUpload != nullptr && otherResample != nullptr &&
                    otherUpload->semanticKey == upload->semanticKey &&
                    otherPrepared.scene->mediaStatistics().uploadCacheHits == 1 &&
                    otherPrepared.scene->mediaStatistics().uploadCacheMisses == 0 &&
                    otherPrepared.scene->mediaStatistics().imageConversions == 0 &&
                    otherResample->semanticKey != resample->semanticKey,
                "proxy: a changed proxy reuses the decoded raw upload without a re-decode");
        }
    }
    expectations.expect(ocio->inputKey == resample->semanticKey &&
                            ocio->outputWindow == resample->outputWindow,
                        "proxy: the OCIO transform runs over the proxy window");

    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, "proxy: the CPU oracle evaluates");
    if (frame.frame() == nullptr) {
        return;
    }
    expectations.expect(prepared.scene->outputDescriptor() ==
                            *frame.frame()->processImage().descriptor(),
                        "proxy: output descriptor matches the CPU frame");

    const auto run = runScene(*executor.executor, prepared.scene, kSceneBudget);
    if (!run.ready) {
        std::cerr << "proxy: executor diagnostic code="
                  << static_cast<int>(executor.executor->diagnostic().code)
                  << " message=" << executor.executor->diagnostic().message << '\n';
    }
    expectations.expect(run.ready, "proxy: the executor reaches Ready");
    if (!run.ready || run.image == nullptr) {
        return;
    }
    const auto counters = executor.executor->counters();
    expectations.expect(counters.pointResampleDispatches >= 1,
                        "proxy: the cold build dispatched a GPU point-resample");
    expectations.expect(counters.ocioEffectDispatches >= 1,
                        "proxy: the OCIO transform was dispatched on the GPU");
    const auto readback = bloom::render::readbackResidentImage(*run.image, kReadbackBudget);
    expectations.expect(readback.hasValue(), "proxy: the output reads back for the oracle");
    if (!readback) {
        return;
    }
    const auto& cpuImage = frame.frame()->processImage();
    expectations.expect(readback.pixels.size() == cpuImage.pixels().size(),
                        "proxy: the pixel count matches the CPU frame");
    if (readback.pixels.size() != cpuImage.pixels().size()) {
        return;
    }
    expectations.expect(pixelsClose(readback.pixels, cpuImage.pixels()),
                        "proxy: every pixel is within the 2e-6 process gate");
    // Alpha is carried through both the exact nearest gather and the OCIO CST unchanged, so it must
    // match the CPU oracle bit for bit (not merely within a tolerance).
    {
        bool alphaExact = true;
        for (std::size_t i = 0; i < readback.pixels.size(); ++i) {
            if (readback.pixels[i].alpha() != cpuImage.pixels()[i].alpha()) {
                alphaExact = false;
                break;
            }
        }
        expectations.expect(alphaExact, "proxy: every alpha lane is exactly the CPU oracle's");
    }
    // No host per-pixel resampling: the only resampling command is the dispatched GPU
    // PointResampleV1, and the raw upload that feeds it is the full-resolution source.
    expectations.expect(prepared.scene->mediaStatistics().ocioCommandPreparations == 1 &&
                            counters.pointResampleDispatches == 1 &&
                            counters.ocioEffectDispatches == 1,
                        "proxy: exactly one GPU point-resample and one GPU OCIO dispatch ran");

    // Warm: the same scene is served from the content cache with zero additional dispatches.
    const auto warmPrepared = builder.build(plan, request);
    const auto warm = runScene(*executor.executor, warmPrepared.scene, kSceneBudget);
    expectations.expect(warm.ready, "proxy: the warm scene serves from cache");
    const auto warmCounters = executor.executor->counters();
    expectations.expect(warmCounters.pointResampleDispatches == counters.pointResampleDispatches &&
                            warmCounters.ocioEffectDispatches == counters.ocioEffectDispatches,
                        "proxy: a warm scene performs zero additional resample/OCIO dispatches");

    // Tiny budget: a proxy scene whose full-resolution upload is larger than the budget is refused
    // at begin without any native work, and the executor stays reusable.
    const auto tightPrepared = builder.build(plan, request);
    const auto tight = executor.executor->begin(tightPrepared.scene, 1);
    expectations.expect(tight.code == GpuSceneExecutorDiagnosticCode::OverBudget,
                        "proxy: a one-byte budget is refused");
    expectations.expect(executor.executor->state() ==
                            bloom::runtime::GpuSceneExecutorJobState::Idle,
                        "proxy: the budget refusal leaves the executor idle");
}

// An identity (input colour space == working colour space) proxied still under a configured GPU
// colour context: no OCIO program is prepared, but the full-resolution raw upload still goes
// through the native PointResampleV1 gather. This is the identity/no-OCIO proxy path the split must
// not leave on the CPU.
void testProxiedIdentityStill(Expectations& expectations, bloom::render::GpuDevice& device,
                              const CpuCompositionEvaluator& evaluator, const Fixture& fixture,
                              const GpuSceneOcioContext& ocioContext) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(device, *cache.cache, GpuSceneExecutorBudgets{});
    expectations.expect(cache.hasValue() && executor.hasValue(),
                        "identity proxy: cache and executor host");
    if (!cache || !executor) {
        return;
    }
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    document::AssetRecord asset = fixture.asset;
    // Explicit input == working space: the resolved transform is exact identity, so no OCIO command
    // is emitted even though a preparer is configured.
    asset.interpretation.inputColorSpaceId = std::string{kAcesWorking};
    const auto plan =
        mediaPlan(format(9, 6), asset, LayerValues{.position = {4.5, 3.0}, .opacity = 0.9}, 4750);
    const auto extent = bloom::render::ImageExtent::create(6, 4);
    expectations.expect(static_cast<bool>(extent), "identity proxy: extent builds");
    if (!extent) {
        return;
    }
    auto request = acesRequest(*plan, kAcesWorking);
    request.resolution = ProxyResolution{*extent.value()};

    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), "identity proxy: prepares");
    if (!prepared) {
        return;
    }
    const auto* upload = firstUpload(*prepared.scene);
    const auto* resample = firstResample(*prepared.scene);
    const auto* ocio = firstOcio(*prepared.scene);
    expectations.expect(upload != nullptr && resample != nullptr && ocio == nullptr,
                        "identity proxy: full-source upload + native point-resample, no OCIO");
    if (upload == nullptr || resample == nullptr || ocio != nullptr) {
        return;
    }
    const auto sourceWindow = ImageWindow::create(0, 0, 3, 2);
    expectations.expect(sourceWindow && upload->descriptor.dataWindow() == *sourceWindow.value(),
                        "identity proxy: the raw upload keeps the full source dimensions");
    const auto proxyWindow = ImageWindow::create(0, 0, 2, 2);
    expectations.expect(proxyWindow && resample->outputWindow == *proxyWindow.value(),
                        "identity proxy: the resample output is the CPU proxy window");
    expectations.expect(resample->inputKey == upload->semanticKey &&
                            resample->displayWindow ==
                                prepared.scene->outputDescriptor().displayWindow(),
                        "identity proxy: the resample carries the composition display window");

    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, "identity proxy: the CPU oracle evaluates");
    if (frame.frame() == nullptr) {
        return;
    }
    expectations.expect(prepared.scene->outputDescriptor() ==
                            *frame.frame()->processImage().descriptor(),
                        "identity proxy: output descriptor matches the CPU frame");

    const auto run = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(run.ready, "identity proxy: the executor reaches Ready");
    if (!run.ready || run.image == nullptr) {
        return;
    }
    const auto counters = executor.executor->counters();
    expectations.expect(counters.pointResampleDispatches == 1 && counters.ocioEffectDispatches == 0,
                        "identity proxy: exactly one GPU point-resample, zero OCIO dispatches");
    const auto readback = bloom::render::readbackResidentImage(*run.image, kReadbackBudget);
    expectations.expect(readback.hasValue(), "identity proxy: the output reads back");
    if (!readback) {
        return;
    }
    const auto& cpuImage = frame.frame()->processImage();
    expectations.expect(readback.pixels.size() == cpuImage.pixels().size() &&
                            pixelsClose(readback.pixels, cpuImage.pixels()),
                        "identity proxy: every pixel is within the 2e-6 process gate");
    {
        bool alphaExact = true;
        for (std::size_t i = 0; i < readback.pixels.size() && i < cpuImage.pixels().size(); ++i) {
            if (readback.pixels[i].alpha() != cpuImage.pixels()[i].alpha()) {
                alphaExact = false;
                break;
            }
        }
        expectations.expect(alphaExact, "identity proxy: every alpha lane is exactly the CPU's");
    }

    const auto warmPrepared = builder.build(plan, request);
    const auto warm = runScene(*executor.executor, warmPrepared.scene, kSceneBudget);
    const auto warmCounters = executor.executor->counters();
    expectations.expect(
        warm.ready && warmCounters.pointResampleDispatches == counters.pointResampleDispatches,
        "identity proxy: a warm scene performs zero additional resample dispatches");
}

void testProxyCancellation(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                           const Fixture& fixture, const GpuSceneOcioContext& ocioContext) {
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    const auto plan = mediaPlan(format(9, 6), fixture.asset,
                                LayerValues{.position = {4.5, 3.0}, .opacity = 0.9}, 4700);
    const auto extent = bloom::render::ImageExtent::create(6, 4);
    if (!extent) {
        return;
    }
    auto request = acesRequest(*plan, kAcesWorking);
    request.resolution = ProxyResolution{*extent.value()};
    const auto token = makeCancelledToken();
    expectations.expect(token.isCancellationRequested(), "proxy cancel: the token is cancelled");
    const auto prepared = builder.build(plan, request, token);
    expectations.expect(prepared.diagnostic.code ==
                                bloom::runtime::PreparedGpuSceneDiagnosticCode::Cancelled &&
                            !prepared.hasValue(),
                        "proxy cancel: a pre-cancelled request publishes no scene");
}

void testChangedWorkingSpaceReusesDecode(Expectations& expectations,
                                         const CpuCompositionEvaluator& evaluator,
                                         const Fixture& fixture,
                                         const GpuSceneOcioContext& ocioContext) {
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    // An explicit texture input keeps the transform non-identity for both working spaces, so the
    // comparison isolates the working space (the decoded bytes are identical).
    document::AssetRecord asset = fixture.asset;
    asset.interpretation.inputColorSpaceId = std::string{kAcesTextureInput};
    const auto plan =
        mediaPlan(format(8, 8), asset, LayerValues{.position = {4.3, 3.1}, .opacity = 0.9}, 4200);
    const auto first = builder.build(plan, acesRequest(*plan, kAcesWorking));
    const auto second = builder.build(plan, acesRequest(*plan, kAcesAlternateWorking));
    expectations.expect(first.hasValue() && second.hasValue(),
                        "working space: both builds prepare");
    if (!first || !second) {
        return;
    }
    const auto* uploadFirst = firstUpload(*first.scene);
    const auto* uploadSecond = firstUpload(*second.scene);
    const auto* ocioFirst = firstOcio(*first.scene);
    const auto* ocioSecond = firstOcio(*second.scene);
    expectations.expect(uploadFirst != nullptr && uploadSecond != nullptr && ocioFirst != nullptr &&
                            ocioSecond != nullptr,
                        "working space: both scenes emit a raw upload and an effect");
    if (uploadFirst == nullptr || uploadSecond == nullptr || ocioFirst == nullptr ||
        ocioSecond == nullptr) {
        return;
    }
    expectations.expect(uploadFirst->semanticKey == uploadSecond->semanticKey &&
                            second.scene->mediaStatistics().uploadCacheHits == 1 &&
                            second.scene->mediaStatistics().uploadCacheMisses == 0,
                        "working space: a changed working space reuses the decoded raw upload");
    expectations.expect(ocioFirst->semanticKey != ocioSecond->semanticKey,
                        "working space: the changed transform has a distinct effect identity");
}

// A live, already-cancelled CancellationToken. The only public source of one is a real
// TaskScheduler task (cancellation.hpp), so this runs one, copies its token, cancels, and lets it
// finish.
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
        TaskRequest("gpu media colour cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(88)}),
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

// Real decoded video (planar8 and alpha16) through the production builder -> executor -> CPU
// oracle, with an explicit ACES2065-1 -> ACEScg input-to-working CST. Cold/warm decode reuse,
// changed-working-space reuse, missing/corrupt diagnostics and pre-cancellation are all checked.
void testRealVideoColour(Expectations& expectations, bloom::render::GpuDevice& device,
                         CpuCompositionEvaluator& evaluator, const std::filesystem::path& fixtures,
                         const GpuSceneOcioContext& ocioContext) {
    if (fixtures.empty() || !std::filesystem::is_directory(fixtures)) {
        std::cout << "NOTE: real-video colour vectors skipped (no --fixtures directory)\n";
        return;
    }
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(device, *cache.cache, GpuSceneExecutorBudgets{});
    expectations.expect(cache.hasValue() && executor.hasValue(),
                        "video: the cache and executor host");
    if (!cache || !executor) {
        return;
    }
    evaluator.setAssetBaseDirectory(fixtures);
    evaluator.setVideoCacheByteBudget(std::size_t{64} << 20U);
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    const std::array<std::pair<std::string_view, std::string_view>, 2> cases{
        std::pair<std::string_view, std::string_view>{"numbered-prores.mov", "planar8"},
        std::pair<std::string_view, std::string_view>{"alpha-prores.mov", "alpha16"}};
    std::uint64_t idBase = 4500;
    for (const auto& [file, label] : cases) {
        const std::string tag{label};
        const auto path = fixtures / std::string{file};
        expectations.expect(std::filesystem::is_regular_file(path),
                            tag + ": the real decoded fixture exists");
        if (!std::filesystem::is_regular_file(path)) {
            continue;
        }
        document::AssetRecord asset;
        try {
            asset = videoAsset(path, idBase + 100);
        } catch (const std::exception& error) {
            expectations.expect(false, tag + ": fixture probe failed: " + error.what());
            continue;
        }
        asset.interpretation.inputColorSpaceId = std::string{"ACES2065-1"};
        const auto centre = LayerValues{.position = {static_cast<double>(asset.width) * 0.5,
                                                     static_cast<double>(asset.height) * 0.5},
                                        .opacity = 0.9};
        const auto plan = videoPlan(format(asset.width, asset.height), asset, centre, idBase);
        const auto request = acesRequest(*plan, kAcesWorking);
        expectations.expect(expectMediaParity(expectations, *executor.executor, evaluator, builder,
                                              plan, request, asset.width, asset.height,
                                              tag + " video ACES"),
                            tag + ": video input-to-working parity");

        const auto warm = builder.build(plan, request);
        expectations.expect(warm.hasValue() && warm.scene->mediaStatistics().uploadCacheHits == 1 &&
                                warm.scene->mediaStatistics().videoConversions == 0,
                            tag + ": the decoded video frame is reused without a re-decode");

        // A proxied video frame: the raw upload stays full-resolution, and the GPU gathers the
        // proxy before the OCIO transform. The CPU proxy oracle is the unchanged evaluator frame.
        const auto proxyExtent =
            bloom::render::ImageExtent::create((static_cast<std::uint64_t>(asset.width) + 1) / 2,
                                               (static_cast<std::uint64_t>(asset.height) + 1) / 2);
        expectations.expect(static_cast<bool>(proxyExtent), tag + ": proxy extent builds");
        if (proxyExtent) {
            auto proxyRequest = acesRequest(*plan, kAcesWorking);
            proxyRequest.resolution = ProxyResolution{*proxyExtent.value()};
            const auto proxyPrepared = builder.build(plan, proxyRequest);
            const auto* proxyUpload = proxyPrepared ? firstUpload(*proxyPrepared.scene) : nullptr;
            const auto* proxyResample =
                proxyPrepared ? firstResample(*proxyPrepared.scene) : nullptr;
            const auto* proxyOcio = proxyPrepared ? firstOcio(*proxyPrepared.scene) : nullptr;
            expectations.expect(proxyUpload != nullptr && proxyResample != nullptr &&
                                    proxyOcio != nullptr,
                                tag + ": a proxied frame emits upload -> point-resample -> OCIO");
            if (proxyUpload != nullptr && proxyResample != nullptr && proxyOcio != nullptr) {
                const auto sourceWindow = ImageWindow::create(0, 0, asset.width, asset.height);
                expectations.expect(
                    sourceWindow && proxyUpload->descriptor.dataWindow() == *sourceWindow.value(),
                    tag + ": the proxied raw upload keeps the full frame dimensions");
                expectations.expect(proxyOcio->outputWindow == proxyResample->outputWindow,
                                    tag + ": the proxied OCIO runs over the proxy window");
                auto proxyOracle = proxyRequest;
                proxyOracle.bypassOperationCache = true;
                const auto proxyFrame = evaluator.evaluate(plan, proxyOracle, {});
                const auto proxyRun =
                    runScene(*executor.executor, proxyPrepared.scene, kSceneBudget);
                expectations.expect(proxyRun.ready, tag + ": the proxied frame completes");
                if (proxyFrame.frame() != nullptr && proxyRun.ready && proxyRun.image != nullptr) {
                    const auto proxyReadback =
                        bloom::render::readbackResidentImage(*proxyRun.image, kReadbackBudget);
                    expectations.expect(
                        proxyReadback.hasValue() &&
                            proxyReadback.pixels.size() ==
                                proxyFrame.frame()->processImage().pixels().size() &&
                            pixelsClose(proxyReadback.pixels,
                                        proxyFrame.frame()->processImage().pixels()),
                        tag + ": the proxied frame matches the CPU oracle at 2e-6");
                }
            }
        }

        // An identity (input == working) proxied video frame: no OCIO program, but the full-source
        // raw upload still goes through the native PointResampleV1 gather.
        if (proxyExtent) {
            document::AssetRecord identityAsset = asset;
            identityAsset.interpretation.inputColorSpaceId = std::string{kAcesWorking};
            const auto identityPlan =
                videoPlan(format(asset.width, asset.height), identityAsset, centre, idBase + 4);
            auto identityRequest = acesRequest(*identityPlan, kAcesWorking);
            identityRequest.resolution = ProxyResolution{*proxyExtent.value()};
            const auto identityPrepared = builder.build(identityPlan, identityRequest);
            const auto* identityUpload =
                identityPrepared ? firstUpload(*identityPrepared.scene) : nullptr;
            const auto* identityResample =
                identityPrepared ? firstResample(*identityPrepared.scene) : nullptr;
            const auto* identityOcio =
                identityPrepared ? firstOcio(*identityPrepared.scene) : nullptr;
            expectations.expect(identityUpload != nullptr && identityResample != nullptr &&
                                    identityOcio == nullptr,
                                tag + ": an identity proxied frame emits upload + resample, no "
                                      "OCIO");
            if (identityPrepared && identityUpload != nullptr && identityResample != nullptr) {
                auto identityOracle = identityRequest;
                identityOracle.bypassOperationCache = true;
                const auto identityFrame = evaluator.evaluate(identityPlan, identityOracle, {});
                const auto identityRun =
                    runScene(*executor.executor, identityPrepared.scene, kSceneBudget);
                expectations.expect(identityRun.ready,
                                    tag + ": the identity proxied frame completes");
                if (identityFrame.frame() != nullptr && identityRun.ready &&
                    identityRun.image != nullptr) {
                    const auto identityReadback =
                        bloom::render::readbackResidentImage(*identityRun.image, kReadbackBudget);
                    expectations.expect(
                        identityReadback.hasValue() &&
                            identityReadback.pixels.size() ==
                                identityFrame.frame()->processImage().pixels().size() &&
                            pixelsClose(identityReadback.pixels,
                                        identityFrame.frame()->processImage().pixels()),
                        tag + ": the identity proxied frame matches the CPU oracle at 2e-6");
                }
            }
        }

        document::AssetRecord reuseAsset = asset;
        reuseAsset.interpretation.inputColorSpaceId = std::string{kAcesTextureInput};
        const auto reusePlan =
            videoPlan(format(asset.width, asset.height), reuseAsset, centre, idBase + 1);
        const auto first = builder.build(reusePlan, acesRequest(*reusePlan, kAcesWorking));
        const auto second =
            builder.build(reusePlan, acesRequest(*reusePlan, kAcesAlternateWorking));
        const auto* uploadFirst = first ? firstUpload(*first.scene) : nullptr;
        const auto* uploadSecond = second ? firstUpload(*second.scene) : nullptr;
        const auto* ocioFirst = first ? firstOcio(*first.scene) : nullptr;
        const auto* ocioSecond = second ? firstOcio(*second.scene) : nullptr;
        expectations.expect(
            uploadFirst != nullptr && uploadSecond != nullptr &&
                uploadFirst->semanticKey == uploadSecond->semanticKey && second.hasValue() &&
                second.scene->mediaStatistics().uploadCacheHits == 1 && ocioFirst != nullptr &&
                ocioSecond != nullptr && ocioFirst->semanticKey != ocioSecond->semanticKey,
            tag + ": a changed working space reuses the decoded video frame");

        document::AssetRecord missing = asset;
        missing.locator.path = "missing.mov";
        missing.locator.relinkHint = "file:" + (fixtures / "missing.mov").string();
        const auto missingPlan =
            videoPlan(format(asset.width, asset.height), missing, centre, idBase + 2);
        const auto missingResult =
            builder.build(missingPlan, acesRequest(*missingPlan, kAcesWorking));
        expectations.expect(!missingResult.hasValue() && !missingResult.diagnostic.message.empty(),
                            tag + ": missing media fails closed with a diagnostic");

        const auto corruptPath =
            std::filesystem::temp_directory_path() /
            ("bloom_gpu_media_color_corrupt_" + std::to_string(idBase) + ".mov");
        {
            std::ofstream corrupt(corruptPath, std::ios::binary | std::ios::trunc);
            corrupt << "not a movie";
        }
        document::AssetRecord corrupt = asset;
        corrupt.locator.path = corruptPath.filename().string();
        corrupt.locator.relinkHint = "file:" + corruptPath.string();
        const auto corruptPlan =
            videoPlan(format(asset.width, asset.height), corrupt, centre, idBase + 3);
        const auto corruptResult =
            builder.build(corruptPlan, acesRequest(*corruptPlan, kAcesWorking));
        expectations.expect(!corruptResult.hasValue() && !corruptResult.diagnostic.message.empty(),
                            tag + ": corrupt media fails closed with a diagnostic");
        std::error_code ignored;
        std::filesystem::remove(corruptPath, ignored);
        idBase += 10;
    }
}

void testVideoCancellation(Expectations& expectations, const std::filesystem::path& fixtures,
                           const GpuSceneOcioContext& ocioContext) {
    if (fixtures.empty() || !std::filesystem::is_directory(fixtures)) {
        return;
    }
    const auto path = fixtures / "numbered-prores.mov";
    if (!std::filesystem::is_regular_file(path)) {
        return;
    }
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(fixtures);
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    document::AssetRecord asset;
    try {
        asset = videoAsset(path, 4700);
    } catch (const std::exception&) {
        return;
    }
    asset.interpretation.inputColorSpaceId = std::string{"ACES2065-1"};
    const auto plan = videoPlan(format(asset.width, asset.height), asset,
                                LayerValues{.position = {static_cast<double>(asset.width) * 0.5,
                                                         static_cast<double>(asset.height) * 0.5}},
                                4700);
    const auto token = makeCancelledToken();
    expectations.expect(token.isCancellationRequested(), "video cancel: the token is cancelled");
    const auto prepared = builder.build(plan, acesRequest(*plan, kAcesWorking), token);
    expectations.expect(prepared.diagnostic.code ==
                                bloom::runtime::PreparedGpuSceneDiagnosticCode::Cancelled &&
                            !prepared.hasValue(),
                        "video cancel: a pre-cancelled request publishes no scene");
}

void testMissingAndCorruptMedia(Expectations& expectations, const Fixture& fixture,
                                const GpuSceneOcioContext& ocioContext) {
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(fixture.directory);
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);

    // Missing file: a valid asset record whose locator points at a file that is not there.
    const auto missingPath = fixture.directory / "missing.exr";
    document::AssetRecord missingAsset = fixture.asset;
    missingAsset.locator.path = missingPath.filename().string();
    missingAsset.locator.relinkHint = "file:" + missingPath.string();
    const auto missingPlan =
        mediaPlan(format(8, 8), missingAsset, LayerValues{.position = {4.3, 3.1}}, 4300);
    const auto missing = builder.build(missingPlan, acesRequest(*missingPlan, kAcesWorking));
    expectations.expect(!missing.hasValue(), "missing media: preparation fails closed");
    expectations.expect(missing.diagnostic.code ==
                                bloom::runtime::PreparedGpuSceneDiagnosticCode::MediaUnavailable &&
                            !missing.diagnostic.message.empty(),
                        "missing media: the diagnostic is honest and non-empty");

    // Corrupt file: an existing file whose bytes are no longer a valid EXR.
    const auto corruptPath = fixture.directory / "corrupt.exr";
    {
        std::ofstream corrupt(corruptPath, std::ios::binary | std::ios::trunc);
        corrupt << "not an exr";
    }
    document::AssetRecord corruptAsset = fixture.asset;
    corruptAsset.locator.path = corruptPath.filename().string();
    corruptAsset.locator.relinkHint = "file:" + corruptPath.string();
    const auto corruptPlan =
        mediaPlan(format(8, 8), corruptAsset, LayerValues{.position = {4.3, 3.1}}, 4400);
    const auto corrupt = builder.build(corruptPlan, acesRequest(*corruptPlan, kAcesWorking));
    expectations.expect(!corrupt.hasValue(), "corrupt media: preparation fails closed");
    expectations.expect(!corrupt.diagnostic.message.empty(),
                        "corrupt media: the diagnostic is honest and non-empty");
}

} // namespace

int main(const int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
#ifndef BLOOM_GPUSHADER_TOOLS_DIR
        std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
        return kSkipExit;
#else
        Expectations expectations;
        const auto fixture = makeAcesFixture();
        CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(fixture.directory);
        auto preparer = std::make_shared<GpuOcioProgramPreparer>();
        GpuOcioCompileOptions compileOptions;
        compileOptions.glslangValidatorPath =
            std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
        compileOptions.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
        GpuSceneOcioContext ocioContext;
        ocioContext.preparer = preparer;
        ocioContext.compileOptions = compileOptions;

        testChangedWorkingSpaceReusesDecode(expectations, evaluator, fixture, ocioContext);
        testMissingAndCorruptMedia(expectations, fixture, ocioContext);
        testProxyCancellation(expectations, evaluator, fixture, ocioContext);
        testVideoCancellation(expectations, options.fixtures, ocioContext);

        bloom::render::GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = bloom::render::GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "NOTE: no compatible Vulkan device; the native route was not executed\n";
        } else {
            testColdBuilderToExecutor(expectations, *device.device, evaluator, fixture,
                                      ocioContext);
            testProxiedStill(expectations, *device.device, evaluator, fixture, ocioContext);
            testProxiedIdentityStill(expectations, *device.device, evaluator, fixture, ocioContext);
            testRealVideoColour(expectations, *device.device, evaluator, options.fixtures,
                                ocioContext);
        }

        if (!expectations.ok()) {
            std::cerr << "FAIL: GPU media colour expectations failed\n";
            return 1;
        }
        std::cout << "PASS: GPU media colour\n";
        return 0;
#endif
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
