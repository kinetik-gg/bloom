// End-to-end audio transport check: an imported WAV placed as an audio layer must reach the
// playback engine through the same wiring apps/bloom/main.cpp uses (asset decode -> compiled
// audio mix -> AudioPlaybackSession -> PlaybackController -> AudioEngine) and produce non-silent
// samples when the transport plays.
#include "window_fixture.hpp"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <bloom/commands/operations.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/ui/audio_playback_session.hpp>
#include <bloom/ui/composition_plan_cache.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <numbers>
#include <string>
#include <string_view>

namespace {
using namespace bloom;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void writeTone(const QString& path) {
    QByteArray wave;
    const auto appendLe16 = [&wave](const std::uint16_t value) {
        wave.append(static_cast<char>(value & 0xffU));
        wave.append(static_cast<char>((value >> 8U) & 0xffU));
    };
    const auto appendLe32 = [&wave](const std::uint32_t value) {
        wave.append(static_cast<char>(value & 0xffU));
        wave.append(static_cast<char>((value >> 8U) & 0xffU));
        wave.append(static_cast<char>((value >> 16U) & 0xffU));
        wave.append(static_cast<char>((value >> 24U) & 0xffU));
    };
    const std::uint32_t sampleCount = 48'000;
    const std::uint32_t dataBytes = sampleCount * static_cast<std::uint32_t>(sizeof(std::int16_t));
    wave.append("RIFF", 4);
    appendLe32(36U + dataBytes);
    wave.append("WAVEfmt ", 8);
    appendLe32(16);
    appendLe16(1);
    appendLe16(1);
    appendLe32(48'000);
    appendLe32(48'000U * static_cast<std::uint32_t>(sizeof(std::int16_t)));
    appendLe16(static_cast<std::uint16_t>(sizeof(std::int16_t)));
    appendLe16(16);
    wave.append("data", 4);
    appendLe32(dataBytes);
    for (std::uint32_t index = 0; index < sampleCount; ++index) {
        const auto sample = std::sin(2.0 * std::numbers::pi * 440.0 * index / 48'000.0);
        appendLe16(static_cast<std::uint16_t>(static_cast<std::int16_t>(sample * 12'000.0)));
    }
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(wave) == wave.size(), "tone fixture");
}

int run(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomAudioTests");
    app.setApplicationName("AudioTransport");
    ui::kit::installKinetikTheme(app);
    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory");
    writeTone(directory.filePath("Tone.wav"));

    ui::test::WindowFixture fixture([&](auto& target) {
        commands::Transaction setup("Audio composition", target.session.snapshot().revision());
        setup.emplace<commands::SetCompositionDuration>(target.session.compositionId(),
                                                        core::RationalTime::fromInteger(2));
        require(target.session.executeTransaction(std::move(setup)).succeeded(), "setup");
        target.assets->importFiles({directory.filePath("Tone.wav")});
        QElapsedTimer timer;
        timer.start();
        while (target.assets->busy() && timer.elapsed() < 15000)
            QTest::qWait(10);
        require(target.session.snapshot().project().assets().size() == 1, "WAV import");
        const auto asset = target.session.snapshot().project().assets().front();
        commands::Transaction add("Place audio", target.session.snapshot().revision());
        add.emplace<commands::AddAudioLayer>(target.session.compositionId(), asset.id);
        require(target.session.executeTransaction(std::move(add)).succeeded(), "audio layer");
    });
    const auto assetId = fixture.session.snapshot().project().assets().front().id;
    QElapsedTimer wait;
    wait.start();
    while (fixture.assets->audioBuffer(assetId) == nullptr && wait.elapsed() < 15000)
        QTest::qWait(10);
    require(fixture.assets->audioBuffer(assetId) != nullptr, "stage 1: decoded audio buffer");
    std::cout << "stage 1 ok: buffer frames " << fixture.assets->audioBuffer(assetId)->frames
              << '\n';

    // Same wiring as apps/bloom/main.cpp.
    ui::PlaybackController playback(
        fixture.session, *fixture.preview, [] { return std::chrono::steady_clock::now(); }, 16ms,
        [](std::uint64_t) { return false; });
    ui::AudioPlaybackSession audioPlaybackSession(fixture.session, fixture.compiler,
                                                  std::make_shared<ui::CompiledPlanCache>(),
                                                  fixture.evaluator);
    auto backendOwner = std::make_unique<media::audio::playback::NullBackend>();
    auto* backend = backendOwner.get();
    auto engineOwner =
        std::make_unique<media::audio::playback::AudioEngine>(std::move(backendOwner));
    auto* engine = engineOwner.get();
    playback.setAudioEngine(std::move(engineOwner));
    std::size_t appliedClips = 0;
    const auto applyAudioMix = [&] {
        const auto& mix = audioPlaybackSession.mix();
        if (!mix.has_value()) {
            appliedClips = 0;
            playback.setAudioMix({}, {});
            return;
        }
        std::vector<media::audio::playback::AudioClip> clips;
        for (const auto& description : mix->clips) {
            const auto buffer = fixture.assets->audioBuffer(description.assetId);
            if (buffer == nullptr)
                continue;
            clips.push_back({.buffer = *buffer,
                             .startTime = description.startTime,
                             .level = static_cast<float>(description.level),
                             .muted = description.muted,
                             .solo = description.solo,
                             .endTime = description.endTime});
        }
        appliedClips = clips.size();
        playback.setAudioMix(*mix, std::move(clips));
    };
    QObject::connect(&audioPlaybackSession, &ui::AudioPlaybackSession::mixChanged, &playback,
                     applyAudioMix);
    QObject::connect(fixture.assets.get(), &ui::AssetController::changed, &playback, applyAudioMix);
    require(audioPlaybackSession.refresh(), "stage 2: mix derived from the live revision");
    applyAudioMix();
    const auto& mix = audioPlaybackSession.mix();
    if (!mix.has_value())
        throw std::runtime_error("stage 3: published mix");
    std::cout << "stage 3 ok: mix clips " << mix->clips.size() << " applied clips " << appliedClips
              << '\n';
    require(!mix->clips.empty(), "stage 3: mix has clips");
    require(appliedClips == 1, "stage 4: clip applied to the engine");

    (void)fixture.session.setCurrentTime(core::RationalTime::fromInteger(0));
    playback.play();
    require(playback.state() == ui::PlaybackState::Playing, "stage 5: transport playing");
    require(engine->isPlaying(), "stage 5: engine playing");
    require(!engine->renderForTesting(4096).has_value(), "stage 6: render");
    float peak = 0.0F;
    for (const float sample : backend->captured())
        peak = std::max(peak, std::abs(sample));
    std::cout << "stage 6: captured " << backend->captured().size() << " samples, peak " << peak
              << '\n';
    require(peak > 0.1F, "stage 6: audible output");
    playback.pause();

    // Regression: a layout-only edit changes the revision without changing any pixel. The mix
    // must survive it -- the live revision, not a shown frame, is what audio follows.
    {
        std::map<document::NodeId, document::Vec2d> positions;
        for (const auto& node : fixture.session.composition()->graph().nodes())
            positions.emplace(node.id, document::Vec2d{40.0, 40.0});
        commands::Transaction move("Move nodes", fixture.session.snapshot().revision());
        move.emplace<commands::MoveNodes>(fixture.session.compositionId(), std::move(positions));
        require(fixture.session.executeTransaction(std::move(move)).succeeded(), "move nodes");
        const auto& moved = audioPlaybackSession.mix();
        require(moved.has_value() && !moved->clips.empty(),
                "stage 7: mix survives a layout-only edit");
        require(appliedClips == 1, "stage 7: engine still holds the clip");
    }

    // Direct feeds: a Layer, and then an Audio source, wired straight into the composition's
    // audio input are one-clip mixes rather than an unsupported preview.
    document::NodeId layerNode;
    document::NodeId sourceNode;
    document::NodeId outputNode;
    for (const auto& node : fixture.session.composition()->graph().nodes()) {
        if (node.typeId == document::kAudioSourceNodeType)
            sourceNode = node.id;
        else if (node.typeId == document::kLayerOutputNodeType)
            layerNode = node.id;
    }
    if (const auto& endpoint = fixture.session.composition()->graph().compositionOutput())
        outputNode = endpoint->nodeId;
    require(layerNode.isValid() && sourceNode.isValid() && outputNode.isValid(), "graph nodes");
    const auto wire = [&](const document::NodeId from, const std::string_view port,
                          const char* label) {
        commands::Transaction connect(label, fixture.session.snapshot().revision());
        connect.emplace<commands::ConnectPorts>(
            fixture.session.compositionId(), document::OutputPortRef{from, std::string(port)},
            document::NodeInputRef{outputNode,
                                   std::string(document::kCompositionOutputAudioInputPort)});
        require(fixture.session.executeTransaction(std::move(connect)).succeeded(), label);
        const auto& wired = audioPlaybackSession.mix();
        require(wired.has_value() && wired->clips.size() == 1, label);
    };
    wire(layerNode, document::kLayerOutputAudioOutputPort, "stage 8: layer wired to output");
    wire(sourceNode, document::kAudioSourceOutputPort, "stage 9: source wired to output");
    std::cout << "audio transport ok\n";
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
