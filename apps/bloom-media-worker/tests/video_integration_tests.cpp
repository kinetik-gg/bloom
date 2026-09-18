#include <algorithm>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/media/video/audio.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/video_asset.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
using namespace bloom;
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
project::ProjectIoOperationMemory memory() {
    auto coordinator = project::ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}
void run(const std::filesystem::path& directory) {
    auto seed = document::makeNewProject("Video test", "Main", core::RationalTime::fromInteger(2));
    const auto composition = seed.initialCompositionId;
    const auto format = document::CompositionFormat::create(96, 64);
    if (!format)
        throw std::runtime_error("test composition format");
    seed.project.findComposition(composition)->setFormat(*format);
    document::Document document(std::move(seed.project));
    const auto snapshot = document.snapshot();
    auto draft = document.draft(snapshot);
    check(commands::AddSolidLayer(composition, "Background", {0.1, 0.2, 0.3, 1})
                  .apply(draft)
                  .status == commands::OperationStatus::Applied,
          "solid layer");
    const auto videoPath = directory / "integration-prores.mov";
    std::filesystem::copy_file(directory / "numbered-prores.mov", videoPath,
                               std::filesystem::copy_options::overwrite_existing);
    commands::ImportAssets imported({videoPath}, directory);
    if (!imported.diagnostic().empty())
        throw std::runtime_error(imported.diagnostic());
    check(imported.apply(draft).status == commands::OperationStatus::Applied,
          "worker-backed video import");
    const auto asset = draft.project().assets().front();
    check(asset.kind == document::AssetKind::Video && asset.frames == 48 && asset.channels == 1 &&
              asset.videoStreams.size() >= 2,
          "durable video metadata");
    auto invalidAsset = asset;
    invalidAsset.videoStreams.front().kind = 0;
    check(!invalidAsset.validate().ok(), "unknown stream kind is refused by document validation");
    invalidAsset = asset;
    invalidAsset.videoStreams.front().width = 16385;
    check(!invalidAsset.validate().ok(), "oversized persisted video dimensions are refused");
    const auto added = commands::AddImageLayer(composition, asset.id).apply(draft);
    check(added.status == commands::OperationStatus::Applied, "video layer drop command");
    for (const auto& output : added.outputs)
        if (output.name == "startFrameParameter")
            check(commands::SetParameterSource(composition,
                                               std::get<document::ParameterId>(output.id),
                                               document::ConstantValueSource{std::int64_t{12}})
                          .apply(draft)
                          .status == commands::OperationStatus::Applied,
                  "video start frame offsets both ports");
    check(document.commit(snapshot.revision(), std::move(draft)).committed(),
          "video document validates");
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({document.snapshot(), composition}, {});
    if (!compiled.plan) {
        for (const auto& diagnostic : compiled.diagnostics)
            std::cerr << diagnostic.summary << '\n';
        throw std::runtime_error("video plan compiles");
    }
    check(std::ranges::any_of(compiled.plan->operations(),
                              [](const auto& op) {
                                  return std::holds_alternative<runtime::CompiledVideoSource>(op);
                              }),
          "typed video arm");
    check(compiled.plan->planSemanticsVersion() == 7, "plan version 7");
    const auto time = core::RationalTime::create(35, 24);
    if (!time)
        throw std::runtime_error("scrub time");
    runtime::CpuCompositionEvaluator first, second;
    first.setAssetBaseDirectory(directory);
    second.setAssetBaseDirectory(directory);
    first.setVideoCacheByteBudget(65536);
    second.setVideoCacheByteBudget(65536);
    runtime::EvaluationRequest request{.time = *time,
                                       .output = compiled.plan->output(),
                                       .resolution = {},
                                       .pixelStorageByteLimit = std::size_t{16} * 1024 * 1024};
    const auto a = first.evaluate(compiled.plan, request, {}),
               b = second.evaluate(compiled.plan, request, {});
    for (const auto& d : a.diagnostics())
        std::cerr << d.summary << '\n';
    check(a.frame() && b.frame(), "Video over Solid renders at scrub");
    check(a.diagnostics().empty() && b.diagnostics().empty(),
          "qualified video has no evaluation warning");
    check(std::ranges::equal(std::as_bytes(a.frame()->processImage().pixels()),
                             std::as_bytes(b.frame()->processImage().pixels())),
          "Video over Solid is byte stable across two evaluators");
    const auto mix = first.evaluateAudioMix(compiled.plan, *time);
    check(mix && mix->clips.size() == 1 && mix->clips.front().assetId == asset.id,
          "video audio follows the clip mix");
    media::video::VideoDecodeSession session(directory / "numbered-prores.mov");
    const auto clip = media::video::decodeAudioClip(session, runtime::video::probeMetadata(asset));
    check(std::holds_alternative<media::audio::AudioBuffer>(clip),
          "video audio prepares a mixer buffer");
    check(std::get<media::audio::AudioBuffer>(clip).frames == 96000, "complete sample count");
    if (!mix || mix->clips.empty())
        throw std::runtime_error("audio clip missing");
    const auto& description = mix->clips.front();
    check(description.startTime.toSeconds() == 0.5, "video audio has the exact mapped start time");
    auto backend = std::make_unique<media::audio::playback::NullBackend>();
    auto* captured = backend.get();
    media::audio::playback::AudioEngine engine(std::move(backend));
    (void)engine.addClip({std::make_shared<const media::audio::AudioBuffer>(
                              std::get<media::audio::AudioBuffer>(clip)),
                          description.startTime, static_cast<float>(description.level),
                          description.muted, description.solo, description.endTime});
    check(!engine.play(*time) && !engine.renderForTesting(128), "video clip renders at scrub time");
    const auto samples = captured->captured();
    check(samples.size() == 256, "mono video audio routes to stereo");
    for (std::size_t i = 0; i < 128; ++i) {
        const auto expected =
            static_cast<float>(static_cast<int>((46000 + i) % 997) - 498) / 1024.0F;
        check(samples[i * 2] == expected && samples[i * 2 + 1] == expected,
              "video audio is sample-accurate after a 12-frame offset");
    }
    engine.stop();
    const auto saved = document.snapshot();
    const auto colors = document::makeBloomNeutralColorSettingsV1({});
    auto archive = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &saved, .colorSettings = &colors}, {}, memory());
    check(static_cast<bool>(archive), "video archive writes schema 1.17");
    auto opened = project::openProjectArchive(archive.archive()->bytes(), {}, memory());
    check(opened.outcome() == project::OpenArchiveOutcome::Opened, "video archive reopens");
    auto restored = std::move(opened).takeOpened();
    check(restored.schemaMinor == 17 &&
              *restored.document->snapshot().project().findAsset(asset.id) == asset,
          "video streams, timing and identity round trip");
    {
        std::ofstream changed(videoPath, std::ios::app | std::ios::binary);
        changed << "changed";
    }
    const auto stale = first.evaluate(compiled.plan, request, {});
    check(std::ranges::any_of(
              stale.diagnostics(),
              [](const auto& issue) { return issue.summary.find("changed") != std::string::npos; }),
          "warm video source flags changed content");
    const auto corruptPath = directory / "corrupt-import.mov";
    {
        std::ofstream corrupt(corruptPath);
        corrupt << "not media";
    }
    commands::ImportAssets corrupt({corruptPath}, directory);
    check(corrupt.prepared().empty() && corrupt.mediaDiagnostic() &&
              corrupt.mediaDiagnostic()->reason == media::provider::Error::Corrupt,
          "corrupt import preserves typed worker diagnostic");
}
} // namespace
int main(int argc, char** argv) {
    try {
        check(argc == 2, "fixture directory");
        run(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
