// Real-device proof for the media extension of the owner-thread GPU scene executor.
//
// Every fixture is a real CompiledCompositionPlan -> CpuGpuSceneBuilder -> GpuSceneExecutor on a
// real Vulkan device. The builder emits a GpuSceneUploadCommand carrying the already-decoded,
// colour-converted host image; the executor uploads it once per builder semantic source key with
// the real GpuImageUpload, then runs the existing translation / source-over / composition-output
// path. The result is compared to a genuine uncached CpuCompositionEvaluator frame for every output
// pixel and the actual returned native descriptor. The image is read back only by this test oracle.
//
// Gate: absolute-or-relative 2e-6 per finite component, matching the solid executor suite.

#include "gpu_media_scene_preparation_test_support.hpp"

#include "gpu_media_video_scene_preparation_test_support.hpp"

#include "gpu_media_executor_test_support.hpp"

#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>

#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
#include "gpu_scene_executor_fault_injection.hpp"
#endif

#include <bloom/core/color.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuImage;
using bloom::render::readbackResidentImage;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneExecutorBudgets;
using bloom::runtime::GpuSceneExecutorCounters;
using bloom::runtime::GpuSceneExecutorDiagnosticCode;
using bloom::runtime::GpuSceneExecutorJobState;
using bloom::runtime::GpuSceneExecutorPollResult;
using bloom::runtime::GpuSceneMediaContext;
using bloom::runtime::GpuSceneUploadCommand;
using bloom::runtime::PreparedGpuScene;

using bloom::runtime::media_executor_test::descriptorMatchesScene;
using bloom::runtime::media_executor_test::drainToTerminal;
using bloom::runtime::media_executor_test::kCacheBudget;
using bloom::runtime::media_executor_test::kReadbackBudget;
using bloom::runtime::media_executor_test::kSceneBudget;
using bloom::runtime::media_executor_test::Options;
using bloom::runtime::media_executor_test::parseOptions;
using bloom::runtime::media_executor_test::pixelsClose;
using bloom::runtime::media_executor_test::runScene;

using PlanPtr = std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>;

struct MediaFixture final {
    std::filesystem::path directory;
    std::filesystem::path path;
    bloom::document::AssetRecord asset;
};

[[nodiscard]] MediaFixture makeFixture() {
    MediaFixture fixture;
    fixture.directory = std::filesystem::temp_directory_path() / "bloom_gpu_media_executor_test";
    std::filesystem::remove_all(fixture.directory);
    std::filesystem::create_directories(fixture.directory);
    fixture.path = fixture.directory / "signed_hdr.exr";
    writeExrRgba(fixture.path, 3, 2, signedHdrPixels());
    fixture.asset = imageAsset(fixture.path, "signed_hdr", 900);
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

// Builds, runs and compares a media scene against the genuine uncached CPU evaluator frame.
bool expectMediaParity(Expectations& expectations, GpuSceneExecutor& executor,
                       const CpuCompositionEvaluator& evaluator, const CpuGpuSceneBuilder& builder,
                       const PlanPtr& plan, const EvaluationRequest& request,
                       const std::string& label) {
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), label + ": prepares");
    if (!prepared) {
        return false;
    }
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, label + ": CPU frame evaluates");
    if (frame.frame() == nullptr) {
        return false;
    }
    const auto& cpuImage = frame.frame()->processImage();
    expectations.expect(prepared.scene->outputDescriptor() == *cpuImage.descriptor(),
                        label + ": prepared output descriptor matches the CPU frame");
    expectations.expect(prepared.scene->processIdentity() == frame.frame()->identity(),
                        label + ": process identity matches the CPU frame");

    const auto run = runScene(executor, prepared.scene, kSceneBudget);
    expectations.expect(run.begin.code == GpuSceneExecutorDiagnosticCode::None,
                        label + ": executor accepts the scene");
    expectations.expect(run.ready, label + ": executor reaches Ready");
    if (!run.ready || run.image == nullptr) {
        std::cerr << label << ": diagnostic code=" << static_cast<int>(executor.diagnostic().code)
                  << " message=" << executor.diagnostic().message << '\n';
        return false;
    }
    expectations.expect(descriptorMatchesScene(*run.image, *prepared.scene),
                        label + ": actual native descriptor matches the scene");
    const auto readback = readbackResidentImage(*run.image, kReadbackBudget);
    expectations.expect(readback.hasValue(), label + ": test readback succeeds");
    if (!readback) {
        return false;
    }
    expectations.expect(readback.pixels.size() == cpuImage.pixels().size(),
                        label + ": pixel count matches the CPU frame");
    if (readback.pixels.size() != cpuImage.pixels().size()) {
        return false;
    }
    expectations.expect(pixelsClose(readback.pixels, cpuImage.pixels()),
                        label + ": every pixel is within the 2e-6 process gate");
    return true;
}

void testStillImageParity(Expectations& expectations, GpuSceneExecutor& executor,
                          const CpuCompositionEvaluator& evaluator, const MediaFixture& fixture) {
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);

    const auto fractional = mediaPlan(format(8, 8), fixture.asset,
                                      LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, 1000);
    expectMediaParity(expectations, executor, evaluator, builder, fractional,
                      requestFor(*fractional), "signed/HDR still fractional +0.3/+0.1");

    const auto negative = mediaPlan(format(8, 8), fixture.asset,
                                    LayerValues{.position = {2.7, 2.4}, .opacity = 0.625}, 1100);
    expectMediaParity(expectations, executor, evaluator, builder, negative, requestFor(*negative),
                      "fractional -0.3 / non-unit opacity");

    const auto integerGrid = mediaPlan(format(8, 8), fixture.asset,
                                       LayerValues{.position = {4.0, 3.0}, .opacity = 1.0}, 1200);
    expectMediaParity(expectations, executor, evaluator, builder, integerGrid,
                      requestFor(*integerGrid), "integer grid media raster");

    const auto odd = mediaPlan(format(7, 5), fixture.asset,
                               LayerValues{.position = {2.3, 1.4}, .opacity = 0.8}, 1250);
    expectMediaParity(expectations, executor, evaluator, builder, odd, requestFor(*odd),
                      "odd composition window / media origin");

    const auto extent = bloom::render::ImageExtent::create(5, 4);
    expectations.expect(static_cast<bool>(extent), "the proxy extent builds");
    if (extent) {
        const auto proxyPlan = mediaPlan(format(9, 6, *bloom::core::PixelAspectRatio::create(4, 3)),
                                         fixture.asset, LayerValues{.position = {4.5, 3.0}}, 1300);
        auto proxyRequest = requestFor(*proxyPlan);
        proxyRequest.resolution = bloom::runtime::ProxyResolution{*extent.value()};
        expectMediaParity(expectations, executor, evaluator, builder, proxyPlan, proxyRequest,
                          "media proxy / non-square PAR");
    }
    expectations.expect(executor.counters().uploads >= 1,
                        "a cold media scene dispatched a real upload");
}

// An unchanged source behind a changed transform is a content-cache hit: no re-upload, and the
// cached GPU source is the SAME resident image pointer.
void testChangedTransformReusesUpload(Expectations& expectations, GpuDevice& device,
                                      const CpuCompositionEvaluator& evaluator,
                                      const MediaFixture& fixture) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "reuse: cache created");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "reuse: executor created");
    if (!executor) {
        return;
    }
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);

    const auto planA = mediaPlan(format(8, 8), fixture.asset,
                                 LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, 1500);
    const auto planB = mediaPlan(format(8, 8), fixture.asset,
                                 LayerValues{.position = {2.7, 2.4}, .opacity = 1.0}, 1600);
    const auto a = builder.build(planA, requestFor(*planA));
    const auto b = builder.build(planB, requestFor(*planB));
    expectations.expect(a.hasValue() && b.hasValue(), "reuse: both scenes prepare");
    if (!a || !b) {
        return;
    }
    const auto* uploadA = firstUpload(*a.scene);
    const auto* uploadB = firstUpload(*b.scene);
    expectations.expect(uploadA != nullptr && uploadB != nullptr &&
                            uploadA->semanticKey == uploadB->semanticKey,
                        "reuse: the unchanged source keeps one upload key across transforms");
    if (uploadA == nullptr || uploadB == nullptr) {
        return;
    }

    const auto firstRun = runScene(*executor.executor, a.scene, kSceneBudget);
    expectations.expect(firstRun.ready, "reuse: the first transform completes");
    if (!firstRun.ready) {
        return;
    }
    const auto afterFirst = executor.executor->counters();
    expectations.expect(afterFirst.uploads == 1, "reuse: the first run uploads once");
    const auto cachedSource = cache.cache->find(uploadA->semanticKey);
    expectations.expect(cachedSource != nullptr, "reuse: the uploaded source is cached");
    if (cachedSource == nullptr) {
        return;
    }

    const auto secondRun = runScene(*executor.executor, b.scene, kSceneBudget);
    expectations.expect(secondRun.ready, "reuse: the changed transform completes");
    if (!secondRun.ready) {
        return;
    }
    const auto afterSecond = executor.executor->counters();
    expectations.expect(afterSecond.uploads == afterFirst.uploads,
                        "reuse: the changed transform performs no re-upload");
    expectations.expect(afterSecond.commandCacheHits > afterFirst.commandCacheHits,
                        "reuse: the changed transform serves the source from the content cache");
    expectations.expect(afterSecond.dispatches > afterFirst.dispatches,
                        "reuse: the changed transform still dispatches the transform/output");

    const auto cachedSourceAgain = cache.cache->find(uploadB->semanticKey);
    expectations.expect(cachedSourceAgain.get() == cachedSource.get(),
                        "reuse: the same GPU resident source pointer is reused");

    auto oracleRequest = requestFor(*planB);
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(planB, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, "reuse: the CPU frame evaluates");
    if (frame.frame() != nullptr && secondRun.image != nullptr) {
        const auto readback = readbackResidentImage(*secondRun.image, kReadbackBudget);
        expectations.expect(
            readback.hasValue() &&
                readback.pixels.size() == frame.frame()->processImage().pixels().size() &&
                pixelsClose(readback.pixels, frame.frame()->processImage().pixels()),
            "reuse: the reused source still yields every-pixel parity");
    }
}

// A changed source image, sequence frame or input colour interpretation is a new source key and a
// new upload; each still matches the CPU oracle.
void testChangedSourceInvalidates(Expectations& expectations, GpuDevice& device,
                                  const CpuCompositionEvaluator& evaluator,
                                  const MediaFixture& fixture) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(cache.hasValue() && executor.hasValue(), "invalidate: cache+executor");
    if (!cache || !executor) {
        return;
    }
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);

    const auto base = mediaPlan(format(8, 8), fixture.asset,
                                LayerValues{.position = {4.3, 3.1}, .opacity = 0.9}, 1700);
    expectations.expect(expectMediaParity(expectations, *executor.executor, evaluator, builder,
                                          base, requestFor(*base), "invalidate: base source"),
                        "invalidate: base source parity");
    const auto baseRebuilt = builder.build(base, requestFor(*base));
    const auto* baseKeys = firstUpload(*baseRebuilt.scene);
    expectations.expect(baseKeys != nullptr, "invalidate: the base upload key exists");
    if (baseKeys == nullptr) {
        return;
    }
    const auto uploadsAfterBase = executor.executor->counters().uploads;

    // Changed still image: a distinct EXR content digest.
    auto changedPixels = signedHdrPixels();
    for (auto& pixel : changedPixels) {
        pixel.green += 0.75F;
    }
    const auto changedPath = fixture.directory / "changed.exr";
    writeExrRgba(changedPath, 3, 2, changedPixels);
    const auto changedAsset = imageAsset(changedPath, "changed", 903);
    const auto changedPlan = mediaPlan(format(8, 8), changedAsset,
                                       LayerValues{.position = {4.3, 3.1}, .opacity = 0.9}, 1750);
    expectations.expect(expectMediaParity(expectations, *executor.executor, evaluator, builder,
                                          changedPlan, requestFor(*changedPlan),
                                          "invalidate: changed still image"),
                        "invalidate: changed still parity");
    const auto changedRebuilt = builder.build(changedPlan, requestFor(*changedPlan));
    const auto* changedKeys = firstUpload(*changedRebuilt.scene);
    expectations.expect(changedKeys != nullptr && changedKeys->semanticKey != baseKeys->semanticKey,
                        "invalidate: a changed still image changes the upload key");
    expectations.expect(executor.executor->counters().uploads > uploadsAfterBase,
                        "invalidate: a changed still image uploads again");
    const auto uploadsAfterChanged = executor.executor->counters().uploads;

    // Changed sequence frame: two distinct members resolved by time.
    auto secondPixels = signedHdrPixels();
    for (auto& pixel : secondPixels) {
        pixel.red += 0.5F;
        pixel.blue *= 0.5F;
    }
    const auto secondPath = fixture.directory / "sequence_1.exr";
    writeExrRgba(secondPath, 3, 2, secondPixels);
    auto sequenceAsset = fixture.asset;
    sequenceAsset.id = bloom::document::AssetId::fromRaw(901);
    sequenceAsset.kind = bloom::document::AssetKind::Sequence;
    sequenceAsset.manifest.pattern = "sequence.####.exr";
    sequenceAsset.manifest.padding = 4;
    sequenceAsset.manifest.first = 0;
    sequenceAsset.manifest.last = 1;
    sequenceAsset.manifest.members = {sequenceMember(fixture.path, 0, 0),
                                      sequenceMember(secondPath, 1, 1)};
    const auto sequencePlan = mediaPlan(format(8, 8), sequenceAsset,
                                        LayerValues{.position = {4.3, 3.1}, .opacity = 0.75}, 1800);
    const auto frameZero =
        builder.build(sequencePlan, requestFor(*sequencePlan, RationalTime::fromInteger(0)));
    const auto frameOne =
        builder.build(sequencePlan, requestFor(*sequencePlan, *RationalTime::create(1, 24)));
    expectations.expect(frameZero.hasValue() && frameOne.hasValue(), "invalidate: sequence builds");
    if (frameZero && frameOne) {
        const auto* uploadZero = firstUpload(*frameZero.scene);
        const auto* uploadOne = firstUpload(*frameOne.scene);
        expectations.expect(uploadZero != nullptr && uploadOne != nullptr &&
                                uploadZero->semanticKey != uploadOne->semanticKey,
                            "invalidate: a changed sequence frame changes the upload key");
    }
    expectations.expect(expectMediaParity(expectations, *executor.executor, evaluator, builder,
                                          sequencePlan,
                                          requestFor(*sequencePlan, *RationalTime::create(1, 24)),
                                          "invalidate: sequence frame 1"),
                        "invalidate: sequence frame parity");
    expectations.expect(executor.executor->counters().uploads > uploadsAfterChanged,
                        "invalidate: the new sequence frame uploads again");

    // Changed input colour interpretation: Auto -> Raw is a new converted source.
    auto definition = base->copyDefinition();
    auto* source = std::get_if<bloom::runtime::CompiledImageSource>(&definition.operations[0]);
    expectations.expect(source != nullptr && source->asset.has_value(),
                        "invalidate: the base plan has a media source");
    if (source != nullptr && source->asset.has_value()) {
        source->asset->interpretation.colorSpace = bloom::document::AssetColorSpace::Raw;
        const auto reinterpreted = publish(std::move(definition));
        const auto uploadsBeforeColour = executor.executor->counters().uploads;
        expectations.expect(expectMediaParity(expectations, *executor.executor, evaluator, builder,
                                              reinterpreted, requestFor(*reinterpreted),
                                              "invalidate: changed input colour"),
                            "invalidate: changed input colour parity");
        const auto reinterpretedRebuilt = builder.build(reinterpreted, requestFor(*reinterpreted));
        const auto* reinterpretedKeys = firstUpload(*reinterpretedRebuilt.scene);
        expectations.expect(reinterpretedKeys != nullptr &&
                                reinterpretedKeys->semanticKey != baseKeys->semanticKey,
                            "invalidate: a changed input colour changes the upload key");
        expectations.expect(executor.executor->counters().uploads > uploadsBeforeColour,
                            "invalidate: a changed input colour uploads again");
    }
}

// An unchanged scene is an output cache hit with ZERO native dispatches.
void testWarmOutputZeroDispatch(Expectations& expectations, GpuDevice& device,
                                const CpuCompositionEvaluator& evaluator,
                                const MediaFixture& fixture) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(cache.hasValue() && executor.hasValue(), "warm: cache+executor");
    if (!cache || !executor) {
        return;
    }
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan = mediaPlan(format(8, 8), fixture.asset,
                                LayerValues{.position = {4.3, 3.1}, .opacity = 0.9}, 1900);
    const auto firstPrepared = builder.build(plan, requestFor(*plan));
    expectations.expect(firstPrepared.hasValue(), "warm: the first scene prepares");
    if (!firstPrepared) {
        return;
    }
    const auto first = runScene(*executor.executor, firstPrepared.scene, kSceneBudget);
    expectations.expect(first.ready, "warm: the first scene completes");
    if (!first.ready) {
        return;
    }
    const auto afterFirst = executor.executor->counters();
    expectations.expect(afterFirst.outputCacheMisses == 1, "warm: the first run misses");
    expectations.expect(afterFirst.dispatches > 0 && afterFirst.uploads == 1,
                        "warm: the first run dispatches and uploads");

    const auto secondPrepared = builder.build(plan, requestFor(*plan));
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

// The cached-input live-pin budget refuses before any Vulkan work and leaves the executor usable.
void testCachedInputBudget(Expectations& expectations, GpuDevice& device,
                           const CpuCompositionEvaluator& evaluator, const MediaFixture& fixture) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(cache.hasValue() && executor.hasValue(), "budget: cache+executor");
    if (!cache || !executor) {
        return;
    }
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto warmPlan =
        mediaPlan(format(8, 8), fixture.asset, LayerValues{.position = {4.3, 3.1}}, 2000);
    const auto warmPrepared = builder.build(warmPlan, requestFor(*warmPlan));
    expectations.expect(warmPrepared.hasValue(), "budget: the warm scene prepares");
    if (!warmPrepared) {
        return;
    }
    const auto* warmUpload = firstUpload(*warmPrepared.scene);
    expectations.expect(warmUpload != nullptr, "budget: the warm upload key exists");
    if (warmUpload == nullptr) {
        return;
    }
    const auto warmRun = runScene(*executor.executor, warmPrepared.scene, kSceneBudget);
    expectations.expect(warmRun.ready, "budget: the warm scene completes");
    if (!warmRun.ready) {
        return;
    }
    const auto cached = cache.cache->find(warmUpload->semanticKey);
    expectations.expect(cached != nullptr, "budget: the source is cached");
    if (cached == nullptr) {
        return;
    }
    const std::uint64_t cachedBytes = cached->allocationBytes();
    expectations.expect(cachedBytes > 0, "budget: the cached source has real bytes");

    // A changed transform shares the source but misses the output; a budget below the cached input
    // bytes must be refused at begin with no Vulkan work.
    const auto changed =
        mediaPlan(format(8, 8), fixture.asset, LayerValues{.position = {2.7, 2.4}}, 2100);
    const auto changedPrepared = builder.build(changed, requestFor(*changed));
    expectations.expect(changedPrepared.hasValue(), "budget: the changed scene prepares");
    if (!changedPrepared) {
        return;
    }
    const auto tight = executor.executor->begin(changedPrepared.scene, cachedBytes - 1);
    expectations.expect(tight.code == GpuSceneExecutorDiagnosticCode::OverBudget,
                        "budget: a budget below the cached input pins is refused");
    expectations.expect(executor.executor->state() == GpuSceneExecutorJobState::Idle,
                        "budget: the refusal leaves the executor idle");
    const auto oneByte = executor.executor->begin(changedPrepared.scene, 1);
    expectations.expect(oneByte.code == GpuSceneExecutorDiagnosticCode::OverBudget,
                        "budget: a one-byte budget is refused");
    const auto accepted = runScene(*executor.executor, changedPrepared.scene, kSceneBudget);
    expectations.expect(accepted.ready, "budget: the executor is reusable after a refusal");
}

// Optional real-video vector: a genuinely decoded media-worker frame uploaded once, with a changed
// source frame a new upload. Runs only when a real fixture directory is supplied; it never skips as
// a pass.
void testRealVideo(Expectations& expectations, GpuSceneExecutor& executor,
                   CpuCompositionEvaluator& evaluator, const std::filesystem::path& fixtures) {
    if (fixtures.empty() || !std::filesystem::is_directory(fixtures)) {
        std::cout << "NOTE: real-video executor vector skipped (no --fixtures directory)\n";
        return;
    }
    const auto path = fixtures / "numbered-prores.mov";
    expectations.expect(std::filesystem::is_regular_file(path), "video: the ProRes fixture exists");
    if (!std::filesystem::is_regular_file(path)) {
        return;
    }
    evaluator.setAssetBaseDirectory(fixtures);
    evaluator.setVideoCacheByteBudget(std::size_t{64} << 20U);
    bloom::document::AssetRecord asset;
    try {
        asset = videoAsset(path, 26000);
    } catch (const std::exception& error) {
        expectations.expect(false, std::string{"video: fixture probe failed: "} + error.what());
        return;
    }

    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan = videoPlan(format(8, 8), asset, LayerValues{.position = {4.3, 3.1}}, 26000);
    expectMediaParity(expectations, executor, evaluator, builder, plan,
                      requestFor(*plan, RationalTime::fromInteger(0)), "video: frame 0");
    const auto uploadsBefore = executor.counters().uploads;
    expectMediaParity(expectations, executor, evaluator, builder, plan,
                      requestFor(*plan, *RationalTime::create(1, 24)),
                      "video: changed source frame");
    expectations.expect(executor.counters().uploads > uploadsBefore,
                        "video: a changed source frame uploads again");

    const auto zeroRebuilt = builder.build(plan, requestFor(*plan, RationalTime::fromInteger(0)));
    const auto oneRebuilt = builder.build(plan, requestFor(*plan, *RationalTime::create(1, 24)));
    const auto* uploadZero = firstUpload(*zeroRebuilt.scene);
    const auto* uploadOne = firstUpload(*oneRebuilt.scene);
    expectations.expect(uploadZero != nullptr && uploadOne != nullptr &&
                            uploadZero->semanticKey != uploadOne->semanticKey,
                        "video: the two source frames have distinct upload keys");
}

} // namespace

int main(const int argc, char** argv) {
    const Options options = parseOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
    Expectations expectations;
    CpuCompositionEvaluator evaluator;
    const auto fixture = makeFixture();
    evaluator.setAssetBaseDirectory(fixture.directory);

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

    auto parityCache = GpuSceneCache::create(*device.device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(parityCache.hasValue(), "the parity cache is created");
    if (!parityCache) {
        std::cerr << "FAIL: the parity cache could not be created\n";
        return 1;
    }
    auto parityExecutor = GpuSceneExecutor::create(*device.device, *parityCache.cache);
    expectations.expect(parityExecutor.hasValue(), "the parity executor is created");
    if (!parityExecutor) {
        std::cerr << "FAIL: the parity executor could not be created\n";
        return 1;
    }

    testStillImageParity(expectations, *parityExecutor.executor, evaluator, fixture);
    testChangedTransformReusesUpload(expectations, *device.device, evaluator, fixture);
    testChangedSourceInvalidates(expectations, *device.device, evaluator, fixture);
    testWarmOutputZeroDispatch(expectations, *device.device, evaluator, fixture);
    testCachedInputBudget(expectations, *device.device, evaluator, fixture);
    testRealVideo(expectations, *parityExecutor.executor, evaluator, options.fixtures);

    if (!expectations.ok()) {
        std::cerr << "FAIL: GPU media executor expectations failed\n";
        return 1;
    }
    std::cout << "PASS: GPU media executor\n";
    return 0;
}
