#include "gpu_route_proof_export_support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/host/frame_range_runner.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/media/provider/contract.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/video/audio.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <unistd.h>
using namespace bloom;
namespace {
void check(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
template <typename T> T need(std::optional<T> value) {
    if (!value)
        throw std::runtime_error("Missing fixture value");
    return std::move(*value);
}
template <typename T> T need(media::provider::Result<T> value) {
    if (const auto* e = std::get_if<media::provider::Unavailable>(&value))
        throw std::runtime_error(e->detail);
    return std::get<T>(std::move(value));
}

namespace routeproof = bloom::gpu_route_proof_export;
enum class GpuProofOutcome : std::uint8_t { NotRequested, Skipped, Ran };

// The per-frame production identity material the sequence runner can genuinely observe: the real
// compiled plan identity plus the exact frame request and preset. The runner compiles the same
// plan, so this is production plan/request identity, not a placeholder.
struct FrameIdentityFields final {
    std::string_view routeId;
    std::uint64_t projectId = 0;
    std::uint64_t compositionId = 0;
    std::uint64_t sourceRevision = 0;
    std::uint64_t outputIndex = 0;
    std::uint64_t operationCount = 0;
    std::uint32_t planSemantics = 0;
    std::uint32_t animationSamplingSemantics = 0;
    std::uint64_t frameIndex = 0;
    std::int64_t timeNumerator = 0;
    std::int64_t timeDenominator = 1;
    std::uint64_t preset = 0;
    std::string_view provider;
};

[[nodiscard]] std::string frameIdentityHex(const FrameIdentityFields& fields) {
    routeproof::CanonicalWriter writer;
    writer.text("bloom.gpu.route.export-frame-identity.v1");
    writer.text(fields.routeId);
    writer.u64(fields.projectId);
    writer.u64(fields.compositionId);
    writer.u64(fields.sourceRevision);
    writer.u64(fields.outputIndex);
    writer.u64(fields.operationCount);
    writer.u32(fields.planSemantics);
    writer.u32(fields.animationSamplingSemantics);
    writer.u64(fields.frameIndex);
    writer.i64(fields.timeNumerator);
    writer.i64(fields.timeDenominator);
    writer.u64(fields.preset);
    writer.text(fields.provider);
    return routeproof::sha256Hex(writer.bytes());
}

// The actual paired GPU/CPU comparison of one independently decoded video frame. The same encoder
// contract (same ProRes preset/profile/settings) produced both files, so the only difference is the
// input process frame; the lossy codec tolerance below is the binding contract, not an arbitrary
// slack. ProRes is 4:2:2 without alpha, so alpha is not part of the compared planes.
struct PlaneComparison final {
    bool comparable = false;
    std::uint64_t comparedBytes = 0;
    std::uint64_t mismatchedBytes = 0;
    std::uint64_t maxDelta = 0;
};

[[nodiscard]] PlaneComparison compareFrameProduct(const media::provider::FrameProduct& gpu,
                                                  const media::provider::FrameProduct& cpu,
                                                  const std::uint32_t tolerance) {
    PlaneComparison comparison;
    if (gpu.format != cpu.format || gpu.planes.size() != cpu.planes.size() || gpu.planes.empty()) {
        return comparison;
    }
    for (std::size_t plane = 0; plane < cpu.planes.size(); ++plane) {
        if (gpu.planes[plane].width != cpu.planes[plane].width ||
            gpu.planes[plane].height != cpu.planes[plane].height ||
            gpu.planes[plane].stride != cpu.planes[plane].stride ||
            gpu.planes[plane].bytes.size() != cpu.planes[plane].bytes.size()) {
            return comparison;
        }
    }
    comparison.comparable = true;
    for (std::size_t plane = 0; plane < cpu.planes.size(); ++plane) {
        const auto& gpuBytes = gpu.planes[plane].bytes;
        const auto& cpuBytes = cpu.planes[plane].bytes;
        comparison.comparedBytes += cpuBytes.size();
        for (std::size_t index = 0; index < cpuBytes.size(); ++index) {
            const auto gpuValue = std::to_integer<std::uint32_t>(gpuBytes[index]);
            const auto cpuValue = std::to_integer<std::uint32_t>(cpuBytes[index]);
            const auto delta = gpuValue > cpuValue ? gpuValue - cpuValue : cpuValue - gpuValue;
            comparison.maxDelta = std::max(comparison.maxDelta, static_cast<std::uint64_t>(delta));
            if (delta > tolerance) {
                ++comparison.mismatchedBytes;
            }
        }
    }
    return comparison;
}

// Decodes every video frame of `path` independently through the production FFmpeg worker session.
[[nodiscard]] std::vector<media::provider::FrameProduct>
decodeVideoFrames(const std::filesystem::path& path) {
    std::vector<media::provider::FrameProduct> frames;
    media::video::VideoDecodeSession session(path);
    const auto probe = need(session.probe());
    std::uint32_t stream = 0;
    std::uint64_t frameCount = 0;
    bool found = false;
    for (const auto& descriptor : probe.streams) {
        if (descriptor.kind == media::provider::MediaKind::Video) {
            stream = descriptor.id;
            frameCount = descriptor.frameCount;
            found = true;
            break;
        }
    }
    if (!found) {
        return frames;
    }
    media::video::DecodedVideoCache cache(256U * 1024U * 1024U);
    for (std::uint64_t index = 0; index < frameCount; ++index) {
        auto decoded = session.frame(probe, stream, index, 0, &cache);
        if (std::get_if<media::provider::Unavailable>(&decoded) != nullptr) {
            break;
        }
        frames.push_back(*std::get<std::shared_ptr<const media::provider::FrameProduct>>(decoded));
    }
    return frames;
}

void publishVideoProof(const std::filesystem::path& proofDirectory,
                       const host::SequenceExportResultV1& gpuResult,
                       const std::vector<std::string>& identityHex,
                       const std::vector<routeproof::FrameEvidence>& evidence,
                       const std::uint64_t frameWidth, const std::uint64_t frameHeight) {
    routeproof::ExportProofCounters counters;
    counters.deviceOwnershipEpoch = gpuResult.gpuDeviceOwnershipEpoch;
    counters.nativeDispatches = gpuResult.gpuNativeDispatches;
    counters.verifiedFrames = identityHex.size();
    counters.readbackSubmissions = gpuResult.gpuReadbacks;
    // The accepted final readback transfers one process payload; no separate production payload
    // counter exists yet, so the real submission count is the payload count, not a guess.
    counters.payloads = gpuResult.gpuReadbacks;
    counters.transferredBytes =
        routeproof::processPayloadBytes(gpuResult.gpuReadbacks, frameWidth, frameHeight);
    std::string nonce;
    if (!routeproof::readProofNonce(proofDirectory, nonce)) {
        throw std::runtime_error("video route proof: a fresh run nonce is required");
    }
    const auto processDigest = routeproof::orderedIdentityDigest(identityHex);
    const auto capturedEvidenceDigest = routeproof::evidenceDigest(evidence);
    const auto written =
        routeproof::publishExportProof(proofDirectory, nonce, "route.export.video",
                                       bloom::runtime::GpuRouteHarnessKind::VideoExport, counters,
                                       processDigest, capturedEvidenceDigest);
    if (!written.written) {
        throw std::runtime_error("video route proof was rejected: " + written.detail);
    }
    std::cout << "PASS(route-proof) route.export.video frames=" << counters.verifiedFrames
              << " dispatches=" << counters.nativeDispatches
              << " readbacks=" << counters.readbackSubmissions
              << " bytes=" << counters.transferredBytes << '\n';
}

struct Fixture {
    document::NewProject initial =
        document::makeNewProject("Export", "Motion", core::RationalTime::fromInteger(2),
                                 need(document::CompositionFormat::create(256, 128)));
    document::Document doc{std::move(initial.project)};
    document::AssetId asset;
    runtime::NodeDefinitionRegistry registry;
    runtime::SnapshotCompiler compiler{registry};
    runtime::TaskScheduler scheduler;
    host::PublicationCoordinator publications = need(host::PublicationCoordinator::create());
    output::ExportResourceLedgerV1 ledger;
    platform::StagedArtifactCoordinator artifacts = [] {
        auto result = platform::StagedArtifactCoordinator::create({});
        check(static_cast<bool>(result), "artifact coordinator");
        return std::move(result).takeCoordinator();
    }();
    explicit Fixture(const std::filesystem::path& directory) {
        media::provider::Bytes pcm;
        const auto text = [&](std::string_view value) {
            for (char c : value)
                pcm.push_back(static_cast<std::byte>(c));
        };
        const auto number = [&](std::uint64_t value, unsigned width) {
            for (unsigned i = 0; i < width; ++i)
                pcm.push_back(static_cast<std::byte>((value >> (8U * i)) & 255U));
        };
        text("RIFF");
        number(384036, 4);
        text("WAVEfmt ");
        number(16, 4);
        number(1, 2);
        number(2, 2);
        number(48000, 4);
        number(192000, 4);
        number(4, 2);
        number(16, 2);
        text("data");
        number(384000, 4);
        for (std::uint64_t i = 0; i < 96000; ++i)
            for (unsigned c = 0; c < 2; ++c)
                number(static_cast<std::uint16_t>((static_cast<int>((i + c) % 997) - 498) * 32), 2);
        const auto source = std::filesystem::absolute(directory / "reference.wav");
        {
            std::ofstream file(source, std::ios::binary);
            file.write(reinterpret_cast<const char*>(pcm.data()),
                       static_cast<std::streamsize>(pcm.size()));
            check(static_cast<bool>(file), "reference PCM source");
        }
        check(runtime::registerBuiltInNodeDefinitions(registry), "node registry");
        registry.freeze();
        auto snapshot = doc.snapshot();
        auto draft = doc.draft(snapshot);
        asset = need(draft.ids().allocateAsset());
        document::AssetRecord record;
        record.id = asset;
        record.kind = document::AssetKind::Audio;
        record.name = "PCM reference";
        record.locator.path = "reference.wav";
        record.locator.relinkHint = "file://" + source.string();
        record.contentDigest = media::provider::digestBytes(pcm);
        record.rate = 48000;
        record.channels = 2;
        record.frames = 96000;
        record.duration = core::RationalTime::fromInteger(2);
        check(draft.project().addAsset(record), "audio asset");
        check(doc.commit(snapshot.revision(), std::move(draft)).committed(), "asset commit");
        commands::CommandStack stack(doc);
        const auto apply = [&]<typename T, typename... Args>(Args&&... args) {
            commands::Transaction transaction("Export fixture", doc.snapshot().revision());
            transaction.emplace<T>(initial.initialCompositionId, std::forward<Args>(args)...);
            auto result = stack.execute(std::move(transaction));
            check(result.changed(), "fixture command");
            return result;
        };
        const auto solid = apply.operator()<commands::AddSolidLayer>(
            "Moving", core::Color4d{0.25, 0.25, 0.25, 1}, document::Vec2d{128, 64});
        const auto position = need(
            solid.outputId<document::ParameterId>(commands::kAddSolidLayerPositionParameterOutput));
        const auto animation =
            apply.operator()<commands::CreateAnimationForParameter>(position, core::RationalTime{});
        const auto curve =
            need(animation.outputId<document::AnimationCurveId>(commands::kAnimationCurveOutput));
        (void)apply.operator()<commands::InsertVec2Keyframe>(
            curve, need(core::RationalTime::create(47, 24)), document::Vec2d{180, 64});
        (void)apply.operator()<commands::AddAudioLayer>(asset);
    }
    host::SequenceExportRequestV1 request(const std::filesystem::path& path,
                                          output::OutputPresetV1 preset) {
        host::SequenceExportRequestV1 r{
            .composition = {doc.snapshot(), initial.initialCompositionId},
            .range = {path, 0, 47, document::FrameRate::framesPerSecond24(),
                      core::RationalTime::fromInteger(2)},
            .bwfDescription = {},
            .assetBaseDirectory = {},
            .worker = {}};
        r.preset = preset;
        if (preset == output::OutputPresetV1::DnxhrMxfV1)
            r.profile = "dnxhr_hq";
        return r;
    }
    host::SequenceExportResultV1 run(host::SequenceExportRequestV1 request, bool cancel = false,
                                     bool crash = false) {
        std::atomic<std::int64_t> pid{0};
        request.worker.launched = [&](auto value) { pid.store(value); };
        host::SequenceExportRunnerV1 runner(scheduler, compiler, publications, artifacts, ledger,
                                            std::move(request));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        bool injected = false;
        while (!runner.result() && std::chrono::steady_clock::now() < deadline) {
            runner.poll();
            if (runner.stage() == host::SequenceExportStageV1::AwaitingApproval) {
                const auto* analysis = runner.analysis();
                check(analysis != nullptr, "media analysis");
                check(!runner.approve({}, need(runner.frameApprovalDigest())),
                      "wrong approval digest refused");
                check(runner.approve(analysis->digest, need(runner.frameApprovalDigest())),
                      "approve captured identities");
            }
            if (!injected && runner.encodedFrames() >= 8) {
                if (cancel)
                    runner.cancel();
                if (crash)
                    check(::kill(static_cast<pid_t>(pid.load()), SIGKILL) == 0,
                          "worker killed mid-export");
                injected = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(runner.result().has_value(), "composition export deadline");
        const auto releasedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (ledger.chargedBytes() != 0 && std::chrono::steady_clock::now() < releasedDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(ledger.chargedBytes() == 0,
              "terminal export releases frame and source audio reservations");
        return need(runner.result());
    }
};
void verify(const host::SequenceExportResultV1& r, const std::filesystem::path& path, bool video) {
    check(r.published(), r.failure ? r.failure->detail : "composition was not published");
    const auto qc = need(r.evidence);
    check(qc.audioSamples == 96000 && qc.duration == media::provider::Rational{2, 1},
          "sample count and rational duration");
    if (video)
        check(qc.frameCount == 48 && qc.firstFrame != qc.lastFrame, "48 frames with keyed motion");
    check(!qc.appleAuthorized && !qc.deliveryQualified && !qc.independentReader,
          "no unsupported authority claims");
    media::video::VideoDecodeSession decoder(path);
    const auto probe = need(decoder.probe());
    const auto decoded = need(media::video::decodeAudioClip(decoder, probe, {}));
    check(decoded.frames == 96000 && decoded.channels == 2, "published audio layout");
    for (std::size_t i = 0; i < 96000; ++i)
        for (unsigned c = 0; c < 2; ++c)
            check(decoded.planes[c][i] ==
                      static_cast<float>(static_cast<int>((i + c) % 997) - 498) / 1024.F,
                  "timeline mixer PCM sample exact");
    std::cout << path.filename() << " frames=" << qc.frameCount << " samples=" << qc.audioSamples
              << " max=" << qc.maximumError << " mean=" << qc.meanError << '\n';
}
// GPU video proof: the real sequence runner drives the real per-frame output attempts with the real
// provider attached, and the surfaced result must show actual GPU evaluation with positive native
// dispatches and exactly one final readback per GPU-evaluated frame. The published video is then
// decoded independently and every frame is compared against the corresponding CPU-exported video
// (same encoder contract); ProRes is lossy, so the compared 4:2:2 planes are required within the
// documented preset tolerance below, and the evidence digest is bound to the exact decoded bytes.
// Skips (exit 77) when no loader/device is configured; `--require-device` turns that into a
// failure.
GpuProofOutcome testGpuVideoProvenance(Fixture& f, const std::filesystem::path& directory,
                                       const std::filesystem::path& cpuReferencePath,
                                       const std::filesystem::path& proofDirectory,
                                       const bool requireDevice) {
    const auto skip = [&proofDirectory] {
        return proofDirectory.empty() ? GpuProofOutcome::NotRequested : GpuProofOutcome::Skipped;
    };
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        if (requireDevice)
            throw std::runtime_error("--require-device needs BLOOM_TEST_VULKAN_LOADER");
        std::cout << "NOTE: BLOOM_TEST_VULKAN_LOADER unset; skipping GPU video provenance\n";
        return skip();
    }
    auto provider = host::GpuExportProvider::create([&] {
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.enabled = true;
        options.loaderPath = std::filesystem::path(loader);
        return options;
    }());
    provider->prepare(f.scheduler);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!provider->prepared() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(provider->prepared(), "GPU video: provider bootstrap terminals");
    if (!provider->deviceAvailable()) {
        if (requireDevice)
            throw std::runtime_error("GPU video: required device unavailable");
        std::cout << "NOTE: no compatible Vulkan device; skipping GPU video provenance\n";
        return skip();
    }
    const auto path = directory / "gpu-motion.mov";
    auto request = f.request(path, output::OutputPresetV1::ProResMovV1);
    request.gpuProvider = provider;
    const auto result = f.run(std::move(request));
    verify(result, path, true);
    check(result.gpuEvaluatedFrames > 0, "GPU video: at least one frame evaluated on the GPU");
    check(result.gpuNativeDispatches > 0, "GPU video: positive native dispatches");
    check(result.gpuReadbacks == result.gpuEvaluatedFrames,
          "GPU video: exactly one final readback per GPU-evaluated frame");
    check(result.gpuDeviceOwnershipEpoch > 0,
          "GPU video: a genuine device ownership epoch is reported");
    std::cout << "GPU video frames=" << result.gpuEvaluatedFrames
              << " dispatches=" << result.gpuNativeDispatches
              << " readbacks=" << result.gpuReadbacks << " epoch=" << result.gpuDeviceOwnershipEpoch
              << '\n';

    if (proofDirectory.empty()) {
        return GpuProofOutcome::Ran;
    }

    // The real CPU export of the same fixture and encoder contract is the reference. ProRes 422 is
    // a lossy DCT codec and carries no alpha plane, so decoded samples cannot be compared for exact
    // equality. Both files are produced by the SAME encoder contract (same preset/profile/settings)
    // from process frames that agree to 2e-6, so the only divergence is codec rounding. The bound
    // below is the preset's lossy tolerance: 4/255 codes absorbs that rounding while still failing
    // a genuine GPU/CPU divergence, which is why a mismatched byte is a hard check failure.
    const auto gpuFrames = decodeVideoFrames(path);
    const auto cpuFrames = decodeVideoFrames(cpuReferencePath);
    check(!gpuFrames.empty() && gpuFrames.size() == cpuFrames.size(),
          "GPU video: both exports decode to the same frame count");
    constexpr std::uint32_t kProResTolerance = 4;
    const auto planResult =
        f.compiler.compile({f.doc.snapshot(), f.initial.initialCompositionId}, {});
    check(planResult.plan != nullptr, "GPU video: the plan compiles for identity");
    const host::FrameRangeRequestV1 range{
        .destination = path,
        .firstFrame = 0,
        .lastFrame = static_cast<std::uint64_t>(gpuFrames.empty() ? 0 : gpuFrames.size() - 1),
        .frameRate = document::FrameRate::framesPerSecond24(),
        .duration = core::RationalTime::fromInteger(2)};
    std::vector<std::string> identityHex;
    std::vector<routeproof::FrameEvidence> evidence;
    std::uint64_t frameWidth = 0;
    std::uint64_t frameHeight = 0;
    for (std::size_t index = 0; index < gpuFrames.size(); ++index) {
        const auto comparison =
            compareFrameProduct(gpuFrames[index], cpuFrames[index], kProResTolerance);
        check(comparison.comparable, "GPU video: decoded frame descriptors/format match the CPU "
                                     "export");
        check(comparison.mismatchedBytes == 0,
              "GPU video: decoded frame matches the CPU export within the ProRes tolerance");
        const auto time = host::FrameRangeRunnerV1::timeForFrame(range, index);
        check(time.has_value(), "GPU video: the frame has an exact composition time");
        FrameIdentityFields fields;
        fields.routeId = "route.export.video";
        fields.projectId = planResult.plan->projectId().value();
        fields.compositionId = planResult.plan->compositionId().value();
        fields.sourceRevision = planResult.plan->sourceRevision().value();
        fields.outputIndex = planResult.plan->output().value();
        fields.operationCount = planResult.plan->operations().size();
        fields.planSemantics = planResult.plan->planSemanticsVersion();
        fields.animationSamplingSemantics = planResult.plan->animationSamplingSemanticsVersion();
        fields.frameIndex = index;
        fields.timeNumerator = time->numerator();
        fields.timeDenominator = time->denominator();
        fields.preset = static_cast<std::uint64_t>(output::OutputPresetV1::ProResMovV1);
        fields.provider = "gpu-resident";
        const auto identity = frameIdentityHex(fields);
        identityHex.push_back(identity);
        routeproof::FrameEvidence frameEvidence;
        frameEvidence.identityHex = identity;
        frameEvidence.comparedPixels = comparison.comparedBytes;
        frameEvidence.mismatchedPixels = comparison.mismatchedBytes;
        frameEvidence.maxIntegerDelta = comparison.maxDelta;
        frameEvidence.alphaExact = true; // ProRes 4:2:2 carries no alpha plane
        routeproof::CanonicalWriter gpuWriter;
        gpuWriter.u64(gpuFrames[index].planes.size());
        for (const auto& plane : gpuFrames[index].planes)
            gpuWriter.text(routeproof::sha256Hex(plane.bytes));
        frameEvidence.gpuDecodedDigest = routeproof::sha256Hex(gpuWriter.bytes());
        routeproof::CanonicalWriter cpuWriter;
        cpuWriter.u64(cpuFrames[index].planes.size());
        for (const auto& plane : cpuFrames[index].planes)
            cpuWriter.text(routeproof::sha256Hex(plane.bytes));
        frameEvidence.cpuDecodedDigest = routeproof::sha256Hex(cpuWriter.bytes());
        evidence.push_back(std::move(frameEvidence));
        if (!gpuFrames[index].planes.empty()) {
            frameWidth = gpuFrames[index].planes.front().width;
            frameHeight = gpuFrames[index].planes.front().height;
        }
    }
    publishVideoProof(proofDirectory, result, identityHex, evidence, frameWidth, frameHeight);
    return GpuProofOutcome::Ran;
}
GpuProofOutcome tests(const std::filesystem::path& directory, const bool requireDevice,
                      const std::filesystem::path& proofDirectory) {
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    Fixture f(directory);
    std::filesystem::path cpuProResPath;
    for (const auto preset :
         {output::OutputPresetV1::ProResMovV1, output::OutputPresetV1::DnxhrMxfV1,
          output::OutputPresetV1::PcmWavV1}) {
        const auto path = directory / (preset == output::OutputPresetV1::ProResMovV1  ? "motion.mov"
                                       : preset == output::OutputPresetV1::DnxhrMxfV1 ? "motion.mxf"
                                                                                      : "mix.wav");
        const auto result = f.run(f.request(path, preset));
        verify(result, path, preset != output::OutputPresetV1::PcmWavV1);
        if (preset == output::OutputPresetV1::ProResMovV1) {
            cpuProResPath = path;
            check(need(result.evidence)
                      .implementationNote.starts_with(media::provider::kProResExportNote),
                  "required ProRes evidence wording");
        }
    }
    const auto path = directory / "cancel.mov";
    {
        std::ofstream file(path);
        file << "original";
    }
    const auto cancelled = f.run(f.request(path, output::OutputPresetV1::ProResMovV1), true);
    check(!cancelled.published() && cancelled.failure &&
              cancelled.failure->reason == media::provider::Error::Cancelled,
          "cancel typed result");
    {
        std::ifstream file(path);
        std::string bytes;
        file >> bytes;
        check(bytes == "original", "cancel retains existing destination");
    }
    const auto crashed =
        f.run(f.request(directory / "crash.mov", output::OutputPresetV1::ProResMovV1), false, true);
    check(!crashed.published() && crashed.failure &&
              crashed.failure->reason == media::provider::Error::Crashed &&
              !std::filesystem::exists(directory / "crash.mov"),
          "crash publishes nothing");
    verify(f.run(f.request(directory / "retry.mov", output::OutputPresetV1::ProResMovV1)),
           directory / "retry.mov", true);
    // Closing the authoring client abandons an in-flight worker without waiting on the UI.
    std::atomic<std::int64_t> abandonedPid{0};
    auto abandonedRequest =
        f.request(directory / "abandoned.mov", output::OutputPresetV1::ProResMovV1);
    abandonedRequest.worker.launched = [&](auto pid) { abandonedPid.store(pid); };
    auto abandoned = std::make_unique<host::SequenceExportRunnerV1>(
        f.scheduler, f.compiler, f.publications, f.artifacts, f.ledger,
        std::move(abandonedRequest));
    const auto abandonDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (abandoned->encodedFrames() < 8 && std::chrono::steady_clock::now() < abandonDeadline) {
        abandoned->poll();
        if (abandoned->stage() == host::SequenceExportStageV1::AwaitingApproval) {
            const auto* analysis = abandoned->analysis();
            check(analysis != nullptr, "abandonment analysis");
            check(abandoned->approve(analysis->digest, need(abandoned->frameApprovalDigest())),
                  "abandonment approval");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(abandoned->encodedFrames() >= 8, "abandon live encoder");
    abandoned.reset();
    const auto cleanupDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (f.ledger.chargedBytes() != 0 && std::chrono::steady_clock::now() < cleanupDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(f.ledger.chargedBytes() == 0 && !std::filesystem::exists(directory / "abandoned.mov"),
          "abandonment cleans resources without publication");
    check(abandonedPid.load() > 0 && ::kill(static_cast<pid_t>(abandonedPid.load()), 0) != 0,
          "abandoned worker reaped");
    auto oversized = f.request(directory / "oversized.mov", output::OutputPresetV1::ProResMovV1);
    oversized.queueByteLimit = 1;
    const auto refused = f.run(std::move(oversized));
    check(refused.failure && refused.failure->reason == media::provider::Error::Oversized,
          "frame queue budget enforced");
    return testGpuVideoProvenance(f, directory, cpuProResPath, proofDirectory, requireDevice);
}
} // namespace
int main(int argc, char** argv) {
    try {
        std::filesystem::path directory;
        std::filesystem::path proofDirectory;
        bool requireDevice = false;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument{argv[index]};
            if (argument == "--require-device") {
                requireDevice = true;
            } else if (argument == "--route-proof-dir" && index + 1 < argc) {
                proofDirectory = argv[++index];
            } else if (directory.empty()) {
                directory = argv[index];
            } else {
                throw std::runtime_error("usage: <fixture-directory> [--require-device] "
                                         "[--route-proof-dir <dir>]");
            }
        }
        if (directory.empty()) {
            throw std::runtime_error("usage: <fixture-directory> [--require-device] "
                                     "[--route-proof-dir <dir>]");
        }
        const auto outcome = tests(directory, requireDevice, proofDirectory);
        if (outcome == GpuProofOutcome::Skipped) {
            return 77;
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
