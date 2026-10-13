// Opt-in CPU qualification driven by tools/quality/qualify_private_share_outage.py.
// The caller owns a disposable SSHFS mount; this process never mounts or disconnects anything.
#include "gpu_media_scene_preparation_test_support.hpp"
#include "gpu_media_video_scene_preparation_test_support.hpp"
#include "gpu_process_frame_preparation_private.hpp"
#include "image_source.hpp"
#include "video_source.hpp"

#include <bloom/platform/file_snapshot.hpp>

#include <chrono>
#include <thread>

namespace {
using bloom::runtime::GpuSceneMediaContext;
using bloom::runtime::GpuSceneMediaStatistics;
using bloom::runtime::MediaSourceRevisionPolicy;
using bloom::runtime::OperationCacheEntryKind;

void require(const bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void checkpoint(const char* ready, const char* expected) {
    std::cout << ready << '\n' << std::flush;
    std::string received;
    require(static_cast<bool>(std::getline(std::cin, received)) && received == expected,
            "private outage controller supplied the expected transition");
}

const bloom::runtime::GpuSceneUploadCommand& upload(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands())
        if (const auto* image = std::get_if<bloom::runtime::GpuSceneUploadCommand>(&command))
            return *image;
    throw std::runtime_error("production scene contains an actual decoded upload");
}

void requireWarm(const GpuSceneMediaStatistics& stats) {
    require(stats.sourceRevisionCacheHits == 1 && stats.sourceRevisionCaptures == 0 &&
                stats.sourceBytesRead == 0 && stats.sourceRevisionBytesHashed == 0 &&
                stats.sourceIoNanoseconds == 0 && stats.sourceHashNanoseconds == 0 &&
                stats.sourceProbeNanoseconds == 0 && stats.imageDecodeNanoseconds == 0 &&
                stats.imageConversions == 0 && stats.videoConversions == 0 &&
                stats.uploadCacheHits == 1,
            "retained warm production preparation performs no original I/O/hash/probe/decode");
}

void prepare(const std::filesystem::path& served, const std::filesystem::path& videoFixtures) {
    require(std::filesystem::is_directory(served), "caller created the private served directory");
    auto pixels = signedHdrPixels();
    writeExrRgba(served / "still.exr", 3, 2, pixels);
    pixels.front().green += 0.5F;
    writeExrRgba(served / "replacement.exr", 3, 2, pixels);
    std::filesystem::copy_file(videoFixtures / "numbered-prores.mov", served / "video.mov");
    std::filesystem::copy_file(videoFixtures / "alpha-prores.mov", served / "replacement.mov");
    std::cout << "PRIVATE_SHARE fixtures=realOpenExrAndProRes\n";
}

void exercise(const std::filesystem::path& mounted, const std::filesystem::path& spool) {
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(mounted);
    evaluator.setVideoCacheByteBudget(16U << 20U);
    auto context = legacyPreparedMediaContext(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto stillAsset = imageAsset(mounted / "still.exr", "Private still", 7300);
    const auto videoAssetRecord = videoAsset(mounted / "video.mov", 7400);
    const auto still = mediaPlan(format(32, 32), stillAsset, LayerValues{}, 7500);
    const auto video = videoPlan(format(32, 32), videoAssetRecord, LayerValues{}, 7600);
    auto stillRequest = requestFor(*still);
    auto videoRequest = requestFor(*video);
    stillRequest.mediaSourceRevisionPolicy = MediaSourceRevisionPolicy::Retained;
    videoRequest.mediaSourceRevisionPolicy = MediaSourceRevisionPolicy::Retained;
    const auto coldStill = builder.build(still, stillRequest);
    const auto coldVideo = builder.build(video, videoRequest);
    require(coldStill && coldVideo, "real mounted still/video prepare through production paths");
    const auto cpuStill = evaluator.evaluate(still, stillRequest, {});
    const auto cpuVideo = evaluator.evaluate(video, videoRequest, {});
    require(cpuStill.frame() && cpuVideo.frame(), "CPU reference memoizes both real source frames");
    for (const auto* cold : {&coldStill, &coldVideo}) {
        const auto& stats = cold->scene->mediaStatistics();
        require(stats.sourceRevisionCaptures == 1 && stats.sourceBytesRead > 0 &&
                    stats.sourceRevisionBytesHashed > 0 && stats.sourceIoNanoseconds > 0 &&
                    stats.sourceHashNanoseconds > 0,
                "cold source work genuinely captures and hashes mounted bytes");
    }
    const auto oldGeneration = context.operationCache->decodedMediaGeneration();
    const auto& source = std::get<bloom::runtime::CompiledImageSource>(still->operations().front());
    auto selected = bloom::runtime::detail::selectImageSource(
        source, stillRequest.time, still->format().frameRate(), mounted, {},
        stillRequest.colorIntent, context.operationCache.get(), MediaSourceRevisionPolicy::Retained,
        stillRequest.pixelStorageByteLimit, false);
    require(selected.available && selected.retainedRevision,
            "retained source selection exposes the actual immutable still revision");

    // An uncached frame request during outage must read the captured disk capability in the actual
    // worker, rather than reopen the artist path. Compare it against the same real pre-outage
    // frame.
    const auto quota = std::make_shared<bloom::platform::FileSnapshotQuota>(64ULL << 20U, 4);
    auto independent = std::make_unique<bloom::media::video::VideoDecodeSession>(
        mounted / "video.mov", bloom::media::video::VideoDecodeSession::defaultWorker(), quota,
        spool);
    auto probed = independent->probe();
    const auto* probe = std::get_if<provider::ProbeResult>(&probed);
    require(probe != nullptr, "real worker probes the mounted video");
    const auto stream = std::ranges::find(probe->streams, provider::MediaKind::Video,
                                          &provider::StreamDescriptor::kind);
    require(stream != probe->streams.end(), "mounted fixture contains video");
    const auto baseline = independent->frame(*probe, stream->id, 1, 0, nullptr, {}, {}, {}, {},
                                             {bloom::media::video::SourceRevisionPolicy::Retained});
    const auto* baselineFrame =
        std::get_if<std::shared_ptr<const provider::FrameProduct>>(&baseline);
    require(baselineFrame != nullptr, "actual worker produces the uncached frame oracle");
    bool cancelled = false;
    const auto cancelledCapture = bloom::platform::ReadOnlyFileSnapshot::capture(
        mounted / "video.mov", spool, quota, provider::Limits::sourceBytes,
        [&] { return cancelled; }, [&](std::uint64_t, std::uint64_t) { cancelled = true; });
    const auto* cancelledError = std::get_if<bloom::platform::ProcessFailure>(&cancelledCapture);
    require(cancelledError && cancelledError->code == bloom::platform::ProcessError::Cancelled &&
                quota->residentFiles() == 1,
            "cancelled in-progress capture frees its exact private disk quota");

    checkpoint("READY_OUTAGE", "OUTAGE");
    for (int iteration = 0; iteration < 30; ++iteration) {
        const auto warmStill = builder.build(still, stillRequest);
        const auto warmVideo = builder.build(video, videoRequest);
        require(warmStill && warmVideo, "both retained sources remain available during outage");
        requireWarm(warmStill.scene->mediaStatistics());
        requireWarm(warmVideo.scene->mediaStatistics());
        require(upload(*warmStill.scene).image == upload(*coldStill.scene).image &&
                    upload(*warmVideo.scene).image == upload(*coldVideo.scene).image,
                "warm outage reuses the exact immutable decoded allocations");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    bloom::platform::FileSnapshotStatistics work;
    bool retained = false;
    const auto uncached = independent->frame(
        *probe, stream->id, 1, 0, nullptr, {}, {}, {}, {},
        {bloom::media::video::SourceRevisionPolicy::Retained, false, &work, &retained});
    const auto* uncachedFrame =
        std::get_if<std::shared_ptr<const provider::FrameProduct>>(&uncached);
    require(
        uncachedFrame && **uncachedFrame == **baselineFrame && retained && work.bytesRead == 0 &&
            work.bytesHashed == 0,
        "actual worker decodes identical uncached pixels from anonymous snapshot during outage");
    std::cout << "PRIVATE_SHARE outageWarmCalls=60 originalBytesRead=0 originalBytesHashed=0 "
                 "probeNs=0 decodeNs=0 uncachedVideoWorkerParity=exact\n";

    auto freshStill = stillRequest;
    auto freshVideo = videoRequest;
    freshStill.mediaSourceRevisionPolicy = MediaSourceRevisionPolicy::Revalidate;
    freshVideo.mediaSourceRevisionPolicy = MediaSourceRevisionPolicy::Revalidate;
    require(!builder.build(still, freshStill) && !builder.build(video, freshVideo),
            "fresh selection refuses disconnected mounted sources despite retained uploads");
    auto cpuFinalStill = stillRequest;
    auto cpuFinalVideo = videoRequest;
    cpuFinalStill.requireVerifiedMediaSources = true;
    cpuFinalVideo.requireVerifiedMediaSources = true;
    require(!evaluator.evaluate(still, cpuFinalStill, {}).frame() &&
                !evaluator.evaluate(video, cpuFinalVideo, {}).frame(),
            "CPU final verification rejects unavailable sources before cached-frame publication");
    bloom::runtime::GpuProcessFrameEvaluatorOptions finalOptions;
    finalOptions.mediaContext = context;
    require(
        !bloom::runtime::detail::prepareExportScene(finalOptions, still, stillRequest, {}).scene &&
            !bloom::runtime::detail::prepareExportScene(finalOptions, video, videoRequest, {})
                 .scene,
        "production final preparation revalidates even explicitly Retained preview requests");
    context.operationCache->clearDecodedMedia();
    evaluator.clearDecodedVideoCache();
    context.preparedUploadCache->clear();
    require(context.operationCache->decodedMediaGeneration() > oldGeneration &&
                !builder.build(still, stillRequest) && !builder.build(video, videoRequest),
            "Refresh invalidates both retained authorities during actual outage");
    context.operationCache->store(
        "private-share-stale-revision", document::Revision{},
        {.image = {}, .values = {}, .bounds = {}, .sourceRevision = selected.sourceRevision},
        OperationCacheEntryKind::DecodedMedia, oldGeneration);
    require(!context.operationCache->find("private-share-stale-revision", document::Revision{}),
            "pre-Refresh source generation cannot republish retained authority");
    const auto decoded = bloom::media::decodeImage(*selected.sourceRevision);
    require(static_cast<bool>(decoded), "detached immutable still bytes remain safe after Refresh");
    selected.sourceRevision.reset();
    independent.reset();
    require(quota->residentFiles() == 0 && quota->residentBytes() == 0,
            "last snapshot release closes actual backing before exact disk quota returns");
    checkpoint("READY_RECONNECT", "RECONNECTED");

    require(!builder.build(still, stillRequest) && !builder.build(video, videoRequest),
            "reconnected same-path replacements fail the original strong content digests");
    const auto replacementStill = imageAsset(mounted / "still.exr", "Replacement still", 7700);
    const auto replacementVideo = videoAsset(mounted / "video.mov", 7800);
    require(replacementStill.contentDigest != stillAsset.contentDigest &&
                replacementVideo.contentDigest != videoAssetRecord.contentDigest,
            "reconnect detects changed bytes rather than relying on original timestamps");
    const auto newStill = mediaPlan(format(32, 32), replacementStill, LayerValues{}, 7900);
    const auto newVideo = videoPlan(format(32, 32), replacementVideo, LayerValues{}, 8000);
    const auto reconnectedStill = builder.build(newStill, requestFor(*newStill));
    const auto reconnectedVideo = builder.build(newVideo, requestFor(*newVideo));
    require(reconnectedStill && reconnectedVideo,
            "explicitly adopted replacement revisions prepare after reconnect");
    require(bloom::runtime::detail::prepareExportScene(finalOptions, newStill,
                                                       requestFor(*newStill), {})
                    .scene &&
                bloom::runtime::detail::prepareExportScene(finalOptions, newVideo,
                                                           requestFor(*newVideo), {})
                    .scene,
            "production final preparation accepts the newly verified revisions");
    std::cout << "PASS: actual private SSHFS outage/reconnect, retained warm sources, fresh/final "
                 "digest checks, Refresh generations, cancellation and snapshot retirement\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4)
            return 2;
        const std::string mode(argv[1]);
        if (mode == "--prepare")
            prepare(std::filesystem::path(argv[2]), std::filesystem::path(argv[3]));
        else if (mode == "--exercise")
            exercise(std::filesystem::path(argv[2]), std::filesystem::path(argv[3]));
        else
            return 2;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
