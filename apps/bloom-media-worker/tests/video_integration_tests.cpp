#include "tests/video_player_reference.hpp"
#include "video_source.hpp"
#include <algorithm>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/video_source_operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/media/video/audio.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/input_color_context.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/video_asset.hpp>
#include <cstdlib>
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
// Issue #366: retained decode sessions are admitted by estimated worker memory, not a fixed
// count. Construction launches no worker, so this checks admission only.
void sessionAdmission(const std::filesystem::path& directory) {
    runtime::detail::VideoSourceContext context;
    const auto hd = media::video::estimatedDecodeWorkerBytes(1920, 1080);
    const auto uhd = media::video::estimatedDecodeWorkerBytes(3840, 2160);
    context.setSessionByteBudget(hd * 6);
    for (int i = 0; i < 10; ++i)
        check(context.session("hd-" + std::to_string(i), directory / "numbered-h264.mp4", true,
                              hd) != nullptr,
              "session construction");
    check(context.retainedSessionCount() == 6, "memory budget sizes retained HD sessions");
    context.setSessionByteBudget(uhd * 3);
    for (int i = 0; i < 6; ++i)
        (void)context.session("uhd-" + std::to_string(i), directory / "numbered-h264.mp4", true,
                              uhd);
    check(context.retainedSessionCount() == 3, "larger streams admit fewer sessions");
    context.setSessionByteBudget(1);
    check(context.retainedSessionCount() ==
              runtime::detail::VideoSourceContext::kMinimumRetainedSessions,
          "two sources can always alternate");
    context.setSessionByteBudget(std::size_t{1} << 50U);
    for (int i = 0; i < 20; ++i)
        (void)context.session("small-" + std::to_string(i), directory / "numbered-h264.mp4", true,
                              1);
    check(context.retainedSessionCount() == runtime::detail::VideoSourceContext::kSnapshotFiles,
          "the source-snapshot file quota bounds retention");
    context.beginShutdown();
}
// Issue #365: a project saved with the synthesized "Rec.2100-HLG (Bloom)" input space reopens
// with that exact id, the space is re-synthesized when the pinned config is resolved, and the
// HLG video renders without a warning before and after the round trip.
void hlgInterpretationRoundTrip(const std::filesystem::path& directory) {
    auto seed = document::makeNewProject("HLG", "Main", core::RationalTime::fromInteger(1));
    const auto composition = seed.initialCompositionId;
    const auto format = document::CompositionFormat::create(64, 48);
    if (!format)
        throw std::runtime_error("test composition format");
    seed.project.findComposition(composition)->setFormat(*format);
    document::Document document(std::move(seed.project));
    const auto snapshot = document.snapshot();
    auto draft = document.draft(snapshot);
    const auto videoPath = directory / "integration-hlg.mp4";
    std::filesystem::copy_file(directory / "hlg2020-h264.mp4", videoPath,
                               std::filesystem::copy_options::overwrite_existing);
    commands::ImportAssets imported({videoPath}, directory);
    check(imported.apply(draft).status == commands::OperationStatus::Applied, "HLG import");
    const auto assetId = draft.project().assets().front().id;
    check(commands::SetAssetInputColor(assetId, document::AssetColorSpace::Auto,
                                       std::string(color::kBloomRec2100HlgColorSpaceId))
                  .apply(draft)
                  .status == commands::OperationStatus::Applied,
          "explicit synthesized HLG interpretation");
    check(commands::AddImageLayer(composition, assetId).apply(draft).status ==
              commands::OperationStatus::Applied,
          "HLG video layer");
    check(document.commit(snapshot.revision(), std::move(draft)).committed(), "HLG document");
    const auto render = [&](const document::Document& source) {
        runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled = compiler.compile({source.snapshot(), composition}, {});
        check(compiled.plan != nullptr, "HLG plan compiles");
        runtime::CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(directory);
        const auto time = core::RationalTime::create(1, 24);
        const auto result =
            evaluator.evaluate(compiled.plan,
                               {.time = *time,
                                .output = compiled.plan->output(),
                                .resolution = {},
                                .pixelStorageByteLimit = std::size_t{16} * 1024 * 1024},
                               {});
        for (const auto& d : result.diagnostics())
            std::cerr << d.summary << '\n';
        check(result.frame() && result.diagnostics().empty(),
              "HLG video renders through the synthesized space without a warning");
        return std::vector<std::byte>(
            std::as_bytes(result.frame()->processImage().pixels()).begin(),
            std::as_bytes(result.frame()->processImage().pixels()).end());
    };
    const auto before = render(document);
    const auto saved = document.snapshot();
    const auto colors = document::makeBloomNeutralColorSettingsV1({});
    auto archive = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &saved, .colorSettings = &colors}, {}, memory());
    check(static_cast<bool>(archive), "HLG project saves");
    auto opened = project::openProjectArchive(archive.archive()->bytes(), {}, memory());
    check(opened.outcome() == project::OpenArchiveOutcome::Opened, "HLG project reopens");
    auto restored = std::move(opened).takeOpened();
    check(restored.document->snapshot()
                  .project()
                  .findAsset(assetId)
                  ->interpretation.inputColorSpaceId == color::kBloomRec2100HlgColorSpaceId,
          "the synthesized HLG space id round trips");
    check(render(*restored.document) == before, "reopened HLG project renders identically");
}
// Owner decision (2026-10-24): an untagged video renders in Auto under the assumed size
// convention, with no evaluation diagnostic (so the frame stays RAM-cacheable). ADR 0032: Auto
// shows the assumed SDR stream through the sRGB texture space, so an explicit sRGB choice renders
// identically and the explicit Rec.709 camera choice does not. The same import-and-render path
// proves an 8K UHD source (issue #352).
void untaggedAssumedRender(const std::filesystem::path& directory, const std::filesystem::path& clip,
                           const std::uint32_t width, const std::uint32_t height,
                           const bool expectExplicitMatch) {
    const auto render = [&](const std::string& explicitId) {
        auto seed =
            document::makeNewProject("Untagged", "Main", core::RationalTime::fromInteger(1));
        const auto composition = seed.initialCompositionId;
        const auto format = document::CompositionFormat::create(width, height);
        if (!format)
            throw std::runtime_error("test composition format");
        seed.project.findComposition(composition)->setFormat(*format);
        document::Document document(std::move(seed.project));
        const auto snapshot = document.snapshot();
        auto draft = document.draft(snapshot);
        commands::ImportAssets imported({clip}, directory);
        check(imported.apply(draft).status == commands::OperationStatus::Applied,
              "untagged import");
        const auto assetId = draft.project().assets().front().id;
        if (!explicitId.empty())
            check(commands::SetAssetInputColor(assetId, document::AssetColorSpace::Auto,
                                               explicitId)
                          .apply(draft)
                          .status == commands::OperationStatus::Applied,
                  "explicit Rec.709 interpretation");
        check(commands::AddImageLayer(composition, assetId).apply(draft).status ==
                  commands::OperationStatus::Applied,
              "untagged video layer");
        check(document.commit(snapshot.revision(), std::move(draft)).committed(),
              "untagged document");
        runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled = compiler.compile({document.snapshot(), composition}, {});
        check(compiled.plan != nullptr, "untagged plan compiles");
        runtime::CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(directory);
        const auto time = core::RationalTime::create(1, 24);
        const auto result = evaluator.evaluate(
            compiled.plan,
            {.time = *time,
             .output = compiled.plan->output(),
             .resolution = {},
             .pixelStorageByteLimit = std::size_t{width} * height * 16 * 8},
            {});
        for (const auto& d : result.diagnostics())
            std::cerr << d.summary << '\n';
        check(result.frame() && result.diagnostics().empty(),
              "an untagged video renders in Auto without a diagnostic");
        return result.frame() ? std::vector<std::byte>(
                                    std::as_bytes(result.frame()->processImage().pixels()).begin(),
                                    std::as_bytes(result.frame()->processImage().pixels()).end())
                              : std::vector<std::byte>{};
    };
    const auto assumed = render({});
    if (expectExplicitMatch) {
        const auto config = runtime::detail::resolveInputColorConfig({});
        check(config && render(std::string(config->sRgbTextureColorSpaceId())) == assumed,
              "the SDR assumption renders exactly as the explicit sRGB texture choice");
        check(render(std::string(config->rec709VideoColorSpaceId())) != assumed,
              "the Rec.709 camera interpretation stays a distinct explicit choice");
    }
}
// Issue #381 / ADR 0032, CPU reference route: ordinary BT.709-tagged and untagged H.264 in Auto,
// shown through Bloom Neutral's default sRGB display view, matches FFmpeg's player-equivalent
// decode of the same frame within the stated tolerance. The previous Auto default (encoded R'G'B'
// read as linear light, then display-encoded again) fails it by about 33 codes on the 75% bars.
void playerReferenceRender(const std::filesystem::path& directory) {
    namespace reference = test::video_player_reference;
    const auto config = runtime::detail::resolveInputColorConfig({});
    check(config.has_value(), "the Bloom Neutral input config resolves");
    for (const auto& fixture : reference::kFixtures) {
        auto seed = document::makeNewProject("Player", "Main", core::RationalTime::fromInteger(1));
        const auto composition = seed.initialCompositionId;
        const auto format =
            document::CompositionFormat::create(reference::kWidth, reference::kHeight);
        if (!format)
            throw std::runtime_error("test composition format");
        seed.project.findComposition(composition)->setFormat(*format);
        document::Document document(std::move(seed.project));
        const auto snapshot = document.snapshot();
        auto draft = document.draft(snapshot);
        commands::ImportAssets imported({directory / std::string(fixture.clip)}, directory);
        check(imported.apply(draft).status == commands::OperationStatus::Applied,
              "player reference import");
        const auto assetId = draft.project().assets().front().id;
        check(commands::AddImageLayer(composition, assetId).apply(draft).status ==
                  commands::OperationStatus::Applied,
              "player reference layer");
        check(document.commit(snapshot.revision(), std::move(draft)).committed(),
              "player reference document");
        runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled = compiler.compile({document.snapshot(), composition}, {});
        check(compiled.plan != nullptr, "player reference plan compiles");
        runtime::CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(directory);
        const auto result = evaluator.evaluate(compiled.plan,
                                               {.time = core::RationalTime::fromInteger(0),
                                                .output = compiled.plan->output(),
                                                .resolution = {},
                                                .pixelStorageByteLimit = std::size_t{16} << 20U},
                                               {});
        for (const auto& d : result.diagnostics())
            std::cerr << d.summary << '\n';
        check(result.frame() != nullptr && result.diagnostics().empty(),
              "player reference frame renders in Auto without a diagnostic");
        const auto comparison =
            reference::compare(*config, result.frame()->processImage().pixels(),
                               directory / std::string(fixture.reference));
        std::cout << "player reference (CPU) " << fixture.label << ": " << comparison.summary()
                  << '\n';
        check(comparison.passes(),
              "Auto SDR video matches FFmpeg's player decode through the default sRGB view");
    }
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
    // Document 1.24: import records the timecode type and field order; a Video node selects
    // which audio stream plays, creating its parameter on first use, and refuses a missing one.
    check(asset.videoStreams.front().timecode == "01:00:00:00" &&
              asset.videoStreams.front().timecodeType == 1 &&
              asset.videoStreams.front().fieldOrder <= 1,
          "import records timecode type and field order");
    const auto* videoNode = [&]() -> const document::NodeRecord* {
        for (const auto& node : draft.project().findComposition(composition)->graph().nodes())
            if (node.typeId == "bloom.video-source")
                return &node;
        return nullptr;
    }();
    check(videoNode != nullptr, "video source node");
    const auto videoNodeId = videoNode->id;
    check(commands::SetVideoAudioStream(composition, videoNodeId, 1).apply(draft).status ==
              commands::OperationStatus::Rejected,
          "a missing audio stream is refused");
    check(commands::SetVideoAudioStream(composition, videoNodeId, 0).apply(draft).status ==
              commands::OperationStatus::Applied,
          "the first stream selection creates the node's audio stream parameter");
    check(commands::SetVideoAudioStream(composition, videoNodeId, 0).apply(draft).status ==
              commands::OperationStatus::NoChange,
          "re-selecting the same stream is no change");
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
    check(compiled.plan->planSemanticsVersion() ==
              runtime::kCompiledCompositionPlanSemanticsVersion,
          "current plan semantics version");
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
    const auto clip = media::video::decodeAudioClip(session, runtime::video::probeMetadata(asset),
                                                    std::uint64_t{96000});
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
    check(static_cast<bool>(archive), "video archive writes the current schema");
    auto opened = project::openProjectArchive(archive.archive()->bytes(), {}, memory());
    check(opened.outcome() == project::OpenArchiveOutcome::Opened, "video archive reopens");
    auto restored = std::move(opened).takeOpened();
    check(restored.schemaMinor >= 21 &&
              *restored.document->snapshot().project().findAsset(asset.id) == asset,
          "video streams, timing and identity round trip");
    const auto* restoredNode = restored.document->snapshot()
                                   .project()
                                   .findComposition(composition)
                                   ->graph()
                                   .findNode(videoNodeId);
    check(restoredNode != nullptr &&
              std::ranges::any_of(restoredNode->parameters,
                                  [](const auto& binding) {
                                      return binding.role ==
                                             document::kVideoAudioStreamParameterRole;
                                  }),
          "the Video node's audio stream selection round trips");
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
        sessionAdmission(argv[1]);
        hlgInterpretationRoundTrip(argv[1]);
        const std::filesystem::path fixtures(argv[1]);
        playerReferenceRender(fixtures);
        untaggedAssumedRender(fixtures, fixtures / "untagged-hd-prores.mov", 1280, 720, true);
        untaggedAssumedRender(fixtures, fixtures / "untagged-h264.mp4", 64, 48, false);
        // Issue #352: an 8K UHD source imports and renders through the evaluator.
        untaggedAssumedRender(fixtures, fixtures / "uhd8k-prores.mov", 7680, 4320, false);
        // A local untagged HD clip (never committed), e.g. an owner's 720p camera file.
        if (const char* local = std::getenv("BLOOM_LOCAL_UNTAGGED_HD_CLIP"); local && *local)
            untaggedAssumedRender(std::filesystem::path(local).parent_path(), local, 1280, 720,
                                  true);
        run(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
