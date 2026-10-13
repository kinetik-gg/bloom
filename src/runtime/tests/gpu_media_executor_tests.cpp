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

#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>

#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
#include "gpu_scene_executor_fault_injection.hpp"
#endif

#include <bloom/core/color.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_processing_phase_budget.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
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
        const auto proxyPlan = mediaPlan(format(9, 6, pixelAspect(4, 3)), fixture.asset,
                                         LayerValues{.position = {4.5, 3.0}}, 1300);
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
    const auto planB =
        mediaPlan(format(8, 8), fixture.asset,
                  LayerValues{.position = {2.7, 2.4}, .rotation = 7.25, .opacity = 1.0}, 1600);
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
    expectations.expect(afterFirst.nativeBatches > 0 &&
                            afterFirst.nativeSubmissions < afterFirst.dispatches,
                        "reuse: cold uploaded media downstream kernels share real submissions");
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
    expectations.expect(
        afterSecond.affineDispatches > afterFirst.affineDispatches &&
            afterSecond.nativeBatches > afterFirst.nativeBatches &&
            afterSecond.nativeSubmissions - afterFirst.nativeSubmissions <
                afterSecond.dispatches - afterFirst.dispatches,
        "reuse: edited affine/source-over batch pins the cached media source and reduces submits");

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
    const auto unchanged = runScene(*executor.executor, b.scene, kSceneBudget);
    const auto afterUnchanged = executor.executor->counters();
    expectations.expect(unchanged.ready &&
                            afterUnchanged.nativeSubmissions == afterSecond.nativeSubmissions &&
                            afterUnchanged.dispatches == afterSecond.dispatches &&
                            afterUnchanged.uploads == afterSecond.uploads,
                        "reuse: unchanged edited media uses cached output with zero submissions");
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
        builder.build(sequencePlan, requestFor(*sequencePlan, rationalTime(1, 24)));
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
                                          requestFor(*sequencePlan, rationalTime(1, 24)),
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
    bloom::render::GpuProcessingPhaseBudget root(1);
    root.setHostCapacity(1ULL << 30, true);
    root.setDeviceCapacity(1ULL << 30, true);
    std::shared_ptr<bloom::render::GpuProcessingPhaseLease> phase;
    bool retired = false;
    struct PhaseGuard final {
        GpuDevice& device;
        std::shared_ptr<bloom::render::GpuProcessingPhaseLease>& phase;
        bool& retired;
        ~PhaseGuard() {
            if (phase)
                device.retireProcessingPhase(phase->id(), retired);
        }
    } guard{device, phase, retired};
    const auto admit = [&](std::uint64_t uploadBytes = 1ULL << 20) {
        auto grant = root.reserve({.fixedHostBytes = 1ULL << 20,
                                   .requestedSourceBytes = uploadBytes,
                                   .minimumSourceBytes = uploadBytes,
                                   .requestedExecutorBytes = 16ULL << 20,
                                   .minimumExecutorBytes = 16ULL << 20,
                                   .capacityShareDenominator = 1});
        if (!grant.lease || !device.retainProcessingPhase(grant.lease))
            throw std::runtime_error("warm upload max1 exact root admission failed");
        phase = std::move(grant.lease);
        retired = false;
    };
    admit(1); // Deliberately short U, while native/output grants remain sufficient.
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
    const auto refused = runScene(*executor.executor, firstPrepared.scene, kSceneBudget);
    const auto metadata = device.nativeBufferStorageRefusal();
    const bool proven =
        bloom::runtime::gpuSceneExecutorBudgetRefusalRetired(*executor.executor, refused.finalPoll);
    expectations.expect(!refused.ready && proven && metadata &&
                            metadata->domain == bloom::render::GpuProcessingPhaseDomain::Host &&
                            metadata->phaseId == phase->id() &&
                            root.snapshot().ownedNativeBuffers == 0 && !phase->isUnproven(),
                        "actual executor U refusal preserves typed current metadata without CPU "
                        "fallback or latch poison");
    if (!proven || !metadata)
        throw std::runtime_error("required actual upload refusal metadata/proof absent");
    const auto refusalRevision = metadata->revisionAfterOwnRelease;
    device.retireProcessingPhase(phase->id(), true);
    phase.reset();
    expectations.expect(
        root.snapshot().admittedPhases == 0 &&
            root.snapshot().admissionRevision == refusalRevision &&
            root.snapshot().totalHostClaimBytes == 0 && root.snapshot().totalDeviceClaimBytes == 0,
        "own refused phase releases once at exact watermark before immediate max1 recovery");
    admit(); // Only the next request's U estimate changes; root capacities remain unchanged.
    const auto beforeCold = executor.executor->counters();
    auto first = runScene(*executor.executor, firstPrepared.scene, kSceneBudget);
    expectations.expect(first.ready, "warm: the first scene completes");
    if (!first.ready) {
        return;
    }
    const auto afterFirst = executor.executor->counters();
    expectations.expect(afterFirst.outputCacheMisses == beforeCold.outputCacheMisses + 1,
                        "warm: the first run misses");
    expectations.expect(afterFirst.dispatches > 0 && afterFirst.uploads == 1,
                        "warm: the first run dispatches and uploads");

    retired = true;
    const auto cold = root.snapshot();
    expectations.expect(cold.ownedNativeBufferBytes == 0 && cold.ownedNativeBuffers == 0 &&
                            cold.ownedNativeImageBytes > 0 && cold.ownedNativeImages > 0,
                        "warm rooted cache retains native images after staging is really freed");
    std::weak_ptr<bloom::render::GpuProcessingPhaseLease> oldPhase = phase;
    device.retireProcessingPhase(phase->id(), true);
    phase.reset();
    expectations.expect(oldPhase.expired() && root.snapshot().admittedPhases == 0 &&
                            root.snapshot().ownedNativeImageBytes == cold.ownedNativeImageBytes,
                        "cached uploaded images do not retain the originating request phase");
    admit(); // Immediate admission under the same maxConcurrentRequests=1 root, without delay.
    const auto beforeWarm = root.snapshot();
    const auto secondPrepared = builder.build(plan, requestFor(*plan));
    auto second = runScene(*executor.executor, secondPrepared.scene, kSceneBudget);
    expectations.expect(second.ready, "warm: the second scene completes");
    const auto afterSecond = executor.executor->counters();
    expectations.expect(afterSecond.outputCacheHits == 1,
                        "warm: the unchanged output is a cache hit");
    expectations.expect(afterSecond.dispatches == afterFirst.dispatches,
                        "warm: an unchanged output performs zero native dispatches");
    expectations.expect(second.image != nullptr && first.image != nullptr &&
                            second.image.get() == first.image.get(),
                        "warm: the returned image is the cached resident image");
    retired = second.ready && !executor.executor->hasUnretiredSubmission() &&
              !executor.executor->ownerDrainRequired();
    const auto warm = root.snapshot();
    expectations.expect(warm.ownedNativeBufferBytes == 0 && warm.ownedNativeBuffers == 0 &&
                            warm.ownedNativeImageBytes == beforeWarm.ownedNativeImageBytes &&
                            warm.ownedNativeImages == beforeWarm.ownedNativeImages &&
                            warm.totalHostClaimBytes == beforeWarm.totalHostClaimBytes &&
                            warm.totalDeviceClaimBytes == beforeWarm.totalDeviceClaimBytes &&
                            afterSecond.uploads == afterFirst.uploads,
                        "warm output adds no upload, staging, native claim or grant drift");
    first.image.reset();
    second.image.reset();
    executor.executor.reset();
    cache.cache->clear();
    device.retireProcessingPhase(phase->id(), retired);
    phase.reset();
    const auto final = root.snapshot();
    expectations.expect(final.ownedNativeImages == 0 && final.ownedNativeBuffers == 0 &&
                            final.totalHostClaimBytes == 0 && final.totalDeviceClaimBytes == 0 &&
                            final.admittedPhases == 0 && final.unprovenPhases == 0,
                        "actual cache/native destruction releases every covered claim once");
    std::cout << "UPLOAD_CACHE nativeBytes=" << cold.ownedNativeImageBytes
              << " count=" << cold.ownedNativeImages << " staging0/phase0/max1/warm0/final0\n";
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
                      requestFor(*plan, rationalTime(1, 24)), "video: changed source frame");
    expectations.expect(executor.counters().uploads > uploadsBefore,
                        "video: a changed source frame uploads again");

    const auto zeroRebuilt = builder.build(plan, requestFor(*plan, RationalTime::fromInteger(0)));
    const auto oneRebuilt = builder.build(plan, requestFor(*plan, rationalTime(1, 24)));
    const auto* uploadZero = firstUpload(*zeroRebuilt.scene);
    const auto* uploadOne = firstUpload(*oneRebuilt.scene);
    expectations.expect(uploadZero != nullptr && uploadOne != nullptr &&
                            uploadZero->semanticKey != uploadOne->semanticKey,
                        "video: the two source frames have distinct upload keys");
}

} // namespace

// The reported graph at full source size: a real 4608x3164 EXR over an FHD solid and text, decoded,
// prepared, uploaded and dispatched natively. The converted upload keeps its full resolution (no
// resize to the composition), the output matches the CPU oracle, and an identical rebuild reuses
// the prepared upload with zero re-decode and zero native re-upload.
void testLargeSourceNative(Expectations& expectations, GpuDevice& device,
                           CpuCompositionEvaluator& evaluator) {
    const auto directory = std::filesystem::temp_directory_path() / "bloom_gpu_large_executor_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto path = directory / "large_4608x3164.exr";
    writeLargeExrRgba(path, 4608, 3164);
    const auto asset = imageAsset(path, "large_source", 4200);
    evaluator.setAssetBaseDirectory(directory);

    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(
        device, *cache.cache, GpuSceneExecutorBudgets{.maxImageBytes = kSceneBudget});
    expectations.expect(cache.hasValue() && executor.hasValue(), "large: cache+executor created");
    if (!cache || !executor) {
        return;
    }
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    context.assetBaseDirectory = directory;
    context.preparedUploadCache =
        std::make_shared<bloom::runtime::GpuPreparedUploadCache>(kCacheBudget);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan = largeSourcePlan(format(1920, 1080), asset, 5000);
    auto request = requestFor(*plan);
    // The CPU oracle's peak (source + layer output + merge) is larger than the builder's retained
    // set, so this native proof uses the executor's real 4 GiB scene budget. The builder's default
    // 512 MiB allowance is covered by the CPU media scene-preparation test.
    request.pixelStorageByteLimit = kSceneBudget;

    const auto first = builder.build(plan, request);
    expectations.expect(first.hasValue(), "large: the full-size EXR graph prepares");
    if (!first) {
        return;
    }
    const auto* upload = firstUpload(*first.scene);
    expectations.expect(upload != nullptr &&
                            upload->descriptor.dataWindow().extent().width() == 4608 &&
                            upload->descriptor.dataWindow().extent().height() == 3164,
                        "large: the converted upload keeps full 4608x3164 resolution");
    expectations.expect(first.scene->outputDescriptor().dataWindow().extent().width() == 1920 &&
                            first.scene->outputDescriptor().dataWindow().extent().height() == 1080,
                        "large: the composition output stays FHD");

    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, "large: the CPU oracle evaluates the graph");

    const auto run = runScene(*executor.executor, first.scene, kSceneBudget);
    expectations.expect(run.ready, "large: the full-size graph dispatches natively");
    if (!run.ready) {
        std::cerr << "LARGE_TERMINAL code="
                  << static_cast<int>(executor.executor->diagnostic().code)
                  << " message=" << executor.executor->diagnostic().message
                  << " producer=" << kSceneBudget
                  << " preparedUploadCache=" << context.preparedUploadCache->maxBytes() << '\n';
        return;
    }
    if (run.ready && run.image != nullptr && frame.frame() != nullptr) {
        const auto readback = readbackResidentImage(*run.image, kReadbackBudget);
        expectations.expect(readback.hasValue(), "large: the test readback succeeds");
        if (readback) {
            expectations.expect(
                pixelsClose(readback.pixels, frame.frame()->processImage().pixels()),
                "large: every output pixel matches the CPU oracle within the process gate");
        }
    }
    expectations.expect(executor.executor->counters().uploads >= 1,
                        "large: a real native upload ran for the 233 MB source");

    // Warm identical request: the prepared-upload cache retains the full source, so the rebuild
    // re-decodes zero frames, and the unchanged output performs zero native dispatches.
    const auto second = builder.build(plan, request);
    expectations.expect(second.hasValue() &&
                            second.scene->mediaStatistics().imageConversions == 0 &&
                            second.scene->mediaStatistics().uploadCacheHits == 1,
                        "large: an identical rebuild re-decodes zero frames (upload cache hit)");
    if (!second) {
        return;
    }
    const auto dispatchesBefore = executor.executor->counters().dispatches;
    const auto secondRun = runScene(*executor.executor, second.scene, kSceneBudget);
    expectations.expect(secondRun.ready, "large: the warm rebuild completes");
    expectations.expect(executor.executor->counters().dispatches == dispatchesBefore,
                        "large: the warm rebuild performs zero native dispatches");
}

int main(const int argc, char** argv) {
    try {
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
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return expectations.ok() ? 0 : 1;
        }

        // Run the explicit batching proof before a long-lived parity facade occupies the one
        // bounded per-device batch slot; other simultaneous executors retain their serial fallback.
        testChangedTransformReusesUpload(expectations, *device.device, evaluator, fixture);
        auto parityCache =
            GpuSceneCache::create(*device.device, GpuSceneCacheBudgets{kCacheBudget});
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
        testChangedSourceInvalidates(expectations, *device.device, evaluator, fixture);
        testWarmOutputZeroDispatch(expectations, *device.device, evaluator, fixture);
        testCachedInputBudget(expectations, *device.device, evaluator, fixture);
        testRealVideo(expectations, *parityExecutor.executor, evaluator, options.fixtures);
        testLargeSourceNative(expectations, *device.device, evaluator);

        if (!expectations.ok()) {
            std::cerr << "FAIL: GPU media executor expectations failed\n";
            return 1;
        }
        std::cout << "PASS: GPU media executor\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
