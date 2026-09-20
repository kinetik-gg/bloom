#include <algorithm>
#include <atomic>
#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/video/audio.hpp>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
// GPU video/range proof: the real sequence runner drives the real per-frame output attempts with
// the real provider attached, and the surfaced result must show actual GPU evaluation with positive
// native dispatches and exactly one final readback per GPU-evaluated frame. Skips (NOTE) when no
// loader is configured; `--require-device` turns an absent device into a failure.
void testGpuVideoProvenance(Fixture& f, const std::filesystem::path& directory,
                            const bool requireDevice) {
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        if (requireDevice)
            throw std::runtime_error("--require-device needs BLOOM_TEST_VULKAN_LOADER");
        std::cout << "NOTE: BLOOM_TEST_VULKAN_LOADER unset; skipping GPU video provenance\n";
        return;
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
        return;
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
}
void tests(const std::filesystem::path& directory, const bool requireDevice) {
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    Fixture f(directory);
    for (const auto preset :
         {output::OutputPresetV1::ProResMovV1, output::OutputPresetV1::DnxhrMxfV1,
          output::OutputPresetV1::PcmWavV1}) {
        const auto path = directory / (preset == output::OutputPresetV1::ProResMovV1  ? "motion.mov"
                                       : preset == output::OutputPresetV1::DnxhrMxfV1 ? "motion.mxf"
                                                                                      : "mix.wav");
        const auto result = f.run(f.request(path, preset));
        verify(result, path, preset != output::OutputPresetV1::PcmWavV1);
        if (preset == output::OutputPresetV1::ProResMovV1)
            check(need(result.evidence)
                      .implementationNote.starts_with(media::provider::kProResExportNote),
                  "required ProRes evidence wording");
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
    testGpuVideoProvenance(f, directory, requireDevice);
}
} // namespace
int main(int argc, char** argv) {
    try {
        check(argc == 2 || argc == 3, "fixture directory [--require-device]");
        const bool requireDevice = argc == 3 && std::string_view(argv[2]) == "--require-device";
        tests(argv[1], requireDevice);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
