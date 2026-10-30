// #354 audio timing: audio honours the layer in/out window and the layer time mapping exactly as
// pictures do, and a video's embedded audio follows its picture's loop mode. Every case renders
// actual samples through the production mixer at 8 Hz against a 2 s ramp whose output sample n
// reads n + 1, so each expected value names the source sample independently of the mapper.

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::atomic<bool> countAllocations{false};
std::atomic<std::size_t> allocations{0};
} // namespace

void* operator new(const std::size_t size) {
    if (countAllocations.load(std::memory_order_relaxed))
        allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

using namespace bloom;
namespace {
int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
core::RationalTime at(const std::int64_t numerator, const std::int64_t denominator = 1) {
    const auto value = core::RationalTime::create(numerator, denominator);
    if (!value)
        throw std::logic_error("invalid fixture time");
    return *value;
}
constexpr auto kAudioAsset = document::AssetId::fromRaw(9000);
constexpr auto kVideoAsset = document::AssetId::fromRaw(9001);
constexpr std::uint32_t kRate = 8;

std::shared_ptr<const media::audio::AudioBuffer> ramp() {
    // 2 s at 48 kHz; the 8 Hz mixer reads source frame 6000 n for output sample n.
    std::vector<float> samples(96000);
    for (std::size_t index = 0; index < samples.size(); ++index)
        samples[index] = static_cast<float>(index / 6000 + 1);
    return std::make_shared<const media::audio::AudioBuffer>(
        media::audio::AudioBuffer{48000, 1, samples.size(), {std::move(samples)}});
}

struct Fixture {
    document::NewProject initial = makeProject();
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;

    static document::NewProject makeProject() {
        auto project = document::makeNewProject(
            "Audio timing", "Main", core::RationalTime::fromInteger(8),
            *document::CompositionFormat::create(4, 4, core::PixelAspectRatio::square(),
                                                 *document::FrameRate::create(kRate, 1)));
        document::AssetRecord audio;
        audio.id = kAudioAsset;
        audio.kind = document::AssetKind::Audio;
        audio.locator = {"file", "project-relative", "tone.wav", "file:tone.wav"};
        audio.rate = 48000;
        audio.channels = 1;
        audio.frames = 96000;
        audio.duration = at(2);
        document::AssetRecord video;
        video.id = kVideoAsset;
        video.kind = document::AssetKind::Video;
        video.locator = {"file", "project-relative", "clip.mov", "file:clip.mov"};
        video.width = video.height = 4;
        video.rate = 48000;
        video.channels = 1;
        video.frames = 16; // 16 picture frames at 8 fps: a 2 s extent, last frame start 15/8
        video.duration = at(2);
        document::AssetVideoStream picture;
        picture.codec = "raw";
        picture.timebase = picture.framePeriod = at(1, kRate);
        picture.duration = at(2);
        video.videoStreams.push_back(picture);
        document::AssetVideoStream sound;
        sound.id = 1;
        sound.kind = 2;
        sound.codec = "pcm";
        sound.timebase = sound.framePeriod = at(1, 48000);
        sound.duration = at(2);
        sound.sampleRate = 48000;
        sound.channelLayout = {"M"};
        video.videoStreams.push_back(sound);
        if (!project.project.addAsset(audio) || !project.project.addAsset(video))
            throw std::runtime_error("fixture assets");
        return project;
    }
    template <typename Operation, typename... Arguments> void run(Arguments&&... arguments) {
        commands::Transaction transaction("Audio timing", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        const auto result = commands.execute(std::move(transaction));
        if (!result.changed() && result.status != commands::CommandStatus::NoChange) {
            for (const auto& failure : result.operationFailures)
                std::cerr << failure.operationType << ": " << failure.issue.message << '\n';
            throw std::runtime_error("fixture command refused");
        }
    }
    document::LayerId addLayer(const document::AssetId asset) {
        if (asset == kVideoAsset)
            run<commands::AddImageLayer>(composition, asset);
        else
            run<commands::AddAudioLayer>(composition, asset);
        return graph().layerOutputs().back().layerId;
    }
    [[nodiscard]] const document::CanonicalGraph& graph() const {
        return document.snapshot().project().findComposition(composition)->graph();
    }
    void setVideoParameter(const std::string_view role, const std::int64_t value) {
        for (const auto& node : graph().nodes()) {
            if (node.typeId != "bloom.video-source")
                continue;
            for (const auto& binding : node.parameters)
                if (binding.role == role) {
                    run<commands::SetParameterSource>(composition, binding.parameterId,
                                                      document::ConstantValueSource{value});
                    return;
                }
        }
        throw std::runtime_error("video source parameter");
    }
    [[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan> plan() const {
        const auto compiled = runtime::SnapshotCompiler(document::builtInNodeDefinitions())
                                  .compile({document.snapshot(), composition}, {});
        if (!compiled.plan)
            throw std::runtime_error("fixture compile");
        return compiled.plan;
    }
    [[nodiscard]] runtime::AudioMixDescription mix(const core::RationalTime time) const {
        auto mix = runtime::CpuCompositionEvaluator{}.evaluateAudioMix(plan(), time);
        if (!mix)
            throw std::runtime_error("fixture audio mix");
        return *mix;
    }
};

// The production clip construction (export and playback build clips identically).
std::vector<media::audio::playback::AudioClip>
clipsFor(const runtime::AudioMixDescription& mix,
         const std::shared_ptr<const media::audio::AudioBuffer>& buffer) {
    std::vector<media::audio::playback::AudioClip> clips;
    for (const auto& description : mix.clips) {
        clips.push_back({.buffer = buffer,
                         .startTime = description.startTime,
                         .level = static_cast<float>(description.level),
                         .muted = description.muted,
                         .solo = description.solo,
                         .endTime = description.endTime,
                         .mapTime = {}});
        if (!description.timeMappings.empty())
            clips.back().mapTime = [description](const core::RationalTime time) {
                return runtime::mapAudioClipTime(description, time);
            };
    }
    return clips;
}

// Playback: one mix, continuous transport from `start`.
std::vector<float> playback(const Fixture& fixture, const std::int64_t start,
                            const std::size_t count) {
    auto backend = std::make_unique<media::audio::playback::NullBackend>();
    auto* sink = backend.get();
    media::audio::playback::AudioEngine engine(std::move(backend), {kRate, 1, 1024});
    engine.replaceClips(clipsFor(fixture.mix(at(start, kRate)), ramp()));
    if (engine.play(at(start, kRate)) || engine.renderForTesting(count))
        throw std::runtime_error("playback render");
    return {sink->captured().begin(), sink->captured().end()};
}

// Export: the offline stream re-evaluates the mix at every block start and keeps one transport.
std::vector<float> exported(const Fixture& fixture, const std::int64_t start,
                            const std::size_t count, const std::size_t block) {
    auto backend = std::make_unique<media::audio::playback::NullBackend>();
    auto* sink = backend.get();
    media::audio::playback::AudioEngine engine(std::move(backend), {kRate, 1, 1024});
    if (engine.play(at(start, kRate)))
        throw std::runtime_error("export start");
    const auto buffer = ramp();
    std::vector<float> samples;
    for (std::size_t done = 0; done < count; done += block) {
        const auto frames = std::min(block, count - done);
        engine.replaceClips(clipsFor(
            fixture.mix(at(start + static_cast<std::int64_t>(done), kRate)), buffer));
        sink->clearCaptured();
        if (engine.renderForTesting(frames))
            throw std::runtime_error("export render");
        samples.insert(samples.end(), sink->captured().begin(), sink->captured().end());
    }
    return samples;
}

// Expected output for sample n given the source sample it plays (nullopt = silence).
template <typename Source>
std::vector<float> expected(const std::int64_t start, const std::size_t count, Source source) {
    std::vector<float> values;
    for (std::size_t index = 0; index < count; ++index) {
        const auto sample = source(start + static_cast<std::int64_t>(index));
        values.push_back(sample ? static_cast<float>(*sample + 1) : 0.0F);
    }
    return values;
}

void check(const Fixture& fixture, const std::vector<float>& want, const std::string& label,
           const std::int64_t start = 0) {
    const auto played = playback(fixture, start, want.size());
    expect(played == want, label + ": playback is sample-exact");
    for (const std::size_t block : {std::size_t{3}, std::size_t{7}, std::size_t{512}})
        expect(exported(fixture, start, want.size(), block) == played,
               label + ": export (block " + std::to_string(block) + ") equals playback");
}

std::optional<std::int64_t> pingPong(const std::int64_t local) {
    // 16 frames reflect about the last frame start (15) with period 30.
    const auto phase = local % 30;
    return phase <= 15 ? phase : 30 - phase;
}

// An identity-timing (Unbounded, start 0) layer with a trimmed head is silent before its in point
// and after its out point.
void headTrimIdentity() {
    Fixture fixture;
    const auto layer = fixture.addLayer(kAudioAsset);
    fixture.run<commands::SetLayerContentExtent>(fixture.composition, layer,
                                                 document::LayerContentExtent{});
    fixture.run<commands::SetLayerTiming>(fixture.composition, layer, at(0), at(1, 2), at(3, 2));
    check(fixture,
          expected(0, 20,
                   [](const std::int64_t n) {
                       return n >= 4 && n < 12 ? std::optional{n} : std::nullopt;
                   }),
          "identity head trim");
}

// A moved layer with a trimmed head plays content time t - start inside [in, out).
void headTrimRetimed() {
    Fixture fixture;
    const auto layer = fixture.addLayer(kAudioAsset);
    fixture.run<commands::SetLayerTiming>(fixture.composition, layer, at(1), at(3, 2), at(5, 2));
    check(fixture,
          expected(0, 28,
                   [](const std::int64_t n) {
                       return n >= 12 && n < 20 ? std::optional{n - 8} : std::nullopt;
                   }),
          "retimed head trim");
}

// Speed is varispeed: twice the rate reads every second source sample.
void layerSpeed() {
    Fixture fixture;
    const auto layer = fixture.addLayer(kAudioAsset);
    fixture.run<commands::SetLayerTiming>(fixture.composition, layer, at(0), at(0), at(1),
                                          *document::LayerSpeed::create(2, 1));
    check(fixture,
          expected(0, 10,
                   [](const std::int64_t n) {
                       return n < 8 ? std::optional{2 * n} : std::nullopt;
                   }),
          "layer speed");
}

// The layer's after modes wrap audio exactly as they wrap pictures.
void layerModes() {
    for (const auto mode : {document::LayerTimeMode::Loop, document::LayerTimeMode::PingPong}) {
        Fixture fixture;
        const auto layer = fixture.addLayer(kAudioAsset);
        fixture.run<commands::SetLayerTiming>(fixture.composition, layer, at(0), at(0), at(8));
        fixture.run<commands::SetLayerTimeModes>(fixture.composition, layer,
                                                 document::LayerTimeMode::None, mode);
        const bool loop = mode == document::LayerTimeMode::Loop;
        check(fixture,
              expected(0, 40,
                       [&](const std::int64_t n) {
                           return loop ? std::optional{n % 16} : pingPong(n);
                       }),
              loop ? "layer Loop" : "layer PingPong");
    }
}

// A video source's Loop and PingPong apply to its embedded audio over the picture extent; Hold
// plays the stream once. A source start frame delays the stream, silent before it.
void videoLoopModes() {
    for (const std::int64_t loopMode : {0, 1, 2}) {
        for (const std::int64_t startFrame : {0, 4}) {
            Fixture fixture;
            const auto layer = fixture.addLayer(kVideoAsset);
            // Unbounded extent, whole composition: only the source's own mode applies.
            fixture.run<commands::SetLayerContentExtent>(fixture.composition, layer,
                                                         document::LayerContentExtent{});
            fixture.run<commands::SetLayerTiming>(fixture.composition, layer, at(0), at(0),
                                                  at(8));
            fixture.setVideoParameter("loopMode", loopMode);
            fixture.setVideoParameter("startFrame", startFrame);
            check(fixture,
                  expected(0, 48,
                           [&](const std::int64_t n) -> std::optional<std::int64_t> {
                               const auto local = n - startFrame;
                               if (local < 0)
                                   return std::nullopt;
                               if (loopMode == 1)
                                   return local % 16;
                               if (loopMode == 2)
                                   return pingPong(local);
                               return local < 16 ? std::optional{local} : std::nullopt;
                           }),
                  "video loop mode " + std::to_string(loopMode) + " start " +
                      std::to_string(startFrame));
        }
    }
}

// #400: the transport loop wraps inside the mixer, so a retimed (speed + PingPong) layer replays
// exactly its remapped samples on every pass, from a start in the middle of the loop. The oracle
// is the unlooped playback, which the cases above pin sample-exactly.
void transportLoopRemapped() {
    Fixture fixture;
    const auto layer = fixture.addLayer(kAudioAsset);
    fixture.run<commands::SetLayerTiming>(fixture.composition, layer, at(1), at(1), at(8),
                                          *document::LayerSpeed::create(3, 2));
    fixture.run<commands::SetLayerTimeModes>(fixture.composition, layer,
                                             document::LayerTimeMode::Hold,
                                             document::LayerTimeMode::PingPong);
    const auto straight = playback(fixture, 0, 40);
    auto backend = std::make_unique<media::audio::playback::NullBackend>();
    auto* sink = backend.get();
    media::audio::playback::AudioEngine engine(std::move(backend), {kRate, 1, 1024});
    const auto clips = clipsFor(fixture.mix(at(20, kRate)), ramp());
    expect(clips.size() == 1 && clips.front().mapTime, "the looped clip is time-mapped");
    engine.replaceClips(clips);
    // [1.5 s, 4.25 s) is samples 12..33: 22 samples a pass.
    expect(engine.setLoopRange(media::audio::playback::LoopRange{at(3, 2), at(17, 4)}),
           "transport loop accepted");
    if (engine.play(at(20, kRate)) || engine.renderForTesting(22 * 3 + 14))
        throw std::runtime_error("looped playback render");
    std::vector<float> want;
    for (std::int64_t n = 20; n < 20 + 22 * 3 + 14; ++n)
        want.push_back(straight[static_cast<std::size_t>(n < 34 ? n : 12 + (n - 12) % 22)]);
    expect(std::vector<float>(sink->captured().begin(), sink->captured().end()) == want,
           "a retimed layer replays its exact remapped samples on every loop pass");
    expect(std::count_if(want.begin(), want.end(), [](float v) { return v != 0.0F; }) > 10,
           "the looped retimed layer is audible");
}

// The per-sample mapping allocates nothing: it runs on the mixer worker for every output sample.
void mappingAllocatesNothing() {
    Fixture fixture;
    const auto layer = fixture.addLayer(kVideoAsset);
    fixture.run<commands::SetLayerTiming>(fixture.composition, layer, at(1), at(1), at(8),
                                          *document::LayerSpeed::create(3, 2));
    fixture.run<commands::SetLayerTimeModes>(fixture.composition, layer,
                                             document::LayerTimeMode::Hold,
                                             document::LayerTimeMode::PingPong);
    fixture.run<commands::EnableLayerTimeRemap>(fixture.composition, layer);
    fixture.setVideoParameter("loopMode", 2);
    const auto mix = fixture.mix(at(0));
    const auto clips = clipsFor(mix, ramp());
    expect(clips.size() == 1 && clips.front().mapTime, "the retimed clip maps time");
    std::size_t mapped = 0;
    countAllocations.store(true);
    for (std::int64_t sample = 0; sample < 48000 * 8; sample += 7) {
        const auto time = core::RationalTime::create(sample, 48000);
        if (time && clips.front().mapTime(*time))
            ++mapped;
    }
    countAllocations.store(false);
    expect(mapped > 0, "the mapping produces audible samples");
    expect(allocations.load() == 0, "per-sample audio time mapping allocates nothing (" +
                                        std::to_string(allocations.load()) + " allocations)");
}
} // namespace

int main() {
    try {
        headTrimIdentity();
        headTrimRetimed();
        layerSpeed();
        layerModes();
        videoLoopModes();
        transportLoopRemapped();
        mappingAllocatesNothing();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "audio timing: sample-exact playback and export PASS\n";
    return failures == 0 ? 0 : 1;
}
