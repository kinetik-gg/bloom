#include "gpu_media_video_scene_preparation_test_support.hpp"

#include "video_source.hpp"

#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using bloom::runtime::GpuSceneAffineCommand;
using bloom::runtime::GpuSceneBlendCommand;
using bloom::runtime::GpuSceneMediaContext;
using bloom::runtime::GpuSceneTranslationCommand;
using bloom::runtime::GpuSceneUploadCommand;
using bloom::runtime::PreparedGpuScene;

[[nodiscard]] const GpuSceneUploadCommand* firstUpload(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* upload = std::get_if<GpuSceneUploadCommand>(&command)) {
            return upload;
        }
    }
    return nullptr;
}

[[nodiscard]] const GpuSceneTranslationCommand* firstTranslation(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* translation = std::get_if<GpuSceneTranslationCommand>(&command)) {
            return translation;
        }
    }
    return nullptr;
}

// Full parity against a genuine, uncached CpuCompositionEvaluator frame for a real decoded video
// source: identity, bounds, output descriptor and every pixel, plus upload bounds.
void checkVideoParity(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
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
    expectations.expect(prepared.scene->mediaStatistics().videoSources == 1 &&
                            prepared.scene->mediaStatistics().videoConversions == 1,
                        label + ": exactly one video source was converted");
    double hScale = 1.0;
    double vScale = 1.0;
    if (const auto* proxy = std::get_if<bloom::runtime::ProxyResolution>(&request.resolution)) {
        hScale = static_cast<double>(proxy->extent.width()) /
                 static_cast<double>(plan->format().width());
        vScale = static_cast<double>(proxy->extent.height()) /
                 static_cast<double>(plan->format().height());
    }
    std::vector<std::shared_ptr<const Rgba32fImage>> images;
    expectations.expect(replayScene(*prepared.scene, images, hScale, vScale), label + ": replays");
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

struct FixtureSpec final {
    const char* file;
    std::uint64_t idBase;
};

// The only video fixtures owned by the media-worker generator
// (apps/bloom-media-worker/ffmpeg_provider_fixtures.ipp). No other file in the shared fixture
// directory is a generated fixture; in particular the sibling video-integration scratch copy is
// private to that test and mutates mid-run, so it is never assumed here.
constexpr std::array<FixtureSpec, 3> kFixtures{{
    {"numbered-h264.mp4", 10000},
    {"numbered-prores.mov", 11000},
    {"alpha-prores.mov", 12000},
}};

// Every real fixture decodes and converts two DISTINCT selected frames, each matching the uncached
// CPU frame pixel-for-pixel; the two upload keys differ by the resolved source frame.
void testAllFixturesDistinctFrames(Expectations& expectations,
                                   const CpuCompositionEvaluator& evaluator,
                                   const std::filesystem::path& fixtures) {
    for (const auto& spec : kFixtures) {
        const auto path = fixtures / spec.file;
        expectations.expect(std::filesystem::is_regular_file(path),
                            std::string{"fixture is present: "} + spec.file);
        if (!std::filesystem::is_regular_file(path)) {
            continue;
        }
        const auto asset = videoAsset(path, spec.idBase);
        const auto plan =
            videoPlan(format(96, 64), asset, LayerValues{.position = {48.3, 32.1}, .opacity = 0.9},
                      spec.idBase);
        checkVideoParity(expectations, evaluator, plan,
                         requestFor(*plan, RationalTime::fromInteger(0)),
                         std::string{spec.file} + " frame 0");
        checkVideoParity(expectations, evaluator, plan, requestFor(*plan, rationalTime(1, 2)),
                         std::string{spec.file} + " frame 12");

        auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
        const CpuGpuSceneBuilder builder(nullptr, context);
        const auto zero = builder.build(plan, requestFor(*plan, RationalTime::fromInteger(0)));
        const auto twelve = builder.build(plan, requestFor(*plan, rationalTime(1, 2)));
        expectations.expect(zero.hasValue() && twelve.hasValue(),
                            std::string{spec.file} + " distinct-frame builds prepare");
        if (!zero || !twelve) {
            continue;
        }
        const auto* uploadZero = firstUpload(*zero.scene);
        const auto* uploadTwelve = firstUpload(*twelve.scene);
        expectations.expect(uploadZero != nullptr && uploadTwelve != nullptr &&
                                uploadZero->semanticKey != uploadTwelve->semanticKey,
                            std::string{spec.file} +
                                " upload keys differ by the resolved source frame");
        if (uploadZero != nullptr && uploadTwelve != nullptr) {
            expectations.expect(uploadZero->image.get() != uploadTwelve->image.get(),
                                std::string{spec.file} +
                                    " distinct frames convert distinct images");
        }
    }
}

// Fractional translation, a non-square PAR and a proxy on a real ProRes source.
void testProxyNonSquareParAndFractional(Expectations& expectations,
                                        const CpuCompositionEvaluator& evaluator,
                                        const std::filesystem::path& fixtures) {
    const auto path = fixtures / "numbered-prores.mov";
    if (!std::filesystem::is_regular_file(path)) {
        expectations.expect(false, "the ProRes fixture is present");
        return;
    }
    const auto asset = videoAsset(path, 20000);
    const auto plan = videoPlan(format(48, 32, pixelAspect(4, 3)), asset,
                                LayerValues{.position = {24.3, 15.7}, .opacity = 0.7}, 20000);
    const auto extent = bloom::render::ImageExtent::create(24, 16);
    expectations.expect(static_cast<bool>(extent), "the video proxy extent builds");
    if (!extent) {
        return;
    }
    auto request = requestFor(*plan);
    request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
    checkVideoParity(expectations, evaluator, plan, request, "video proxy / non-square PAR");
}

// A transform-only change reuses the same converted image allocation; the evaluator's decoded frame
// cache is retained as its resident-byte counter allows. selectVideoSource may decode before the
// converted-upload lookup, so this never claims a zero-decode from the conversion counters alone.
void testVideoWarmReuse(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                        const std::filesystem::path& fixtures) {
    const auto path = fixtures / "numbered-h264.mp4";
    if (!std::filesystem::is_regular_file(path)) {
        expectations.expect(false, "the H.264 fixture is present");
        return;
    }
    const auto asset = videoAsset(path, 21000);
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto planA = videoPlan(format(96, 64), asset,
                                 LayerValues{.position = {48.3, 32.1}, .opacity = 0.9}, 21000);
    const auto planB = videoPlan(format(96, 64), asset,
                                 LayerValues{.position = {40.7, 28.4}, .opacity = 0.9}, 22000);
    const auto residentBefore = evaluator.videoContext()->cache.residentBytes();
    const auto a = builder.build(planA, requestFor(*planA));
    const auto residentAfterFirst = evaluator.videoContext()->cache.residentBytes();
    const auto b = builder.build(planB, requestFor(*planB));
    const auto residentAfterSecond = evaluator.videoContext()->cache.residentBytes();
    expectations.expect(a.hasValue() && b.hasValue(), "both warm-reuse builds prepare");
    if (!a || !b) {
        return;
    }
    expectations.expect(a.scene->mediaStatistics().videoConversions == 1 &&
                            a.scene->mediaStatistics().uploadCacheMisses == 1,
                        "the cold build decodes and converts once");
    expectations.expect(b.scene->mediaStatistics().videoConversions == 0 &&
                            b.scene->mediaStatistics().uploadCacheHits == 1,
                        "a transform-only update reuses the converted image");
    const auto* uploadA = firstUpload(*a.scene);
    const auto* uploadB = firstUpload(*b.scene);
    const auto* translationA = firstTranslation(*a.scene);
    const auto* translationB = firstTranslation(*b.scene);
    expectations.expect(uploadA != nullptr && uploadB != nullptr && translationA != nullptr &&
                            translationB != nullptr,
                        "both builds emit an upload and a translation");
    if (uploadA == nullptr || uploadB == nullptr || translationA == nullptr ||
        translationB == nullptr) {
        return;
    }
    expectations.expect(uploadA->semanticKey == uploadB->semanticKey &&
                            uploadA->image.get() == uploadB->image.get(),
                        "an unchanged source reuses the SAME immutable converted allocation");
    expectations.expect(translationA->semanticKey != translationB->semanticKey,
                        "a changed transform has a different translation pixel key");
    // The evaluator's decoded-frame cache is a separate retained resource; whether a given frame
    // fits its ledger is not this preparation's concern. The only claim made here is that the warm
    // build did not ADD decoded bytes. selectVideoSource may still decode before the
    // converted-upload lookup, so a zero-decode claim is deliberately never made from the
    // conversion counters.
    expectations.expect(residentAfterSecond <= residentAfterFirst &&
                            residentAfterSecond >= residentBefore,
                        "the warm build never grows the decoded frame cache");
}

// The explicit and interactive bypass matrix must match the evaluator's own still/video behaviour.
void testVideoBypassMatrix(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                           const std::filesystem::path& fixtures) {
    const auto path = fixtures / "numbered-prores.mov";
    if (!std::filesystem::is_regular_file(path)) {
        expectations.expect(false, "the ProRes fixture is present");
        return;
    }
    const auto asset = videoAsset(path, 23000);
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan = videoPlan(format(96, 64), asset,
                                LayerValues{.position = {48.3, 32.1}, .opacity = 0.9}, 23000);
    const auto warm = builder.build(plan, requestFor(*plan));
    expectations.expect(warm.hasValue() && warm.scene->mediaStatistics().videoConversions == 1 &&
                            warm.scene->mediaStatistics().uploadCacheMisses == 1,
                        "the bypass-matrix warm build converts once");

    auto interactiveDefinition = plan->copyDefinition();
    interactiveDefinition.bypassOperationCache = true;
    const auto interactive = publish(std::move(interactiveDefinition));
    const auto interactiveHit = builder.build(interactive, requestFor(*interactive));
    expectations.expect(interactiveHit.hasValue() &&
                            interactiveHit.scene->mediaStatistics().uploadCacheHits == 1 &&
                            interactiveHit.scene->mediaStatistics().videoConversions == 0,
                        "an interactive plan reads the warm converted entry");

    // An interactive gesture MISS is decoded and converted but never inserted.
    const auto frameTwelve = rationalTime(1, 2);
    const auto gestureMiss = builder.build(interactive, requestFor(*interactive, frameTwelve));
    const auto gestureMissAgain = builder.build(interactive, requestFor(*interactive, frameTwelve));
    expectations.expect(gestureMiss.hasValue() && gestureMissAgain.hasValue() &&
                            gestureMiss.scene->mediaStatistics().uploadCacheMisses == 1 &&
                            gestureMiss.scene->mediaStatistics().videoConversions == 1 &&
                            gestureMissAgain.scene->mediaStatistics().uploadCacheMisses == 1 &&
                            gestureMissAgain.scene->mediaStatistics().videoConversions == 1,
                        "an interactive gesture miss is never inserted");
    const auto normalFrameTwelve = builder.build(plan, requestFor(*plan, frameTwelve));
    expectations.expect(normalFrameTwelve.hasValue() &&
                            normalFrameTwelve.scene->mediaStatistics().uploadCacheMisses == 1 &&
                            normalFrameTwelve.scene->mediaStatistics().videoConversions == 1,
                        "the gesture frame was never inserted into the shared upload cache");

    auto explicitRequest = requestFor(*plan);
    explicitRequest.bypassOperationCache = true;
    const auto explicitBuild = builder.build(plan, explicitRequest);
    expectations.expect(explicitBuild.hasValue() &&
                            explicitBuild.scene->mediaStatistics().uploadCacheHits == 0 &&
                            explicitBuild.scene->mediaStatistics().videoConversions == 1,
                        "an explicit bypass reconverts and never reads the prepared-upload cache");
}

// Real affine and all-mode blend coverage on a decoded video source: rotation, nonuniform/signed
// scale and anchor emit GpuAffine, and a non-Normal mode emits an explicit BlendV1 fold, each
// bit-exact against the live CPU frame.
void testVideoAffineAndBlend(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                             const std::filesystem::path& fixtures) {
    const auto path = fixtures / "numbered-h264.mp4";
    if (!std::filesystem::is_regular_file(path)) {
        expectations.expect(false, "the H.264 fixture is present");
        return;
    }
    const auto asset = videoAsset(path, 24000);
    // Rotation + nonuniform scale + anchor: a raster affine placement.
    {
        const auto plan = videoPlan(format(96, 64), asset,
                                    LayerValues{.position = {48.3, 32.1},
                                                .anchor = {1.0, -0.5},
                                                .scale = {1.5, 0.5},
                                                .rotation = 30.0},
                                    24000);
        checkVideoParity(expectations, evaluator, plan, requestFor(*plan),
                         "video rotated/nonuniform/anchor affine");
        auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
        const CpuGpuSceneBuilder builder(nullptr, context);
        const auto prepared = builder.build(plan, requestFor(*plan));
        bool sawAffine = false;
        if (prepared) {
            for (const auto& command : prepared.scene->commands()) {
                sawAffine = sawAffine ||
                            std::holds_alternative<bloom::runtime::GpuSceneAffineCommand>(command);
            }
        }
        expectations.expect(prepared.hasValue() && sawAffine,
                            "a rotated video layer emits a real GpuAffine command");
    }
    // Every non-Normal mode folds through an explicit BlendV1.
    {
        using bloom::core::BlendMode;
        const std::array modes{BlendMode::Add,       BlendMode::Multiply, BlendMode::Screen,
                               BlendMode::Overlay,   BlendMode::Darken,   BlendMode::Lighten,
                               BlendMode::Difference};
        std::uint64_t idBase = 24100;
        for (const auto mode : modes) {
            const auto plan = videoPlan(
                format(96, 64), asset,
                LayerValues{.position = {48.3, 32.1}, .opacity = 0.8, .blendMode = mode}, idBase);
            checkVideoParity(expectations, evaluator, plan, requestFor(*plan),
                             "video blend mode " + std::to_string(static_cast<unsigned>(mode)));
            idBase += 100;
        }
    }
    // Signed scale + proxy on a non-square PAR.
    {
        const auto plan = videoPlan(
            format(48, 32, pixelAspect(4, 3)), asset,
            LayerValues{.position = {24.3, 15.7}, .scale = {-1.25, 0.75}, .rotation = -20.0},
            24900);
        const auto extent = bloom::render::ImageExtent::create(24, 16);
        expectations.expect(static_cast<bool>(extent), "the signed/proxy video extent builds");
        if (!extent) {
            return;
        }
        auto request = requestFor(*plan);
        request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
        checkVideoParity(expectations, evaluator, plan, request, "video signed/proxy affine");
    }
}

void testVideoBudgetRefusal(Expectations& expectations, const std::filesystem::path& fixtures) {
    const auto path = fixtures / "numbered-prores.mov";
    if (!std::filesystem::is_regular_file(path)) {
        expectations.expect(false, "the ProRes fixture is present");
        return;
    }
    const auto asset = videoAsset(path, 25000);
    auto context = GpuSceneMediaContext::fromEvaluator(CpuCompositionEvaluator{});
    context.assetBaseDirectory = fixtures;
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto plan =
        videoPlan(format(256, 256), asset, LayerValues{.position = {128.0, 128.0}}, 25000);
    auto request = requestFor(*plan);
    request.pixelStorageByteLimit = 1U << 10U;
    const auto prepared = builder.build(plan, request);
    expectations.expect(
        !prepared.hasValue() &&
            prepared.diagnostic.code ==
                bloom::runtime::PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
        "a video scene under a tiny budget fails closed");
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path fixtures;
        if (argc >= 2) {
            fixtures = argv[1];
        } else if (const char* const environment = std::getenv("BLOOM_MEDIA_VIDEO_FIXTURES")) {
            fixtures = environment;
        }
        if (fixtures.empty() || !std::filesystem::is_directory(fixtures)) {
            std::cerr << "FAIL: a real media fixture directory is required (argv[1] or "
                         "BLOOM_MEDIA_VIDEO_FIXTURES); this test never skips as a pass\n";
            return 1;
        }

        Expectations expectations;
        const CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(fixtures);
        evaluator.setVideoCacheByteBudget(std::size_t{64} << 20U);

        testAllFixturesDistinctFrames(expectations, evaluator, fixtures);
        testProxyNonSquareParAndFractional(expectations, evaluator, fixtures);
        testVideoWarmReuse(expectations, evaluator, fixtures);
        testVideoBypassMatrix(expectations, evaluator, fixtures);
        testVideoAffineAndBlend(expectations, evaluator, fixtures);
        testVideoBudgetRefusal(expectations, fixtures);
        if (!expectations.ok()) {
            std::cerr << "FAIL: GPU media video scene preparation expectations failed\n";
            return 1;
        }
        std::cout << "PASS: CPU GPU media video scene preparation\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
