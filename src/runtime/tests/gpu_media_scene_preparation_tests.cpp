#include "gpu_media_scene_preparation_test_support.hpp"

#include <bloom/media/cache/media_disk_cache.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

using bloom::runtime::GpuPreparedUploadCache;
using bloom::runtime::GpuSceneMediaContext;
using bloom::runtime::GpuSceneMediaStatistics;
using bloom::runtime::GpuSceneUploadCommand;
using bloom::runtime::PreparedGpuScene;
using bloom::runtime::ProxyResolution;

[[nodiscard]] const GpuSceneUploadCommand* firstUpload(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* upload = std::get_if<GpuSceneUploadCommand>(&command)) {
            return upload;
        }
    }
    return nullptr;
}

[[nodiscard]] const bloom::runtime::GpuSceneTranslationCommand*
firstTranslation(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* translation =
                std::get_if<bloom::runtime::GpuSceneTranslationCommand>(&command)) {
            return translation;
        }
    }
    return nullptr;
}

struct MediaFixture final {
    std::filesystem::path directory;
    std::filesystem::path path;
    document::AssetRecord asset;
};

[[nodiscard]] MediaFixture makeFixture() {
    MediaFixture fixture;
    fixture.directory = std::filesystem::temp_directory_path() / "bloom_gpu_media_scene_prep_test";
    std::filesystem::remove_all(fixture.directory);
    std::filesystem::create_directories(fixture.directory);
    fixture.path = fixture.directory / "signed_hdr.exr";
    writeExrRgba(fixture.path, 3, 2, signedHdrPixels());
    fixture.asset = imageAsset(fixture.path, "signed_hdr", 900);
    return fixture;
}

// Full parity against a genuine, uncached CpuCompositionEvaluator frame: identity, bounds, output
// descriptor and every pixel, plus upload bounds.
void checkMediaParity(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                      const std::shared_ptr<const CompiledCompositionPlan>& plan,
                      const EvaluationRequest& request, const std::string& label) {
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), label + ": prepares");
    if (!prepared) {
        return;
    }
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, label + ": CPU frame evaluates");
    if (!frame.frame()) {
        return;
    }
    expectations.expect(prepared.scene->processIdentity() == frame.frame()->identity(),
                        label + ": identity matches");
    expectations.expect(
        std::ranges::equal(prepared.scene->bounds(), frame.frame()->evaluatedBounds()),
        label + ": bounds match");
    expectations.expect(prepared.scene->outputDescriptor() ==
                            *frame.frame()->processImage().descriptor(),
                        label + ": output descriptor matches");
    expectations.expect(prepared.scene->mediaStatistics().imageSources == 1 &&
                            prepared.scene->mediaStatistics().imageConversions == 1,
                        label + ": exactly one image source was converted");
    std::vector<std::shared_ptr<const Rgba32fImage>> images;
    expectations.expect(replayScene(*prepared.scene, images), label + ": replays");
    const auto& replayed = images[prepared.scene->outputCommand()];
    expectations.expect(
        replayed != nullptr &&
            replayed->pixels().size() == frame.frame()->processImage().pixels().size() &&
            std::memcmp(replayed->pixels().data(), frame.frame()->processImage().pixels().data(),
                        replayed->pixels().size() * sizeof(Rgba32f)) == 0,
        replayed == nullptr
            ? label + ": replay produced no image"
            : label + ": pixel parity " + firstMismatch(*replayed, frame.frame()->processImage()));
}

void testStillImageParity(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                          const MediaFixture& fixture) {
    // Odd 3x2 signed/HDR source smaller than an 8x8 composition, fractional +0.3 translation.
    const auto fractional = mediaPlan(format(8, 8), fixture.asset,
                                      LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, 1000);
    checkMediaParity(expectations, evaluator, fractional, requestFor(*fractional),
                     "signed/HDR still image fractional");
    // Fractional -0.3 and a non-unit opacity.
    const auto fractionalNegative = mediaPlan(
        format(8, 8), fixture.asset, LayerValues{.position = {2.7, 2.4}, .opacity = 0.625}, 1100);
    checkMediaParity(expectations, evaluator, fractionalNegative, requestFor(*fractionalNegative),
                     "fractional -0.3 / opacity");
    // An integer device grid still uses the raster translation command for media (no vector chain).
    const auto integerGrid = mediaPlan(format(8, 8), fixture.asset,
                                       LayerValues{.position = {4.0, 3.0}, .opacity = 1.0}, 1200);
    const auto prepared = CpuGpuSceneBuilder{}.build(integerGrid, requestFor(*integerGrid));
    // The builder above has no media context, so it must fail closed rather than decode.
    expectations.expect(!prepared &&
                            prepared.diagnostic.code ==
                                bloom::runtime::PreparedGpuSceneDiagnosticCode::MediaUnavailable,
                        "a media scene without an evaluator context fails closed");
    checkMediaParity(expectations, evaluator, integerGrid, requestFor(*integerGrid),
                     "integer grid media still raster");
}

void testProxyNonSquarePar(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                           const MediaFixture& fixture) {
    const auto plan = mediaPlan(format(9, 6, pixelAspect(4, 3)), fixture.asset,
                                LayerValues{.position = {4.5, 3.0}}, 1300);
    const auto extent = bloom::render::ImageExtent::create(5, 4);
    expectations.expect(static_cast<bool>(extent), "the proxy extent builds");
    if (!extent) {
        return;
    }
    auto request = requestFor(*plan);
    request.resolution = ProxyResolution{*extent.value()};
    checkMediaParity(expectations, evaluator, plan, request, "media proxy / non-square PAR");
}

void testSequenceFrames(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                        const MediaFixture& fixture) {
    auto secondPixels = signedHdrPixels();
    for (auto& pixel : secondPixels) {
        pixel.red += 0.5F;
        pixel.blue *= 0.5F;
    }
    const auto secondPath = fixture.directory / "second.exr";
    writeExrRgba(secondPath, 3, 2, secondPixels);

    document::AssetRecord asset = fixture.asset;
    asset.id = bloom::document::AssetId::fromRaw(901);
    asset.kind = document::AssetKind::Sequence;
    asset.manifest.pattern = "sequence.####.exr";
    asset.manifest.padding = 4;
    asset.manifest.first = 0;
    asset.manifest.last = 1;
    asset.manifest.members = {sequenceMember(fixture.path, 0, 0), sequenceMember(secondPath, 1, 1)};

    const auto plan =
        mediaPlan(format(8, 8), asset, LayerValues{.position = {4.3, 3.1}, .opacity = 0.75}, 1400);
    checkMediaParity(expectations, evaluator, plan, requestFor(*plan, RationalTime::fromInteger(0)),
                     "sequence frame 0");
    checkMediaParity(expectations, evaluator, plan, requestFor(*plan, rationalTime(1, 24)),
                     "sequence frame 1");
}

void testWarmReuseAndChangedSource(Expectations& expectations,
                                   const CpuCompositionEvaluator& evaluator,
                                   const MediaFixture& fixture) {
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto planA = mediaPlan(format(8, 8), fixture.asset,
                                 LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, 1500);
    const auto planB = mediaPlan(format(8, 8), fixture.asset,
                                 LayerValues{.position = {2.7, 2.4}, .opacity = 1.0}, 1600);
    const auto a = builder.build(planA, requestFor(*planA));
    const auto b = builder.build(planB, requestFor(*planB));
    expectations.expect(a.hasValue() && b.hasValue(), "both media builds prepare");
    if (!a || !b) {
        return;
    }
    const auto* uploadA = firstUpload(*a.scene);
    const auto* uploadB = firstUpload(*b.scene);
    const auto* translationA = firstTranslation(*a.scene);
    const auto* translationB = firstTranslation(*b.scene);
    expectations.expect(uploadA != nullptr && uploadB != nullptr && translationA != nullptr &&
                            translationB != nullptr,
                        "both builds emit an upload and a translation command");
    if (uploadA == nullptr || uploadB == nullptr || translationA == nullptr ||
        translationB == nullptr) {
        return;
    }
    expectations.expect(uploadA->semanticKey == uploadB->semanticKey,
                        "an unchanged source keeps one upload semantic key across transforms");
    expectations.expect(uploadA->image.get() == uploadB->image.get(),
                        "an unchanged source reuses the SAME immutable allocation");
    expectations.expect(translationA->semanticKey != translationB->semanticKey,
                        "a changed transform has a different translation pixel key");
    expectations.expect(b.scene->mediaStatistics().uploadCacheHits == 1 &&
                            b.scene->mediaStatistics().imageConversions == 0,
                        "the warm build serves the source from the prepared-upload cache");
}

void testChangedFrameAndBypass(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                               const MediaFixture& fixture) {
    auto secondPixels = signedHdrPixels();
    secondPixels.front().green += 1.0F;
    const auto secondPath = fixture.directory / "bypass_second.exr";
    writeExrRgba(secondPath, 3, 2, secondPixels);
    document::AssetRecord asset = fixture.asset;
    asset.id = bloom::document::AssetId::fromRaw(902);
    asset.kind = document::AssetKind::Sequence;
    asset.manifest.pattern = "bypass_sequence.####.exr";
    asset.manifest.padding = 4;
    asset.manifest.first = 0;
    asset.manifest.last = 1;
    asset.manifest.members = {sequenceMember(fixture.path, 0, 0), sequenceMember(secondPath, 1, 1)};
    const auto plan = mediaPlan(format(8, 8), asset, LayerValues{.position = {4.3, 3.1}}, 1700);

    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto frameZero = builder.build(plan, requestFor(*plan, RationalTime::fromInteger(0)));
    const auto frameOne = builder.build(plan, requestFor(*plan, rationalTime(1, 24)));
    expectations.expect(frameZero.hasValue() && frameOne.hasValue(),
                        "both sequence frames prepare");
    if (!frameZero || !frameOne) {
        return;
    }
    const auto* uploadZero = firstUpload(*frameZero.scene);
    const auto* uploadOne = firstUpload(*frameOne.scene);
    expectations.expect(uploadZero != nullptr && uploadOne != nullptr &&
                            uploadZero->semanticKey != uploadOne->semanticKey,
                        "a changed selected frame changes the upload semantic key");
    expectations.expect(uploadZero->image.get() != uploadOne->image.get(),
                        "a changed selected frame converts a distinct source image");

    // Explicit bypass recalculates the source instead of reading the prepared-upload cache.
    auto bypassRequest = requestFor(*plan, RationalTime::fromInteger(0));
    bypassRequest.bypassOperationCache = true;
    const auto bypassed = builder.build(plan, bypassRequest);
    expectations.expect(bypassed.hasValue(), "the bypass build prepares");
    if (!bypassed) {
        return;
    }
    const auto* uploadBypass = firstUpload(*bypassed.scene);
    expectations.expect(bypassed.scene->mediaStatistics().imageConversions == 1 &&
                            bypassed.scene->mediaStatistics().uploadCacheHits == 0,
                        "an explicit bypass reconverts and never reads the prepared-upload cache");
    expectations.expect(uploadBypass != nullptr && uploadZero != nullptr &&
                            uploadBypass->image.get() != uploadZero->image.get(),
                        "an explicit bypass recalculates a distinct source image");
}

void testUnsupportedMediaGraph(Expectations& expectations, const MediaFixture& fixture) {
    // A non-Normal media layer is reachable; the screen pass must refuse the whole graph BEFORE any
    // media selection or decode. The counters are per-build and reported on the failure diagnostic,
    // so they must stay at zero.
    auto context = GpuSceneMediaContext::fromEvaluator(CpuCompositionEvaluator{});
    context.assetBaseDirectory = fixture.directory;
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan = mediaPlan(
        format(8, 8), fixture.asset,
        LayerValues{.position = {4.3, 3.1}, .blendMode = bloom::core::BlendMode::Screen}, 1800);
    const auto prepared = builder.build(plan, requestFor(*plan));
    expectations.expect(!prepared &&
                            prepared.diagnostic.code ==
                                bloom::runtime::PreparedGpuSceneDiagnosticCode::UnsupportedBlend,
                        "a non-Normal media graph is refused");
    const auto& refused = prepared.diagnostic.mediaStatistics;
    expectations.expect(refused.imageSources == 0 && refused.imageConversions == 0 &&
                            refused.uploadCacheMisses == 0,
                        "no media was selected or decoded for the unsupported graph");

    // A rotated media layer is likewise refused before any media work.
    const auto rotated = mediaPlan(format(8, 8), fixture.asset,
                                   LayerValues{.position = {4.3, 3.1}, .rotation = 30.0}, 1900);
    const auto rotatedPrepared = builder.build(rotated, requestFor(*rotated));
    expectations.expect(
        !rotatedPrepared &&
            rotatedPrepared.diagnostic.code ==
                bloom::runtime::PreparedGpuSceneDiagnosticCode::UnsupportedTransform,
        "a rotated media layer is refused");
    expectations.expect(rotatedPrepared.diagnostic.mediaStatistics.imageSources == 0,
                        "a rotated media layer selects no media");
}

void testBudgetRefusal(Expectations& expectations, const MediaFixture& fixture) {
    auto context = GpuSceneMediaContext::fromEvaluator(CpuCompositionEvaluator{});
    context.assetBaseDirectory = fixture.directory;
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan =
        mediaPlan(format(256, 256), fixture.asset, LayerValues{.position = {128.0, 128.0}}, 2000);
    auto request = requestFor(*plan);
    request.pixelStorageByteLimit = 1U << 16U;
    const auto prepared = builder.build(plan, request);
    expectations.expect(!prepared.hasValue(), "a media scene under a tiny budget fails closed");
    expectations.expect(
        !prepared.hasValue() &&
            prepared.diagnostic.code ==
                bloom::runtime::PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
        "the media budget refusal is diagnosed as a pixel-storage budget");
}

// A changed input colour interpretation is a cache miss and must still match the CPU exactly.
void testChangedColourInterpretation(Expectations& expectations,
                                     const CpuCompositionEvaluator& evaluator,
                                     const MediaFixture& fixture) {
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto base = mediaPlan(format(8, 8), fixture.asset,
                                LayerValues{.position = {4.3, 3.1}, .opacity = 0.8}, 2100);
    const auto first = builder.build(base, requestFor(*base));
    expectations.expect(first.hasValue() && first.scene->mediaStatistics().imageConversions == 1 &&
                            first.scene->mediaStatistics().uploadCacheMisses == 1,
                        "the base build converts once and misses the upload cache");

    auto definition = base->copyDefinition();
    auto* source = std::get_if<bloom::runtime::CompiledImageSource>(&definition.operations[0]);
    if (source == nullptr || !source->asset) {
        expectations.expect(false, "the base plan has a media source");
        return;
    }
    source->asset->interpretation.colorSpace = bloom::document::AssetColorSpace::Raw;
    const auto reinterpreted = publish(std::move(definition));

    checkMediaParity(expectations, evaluator, reinterpreted, requestFor(*reinterpreted),
                     "changed input colour interpretation");
    const auto second = builder.build(reinterpreted, requestFor(*reinterpreted));
    expectations.expect(second.hasValue() &&
                            second.scene->mediaStatistics().imageConversions == 1 &&
                            second.scene->mediaStatistics().uploadCacheHits == 0 &&
                            second.scene->mediaStatistics().uploadCacheMisses == 1,
                        "a changed input colour interpretation is an upload cache miss");
    const auto* uploadFirst = firstUpload(*first.scene);
    const auto* uploadSecond = firstUpload(*second.scene);
    expectations.expect(uploadFirst != nullptr && uploadSecond != nullptr &&
                            uploadFirst->semanticKey != uploadSecond->semanticKey,
                        "a changed input colour interpretation changes the source key");
}

// The source key is an identity of the resolved pixels, not of the plan: node/layer/parameter ids
// and the document revision must not enter it, while the proxy/composition descriptor must.
void testSourceKeyExcludesIdsAndRevision(Expectations& expectations,
                                         const CpuCompositionEvaluator& evaluator,
                                         const MediaFixture& fixture) {
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto a = mediaPlan(format(8, 8), fixture.asset,
                             LayerValues{.position = {4.3, 3.1}, .opacity = 0.8}, 2700);
    const auto b = mediaPlan(format(8, 8), fixture.asset,
                             LayerValues{.position = {4.3, 3.1}, .opacity = 0.8}, 2800);
    auto definition = a->copyDefinition();
    definition.sourceRevision = bloom::document::Revision::fromRaw(999);
    const auto revised = publish(std::move(definition));

    const auto builtA = builder.build(a, requestFor(*a));
    const auto builtB = builder.build(b, requestFor(*b));
    const auto builtRevised = builder.build(revised, requestFor(*revised));
    expectations.expect(builtA.hasValue() && builtB.hasValue() && builtRevised.hasValue(),
                        "the identity-key builds prepare");
    if (!builtA || !builtB || !builtRevised) {
        return;
    }
    const auto* uploadA = firstUpload(*builtA.scene);
    const auto* uploadB = firstUpload(*builtB.scene);
    const auto* uploadRevised = firstUpload(*builtRevised.scene);
    const auto* translationA = firstTranslation(*builtA.scene);
    const auto* translationB = firstTranslation(*builtB.scene);
    const auto* translationRevised = firstTranslation(*builtRevised.scene);
    expectations.expect(uploadA != nullptr && uploadB != nullptr && uploadRevised != nullptr &&
                            translationA != nullptr && translationB != nullptr &&
                            translationRevised != nullptr,
                        "the identity-key builds emit uploads and translations");
    if (uploadA == nullptr || uploadB == nullptr || uploadRevised == nullptr ||
        translationA == nullptr || translationB == nullptr || translationRevised == nullptr) {
        return;
    }
    expectations.expect(uploadA->semanticKey == uploadB->semanticKey &&
                            uploadA->semanticKey == uploadRevised->semanticKey,
                        "node/layer/parameter ids and the revision never enter the source key");
    expectations.expect(translationA->semanticKey == translationB->semanticKey &&
                            translationA->semanticKey == translationRevised->semanticKey,
                        "node/layer/parameter ids and the revision never enter a derived key");

    // The proxy scale IS part of the converted source identity: a different proxy is a miss.
    const auto proxyPlan = mediaPlan(format(8, 8), fixture.asset,
                                     LayerValues{.position = {4.3, 3.1}, .opacity = 0.8}, 2900);
    const auto extent = bloom::render::ImageExtent::create(5, 4);
    expectations.expect(static_cast<bool>(extent), "the proxy extent builds");
    if (!extent) {
        return;
    }
    auto proxyRequest = requestFor(*proxyPlan);
    proxyRequest.resolution = ProxyResolution{*extent.value()};
    const auto proxied = builder.build(proxyPlan, proxyRequest);
    expectations.expect(proxied.hasValue(), "the proxy build prepares");
    if (!proxied) {
        return;
    }
    const auto* uploadProxy = firstUpload(*proxied.scene);
    expectations.expect(uploadProxy != nullptr &&
                            uploadProxy->semanticKey != uploadA->semanticKey &&
                            proxied.scene->mediaStatistics().uploadCacheMisses == 1,
                        "a changed proxy scale changes the source key and is a miss");
}

// Per-build CPU work is reported on the result, never in a shared mutable aggregate.
void testPerRequestStatisticsAreLocal(Expectations& expectations,
                                      const CpuCompositionEvaluator& evaluator,
                                      const MediaFixture& fixture) {
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan = mediaPlan(format(8, 8), fixture.asset,
                                LayerValues{.position = {4.3, 3.1}, .opacity = 0.9}, 3000);
    const auto first = builder.build(plan, requestFor(*plan));
    const auto second = builder.build(plan, requestFor(*plan));
    expectations.expect(first.hasValue() && second.hasValue(), "both per-request builds prepare");
    if (!first || !second) {
        return;
    }
    expectations.expect(first.scene->mediaStatistics().uploadCacheMisses == 1 &&
                            first.scene->mediaStatistics().uploadCacheHits == 0 &&
                            first.scene->mediaStatistics().imageConversions == 1,
                        "the first result reports only its own cold work");
    expectations.expect(second.scene->mediaStatistics().uploadCacheHits == 1 &&
                            second.scene->mediaStatistics().uploadCacheMisses == 0 &&
                            second.scene->mediaStatistics().imageConversions == 0,
                        "the second result reports only its own warm work");
}

// Explicit bypass and the interactive (overridden) plan bypass must not touch the real disk cache.
void testGestureCacheNeverTouchesDisk(Expectations& expectations,
                                      const CpuCompositionEvaluator& evaluator,
                                      const MediaFixture& fixture) {
    const auto diskRoot = fixture.directory / "gesture_disk_cache";
    auto disk = std::make_shared<bloom::media::cache::MediaDiskCache>(
        bloom::media::cache::MediaDiskCacheConfig{.rootDirectory = diskRoot,
                                                  .byteBudget = std::size_t{1} << 24U});
    evaluator.setMediaDiskCache(disk);
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan = mediaPlan(format(8, 8), fixture.asset,
                                LayerValues{.position = {4.3, 3.1}, .opacity = 0.9}, 3100);
    const auto warm = builder.build(plan, requestFor(*plan));
    expectations.expect(warm.hasValue(), "the disk-cache warm build prepares");
    disk->flush();
    const auto before = disk->statistics();

    auto interactiveDefinition = plan->copyDefinition();
    interactiveDefinition.bypassOperationCache = true;
    const auto interactive = publish(std::move(interactiveDefinition));
    const auto gestureHit = builder.build(interactive, requestFor(*interactive));
    disk->flush();
    const auto afterHit = disk->statistics();
    expectations.expect(gestureHit.hasValue() &&
                            gestureHit.scene->mediaStatistics().uploadCacheHits == 1 &&
                            gestureHit.scene->mediaStatistics().imageConversions == 0,
                        "a gesture hit serves the prepared-upload cache");
    expectations.expect(afterHit.hits == before.hits && afterHit.misses == before.misses &&
                            afterHit.entryCount == before.entryCount &&
                            afterHit.storedBytes == before.storedBytes,
                        "a gesture hit never reads or writes the disk cache");

    auto secondPixels = signedHdrPixels();
    secondPixels.front().red += 0.75F;
    const auto secondPath = fixture.directory / "gesture_second.exr";
    writeExrRgba(secondPath, 3, 2, secondPixels);
    const auto secondAsset = imageAsset(secondPath, "gesture_second", 904);
    const auto missPlan =
        mediaPlan(format(8, 8), secondAsset, LayerValues{.position = {4.3, 3.1}}, 3200);
    auto missDefinition = missPlan->copyDefinition();
    missDefinition.bypassOperationCache = true;
    const auto missInteractive = publish(std::move(missDefinition));
    const auto beforeMiss = disk->statistics();
    const auto gestureMiss = builder.build(missInteractive, requestFor(*missInteractive));
    disk->flush();
    const auto afterMiss = disk->statistics();
    expectations.expect(gestureMiss.hasValue() &&
                            gestureMiss.scene->mediaStatistics().uploadCacheMisses == 1 &&
                            gestureMiss.scene->mediaStatistics().uploadCacheHits == 0 &&
                            gestureMiss.scene->mediaStatistics().imageConversions == 1,
                        "a gesture miss converts directly and is not inserted");
    expectations.expect(afterMiss.hits == beforeMiss.hits &&
                            afterMiss.misses == beforeMiss.misses &&
                            afterMiss.entryCount == beforeMiss.entryCount &&
                            afterMiss.storedBytes == beforeMiss.storedBytes,
                        "a gesture miss never reads or writes the disk cache");

    auto explicitRequest = requestFor(*plan);
    explicitRequest.bypassOperationCache = true;
    const auto beforeExplicit = disk->statistics();
    const auto explicitBuild = builder.build(plan, explicitRequest);
    disk->flush();
    const auto afterExplicit = disk->statistics();
    expectations.expect(explicitBuild.hasValue() &&
                            explicitBuild.scene->mediaStatistics().uploadCacheHits == 0 &&
                            explicitBuild.scene->mediaStatistics().imageConversions == 1,
                        "an explicit bypass reconverts and never reads the prepared-upload cache");
    expectations.expect(afterExplicit.hits == beforeExplicit.hits &&
                            afterExplicit.misses == beforeExplicit.misses &&
                            afterExplicit.entryCount == beforeExplicit.entryCount &&
                            afterExplicit.storedBytes == beforeExplicit.storedBytes,
                        "an explicit bypass never reads or writes the disk cache either");
}

// Obtain a live, already-cancelled CancellationToken. The only public source of one is a real
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
        TaskRequest("gpu media cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(77)}),
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

void testPreCancelledPreparationPublishesNothing(
    Expectations& expectations, const std::shared_ptr<const CompiledCompositionPlan>& plan) {
    const auto token = makeCancelledToken();
    expectations.expect(token.isCancellationRequested(), "the captured token is cancelled");
    const CpuGpuSceneBuilder builder{};
    const auto prepared = builder.build(plan, requestFor(*plan), token);
    expectations.expect(!prepared.hasValue() &&
                            prepared.diagnostic.code ==
                                bloom::runtime::PreparedGpuSceneDiagnosticCode::Cancelled,
                        "a pre-cancelled request returns no partial scene");
}

// Cancellation requested while preparation is already running still publishes no partial scene.
void testCancellationDuringPreparation(Expectations& expectations) {
    const auto plan =
        twoLayerPlan(format(16, 12), LayerValues{.position = {8.3, 6.1}},
                     LayerValues{.position = {8.7, 6.4}, .opacity = 0.75}, 2048.0, 2048.0, 3300);
    const auto request = requestFor(*plan, RationalTime::fromInteger(0), std::size_t{1} << 29U);
    bloom::runtime::TaskSchedulerConfig config = bloom::runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    bloom::runtime::TaskScheduler scheduler(config);
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    std::atomic_bool observedCancelled = false;
    std::atomic_bool observedSuccess = false;
    const CpuGpuSceneBuilder builder{};
    auto submission = scheduler.submit<void>(
        bloom::runtime::TaskRequest("gpu media cancellation fixture",
                                    {.kind = bloom::runtime::TaskOwnerKind::Composition,
                                     .id = bloom::runtime::TaskOwnerId::fromRaw(78)}),
        [&](bloom::runtime::TaskContext& context) {
            {
                std::lock_guard lock(mutex);
                started = true;
            }
            condition.notify_all();
            const auto prepared = builder.build(plan, request, context.cancellation());
            if (!prepared) {
                observedCancelled.store(prepared.diagnostic.code ==
                                        bloom::runtime::PreparedGpuSceneDiagnosticCode::Cancelled);
            } else {
                observedSuccess.store(true);
            }
            return bloom::runtime::TaskResult<void>::succeeded();
        });
    expectations.expect(submission.accepted(), "the cancellation fixture task is accepted");
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return started; });
    }
    submission.handle.cancel();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!submission.handle.tryTakeResult().has_value() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    expectations.expect(observedCancelled.load() && !observedSuccess.load(),
                        "cancellation during preparation returns no partial scene");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
}

} // namespace

int main() {
    try {
        Expectations expectations;
        const CpuCompositionEvaluator evaluator;
        const auto fixture = makeFixture();
        evaluator.setAssetBaseDirectory(fixture.directory);

        testStillImageParity(expectations, evaluator, fixture);
        testProxyNonSquarePar(expectations, evaluator, fixture);
        testSequenceFrames(expectations, evaluator, fixture);
        testWarmReuseAndChangedSource(expectations, evaluator, fixture);
        testChangedFrameAndBypass(expectations, evaluator, fixture);
        testChangedColourInterpretation(expectations, evaluator, fixture);
        testSourceKeyExcludesIdsAndRevision(expectations, evaluator, fixture);
        testPerRequestStatisticsAreLocal(expectations, evaluator, fixture);
        testGestureCacheNeverTouchesDisk(expectations, evaluator, fixture);
        testUnsupportedMediaGraph(expectations, fixture);
        testBudgetRefusal(expectations, fixture);

        {
            const auto cancelPlan =
                mediaPlan(format(8, 8), fixture.asset, LayerValues{.position = {4.3, 3.1}}, 3400);
            testPreCancelledPreparationPublishesNothing(expectations, cancelPlan);
        }
        testCancellationDuringPreparation(expectations);
        if (!expectations.ok()) {
            std::cerr << "FAIL: GPU media scene preparation expectations failed\n";
            return 1;
        }
        if (std::getenv("BLOOM_GPU_MEDIA_PROOF") != nullptr) {
            // Recorded CPU work for the proof log: source-specific key construction and
            // decode/conversion counters. There is deliberately no GPU upload count -- the native
            // executor does not exist.
            const auto cold = GpuSceneMediaContext::fromEvaluator(evaluator);
            const CpuGpuSceneBuilder builder(nullptr, cold);
            const auto plan =
                mediaPlan(format(8, 8), fixture.asset, LayerValues{.position = {4.3, 3.1}}, 9900);
            const auto first = builder.build(plan, requestFor(*plan));
            const auto second = builder.build(plan, requestFor(*plan));
            if (first && second) {
                std::cerr << "PROOF counters: first imageSources="
                          << first.scene->mediaStatistics().imageSources
                          << " conversions=" << first.scene->mediaStatistics().imageConversions
                          << " keyConstructions="
                          << first.scene->mediaStatistics().uploadKeyConstructions
                          << " hits=" << first.scene->mediaStatistics().uploadCacheHits
                          << " misses=" << first.scene->mediaStatistics().uploadCacheMisses
                          << "; warm imageSources=" << second.scene->mediaStatistics().imageSources
                          << " conversions=" << second.scene->mediaStatistics().imageConversions
                          << " keyConstructions="
                          << second.scene->mediaStatistics().uploadKeyConstructions
                          << " hits=" << second.scene->mediaStatistics().uploadCacheHits
                          << " misses=" << second.scene->mediaStatistics().uploadCacheMisses
                          << "\n";
            }
        }
        std::cout << "PASS: CPU GPU media scene preparation\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
