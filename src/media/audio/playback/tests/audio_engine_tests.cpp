#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using bloom::core::RationalTime;
using bloom::media::audio::AudioBuffer;
using bloom::media::audio::playback::AudioClip;
using bloom::media::audio::playback::AudioEngine;
using bloom::media::audio::playback::AudioStatus;
using bloom::media::audio::playback::Backend;
using bloom::media::audio::playback::LoopRange;
using bloom::media::audio::playback::NullBackend;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] RationalTime time(const std::int64_t numerator, const std::int64_t denominator = 1) {
    const auto result = RationalTime::create(numerator, denominator);
    if (!result.has_value()) {
        std::abort();
    }
    return *result;
}

// Engine tests build clips through this small std::make_shared helper rather than deep-copying
// an AudioBuffer per clip, matching how AssetController hands out shared decoded buffers.
[[nodiscard]] std::shared_ptr<const AudioBuffer> stereoBuffer(const std::vector<float>& left,
                                                              const std::vector<float>& right,
                                                              const std::uint32_t rate = 8) {
    return std::make_shared<const AudioBuffer>(AudioBuffer{rate, 2, left.size(), {left, right}});
}

[[nodiscard]] std::shared_ptr<const AudioBuffer> monoBuffer(const std::vector<float>& samples,
                                                            const std::uint32_t rate = 8) {
    return std::make_shared<const AudioBuffer>(AudioBuffer{rate, 1, samples.size(), {samples}});
}

void expectSamples(Expectations& expectations, const std::span<const float> actual,
                   const std::vector<float>& expected, const std::string_view message) {
    expectations.expect(actual.size() == expected.size(), message);
    if (actual.size() != expected.size()) {
        return;
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        expectations.expect(actual[index] == expected[index], message);
    }
}

void testSampleAccurateMixing(Expectations& expectations) {
    auto backend = std::make_unique<NullBackend>();
    auto* capturedBackend = backend.get();
    AudioEngine engine(std::move(backend), AudioEngine::Config{8, 2, 32});
    const auto first = engine.addClip(AudioClip{
        stereoBuffer({1, 2, 3, 4, 5, 6}, {10, 20, 30, 40, 50, 60}), time(0), 1.0F, false, false});
    const auto second = engine.addClip(
        AudioClip{monoBuffer({0.5F, 1.0F, 1.5F, 2.0F, 2.5F}), time(2, 8), 2.0F, false, false});
    expectations.expect(first == 0 && second == 1, "clips receive stable insertion identifiers");

    expectations.expect(!engine.play(time(0)).has_value(), "null backend starts playback");
    expectations.expect(!engine.renderForTesting(6).has_value(),
                        "null backend pulls exactly the requested frames");
    expectSamples(expectations, capturedBackend->captured(),
                  {1, 10, 2, 20, 4, 31, 6, 42, 8, 53, 10, 64},
                  "mixing preserves sample order, level, and mono-to-stereo routing");
    expectations.expect(engine.positionNow() == time(3, 4),
                        "audio clock is exactly six written frames at 8 Hz");
}

void testSoloMuteAndSeek(Expectations& expectations) {
    auto backend = std::make_unique<NullBackend>();
    auto* capturedBackend = backend.get();
    AudioEngine engine(std::move(backend), AudioEngine::Config{8, 2, 32});
    const auto first = engine.addClip(AudioClip{
        stereoBuffer({1, 1, 1, 1, 1, 1}, {1, 1, 1, 1, 1, 1}), time(0), 1.0F, false, false});
    const auto second = engine.addClip(AudioClip{
        stereoBuffer({2, 2, 2, 2, 2, 2}, {2, 2, 2, 2, 2, 2}), time(0), 1.0F, false, false});

    engine.play(time(0));
    engine.renderForTesting(1);
    expectSamples(expectations, capturedBackend->captured(), {3, 3},
                  "unqualified clips mix together");

    capturedBackend->clearCaptured();
    expectations.expect(engine.updateClip(first, 1.0F, true, false), "mute updates a clip");
    engine.seek(time(0));
    engine.renderForTesting(1);
    expectSamples(expectations, capturedBackend->captured(), {2, 2},
                  "muted clips contribute no samples");

    capturedBackend->clearCaptured();
    expectations.expect(engine.updateClip(first, 1.0F, false, false), "unmute updates a clip");
    expectations.expect(engine.updateClip(second, 1.0F, false, true), "solo updates a clip");
    engine.seek(time(0));
    engine.renderForTesting(1);
    expectSamples(expectations, capturedBackend->captured(), {2, 2},
                  "an active solo clip excludes non-solo clips");

    capturedBackend->clearCaptured();
    engine.seek(time(4, 8));
    engine.renderForTesting(2);
    expectSamples(expectations, capturedBackend->captured(), {2, 2, 2, 2},
                  "seek restarts the ring from the requested rational source time");
    expectations.expect(engine.positionNow() == time(3, 4),
                        "seeked audio clock advances from its new origin");
}

void testRateAndClockMonotonicity(Expectations& expectations) {
    auto backend = std::make_unique<NullBackend>();
    auto* capturedBackend = backend.get();
    AudioEngine engine(std::move(backend), AudioEngine::Config{8, 1, 32});
    const auto clip = engine.addClip(
        AudioClip{monoBuffer({0, 1, 2, 3, 4, 5, 6, 7}), time(0), 1.0F, false, false});
    expectations.expect(clip == 0, "one clip receives the first insertion identifier");
    expectations.expect(engine.setRate(2.0), "transport accepts a finite positive rate");
    engine.play(time(0));
    engine.renderForTesting(2);
    expectSamples(expectations, capturedBackend->captured(), {0, 2},
                  "rate two advances source samples at twice transport speed");
    expectations.expect(engine.positionNow() == time(1, 2),
                        "audio clock follows written frames and playback rate");
    const auto before = engine.positionNow();
    engine.renderForTesting(1);
    expectations.expect(engine.positionNow() > before,
                        "audio clock is monotonic across callback pulls");
    expectations.expect(!engine.setRate(0.0), "zero transport rate is rejected");
}

void testPlaySeekStopKeepsFrameAndAudioClock(Expectations& expectations) {
    auto backend = std::make_unique<NullBackend>();
    AudioEngine engine(std::move(backend), AudioEngine::Config{24, 1, 64});
    static_cast<void>(engine.addClip(
        AudioClip{monoBuffer(std::vector<float>(48, 1.0F), 24), time(0), 1.0F, false, false}));
    const auto mappingResult = bloom::core::FrameTimeMapping::create(time(2), 24, 1);
    expectations.expect(mappingResult.hasValue(), "frame mapping is available for the audio check");
    if (!mappingResult.hasValue())
        return;
    const auto& mapping = *mappingResult.value();
    expectations.expect(!engine.play(time(0)).has_value(), "play starts the headless transport");
    expectations.expect(!engine.renderForTesting(5).has_value(), "play renders five frames");
    const auto playedFrame = mapping.nearestFrameIndex(engine.positionNow());
    expectations.expect(playedFrame == 5, "audio clock maps played frames exactly");

    expectations.expect(!engine.seek(time(10, 24)).has_value(), "seek restarts the headless clock");
    expectations.expect(!engine.renderForTesting(2).has_value(), "seek renders two frames");
    const auto seekedPosition = engine.positionNow();
    const auto seekedFrame = mapping.nearestFrameIndex(seekedPosition);
    expectations.expect(seekedFrame == 12, "seeked audio clock maps to the requested frame");

    engine.stop();
    const auto stoppedPosition = engine.positionNow();
    const auto stoppedFrame = mapping.nearestFrameIndex(stoppedPosition);
    expectations.expect(stoppedFrame == seekedFrame && stoppedPosition == seekedPosition,
                        "stop freezes the frame index and audio clock within one frame");
}

void testReplaceClipsSharesBufferWithoutCopy(Expectations& expectations) {
    auto backend = std::make_unique<NullBackend>();
    auto* capturedBackend = backend.get();
    AudioEngine engine(std::move(backend), AudioEngine::Config{8, 1, 32});
    const auto buffer = monoBuffer({1, 2, 3, 4, 5, 6, 7, 8});
    const auto* planesData = buffer->planes.front().data();

    std::vector<AudioClip> clips;
    clips.push_back(AudioClip{buffer, time(0), 1.0F, false, false});
    engine.replaceClips(std::move(clips));

    expectations.expect(buffer.use_count() >= 2,
                        "replaceClips retains the shared buffer instead of deep-copying it");
    expectations.expect(buffer->planes.front().data() == planesData,
                        "the shared buffer's planes are never copied");

    expectations.expect(!engine.play(time(0)).has_value(), "playback starts with the shared clip");
    expectations.expect(!engine.renderForTesting(8).has_value(),
                        "engine renders through the shared clip");
    expectSamples(expectations, capturedBackend->captured(), {1, 2, 3, 4, 5, 6, 7, 8},
                  "the compiled mix still renders non-silent samples from the shared buffer");
}

// Source sample i reads i + 1, so silence (0) is distinguishable from the first sample.
[[nodiscard]] std::vector<float> ramp(const std::size_t frames) {
    std::vector<float> samples(frames);
    for (std::size_t index = 0; index < frames; ++index)
        samples[index] = static_cast<float>(index + 1);
    return samples;
}

// Expected output when output frame n plays source frame source(n) (negative: silence).
template <typename Source>
[[nodiscard]] std::vector<float> expectedRamp(const std::int64_t first, const std::size_t count,
                                              Source source) {
    std::vector<float> values;
    for (std::size_t index = 0; index < count; ++index) {
        const auto frame = source(first + static_cast<std::int64_t>(index));
        values.push_back(frame < 0 ? 0.0F : static_cast<float>(frame + 1));
    }
    return values;
}

// #400: a looped run wraps inside the mixer on every pass, sample-exactly and with no gap, from a
// start inside the loop and across render pulls that straddle the wrap.
void testLoopWrapsEveryPassSampleExactly(Expectations& expectations) {
    auto backend = std::make_unique<NullBackend>();
    auto* captured = backend.get();
    AudioEngine engine(std::move(backend), AudioEngine::Config{8, 1, 64});
    static_cast<void>(engine.addClip(AudioClip{monoBuffer(ramp(16)), time(0), 1.0F, false, false}));
    // [0.5 s, 1.5 s) at 8 Hz is frames 4..11.
    expectations.expect(engine.setLoopRange(LoopRange{time(1, 2), time(3, 2)}),
                        "a positive loop range is accepted");
    expectations.expect(!engine.play(time(1, 4)).has_value(), "a looped run starts mid-loop");
    constexpr std::size_t kPasses = 5;
    for (std::size_t pulled = 0; pulled < 2 + 8 * kPasses; pulled += 5)
        static_cast<void>(engine.renderForTesting(5));
    const auto loop = [](const std::int64_t frame) {
        return frame < 12 ? frame : 4 + (frame - 4) % 8;
    };
    const auto played = captured->captured();
    expectSamples(expectations, played.first(2 + 8 * kPasses),
                  expectedRamp(2, 2 + 8 * kPasses, loop),
                  "every loop pass replays frames 4..11 with no gap at the wrap");
    for (std::size_t pass = 0; pass < kPasses; ++pass)
        expectations.expect(played[2 + pass * 8] == 5.0F && played[1 + pass * 8 + 8] == 12.0F,
                            "each pass starts on the loop start and ends on its last frame");
    expectations.expect(engine.positionNow() == time(2 + 45, 8),
                        "the reported clock stays unwrapped and monotonic across wraps");
}

// A loop that is not a whole number of samples long alternates pass lengths so the wrap never
// drifts from the rational loop period: 10 passes of 2.5 frames end exactly on frame 25.
void testFractionalLoopNeverDrifts(Expectations& expectations) {
    auto backend = std::make_unique<NullBackend>();
    auto* captured = backend.get();
    AudioEngine engine(std::move(backend), AudioEngine::Config{10, 1, 512});
    static_cast<void>(
        engine.addClip(AudioClip{monoBuffer(ramp(10), 10), time(0), 1.0F, false, false}));
    expectations.expect(engine.setLoopRange(LoopRange{time(0), time(1, 4)}),
                        "a fractional-sample loop is accepted");
    static_cast<void>(engine.play(time(0)));
    expectations.expect(!engine.renderForTesting(250).has_value(), "250 looped frames render");
    if (captured->captured().size() != 250)
        return;
    const auto expected = expectedRamp(0, 250, [](const std::int64_t frame) {
        // Exact: floor(((frame * 4) mod 10) / 4) once past the first pass.
        return frame * 4 < 10 ? frame : ((frame * 4) % 10) / 4;
    });
    expectSamples(expectations, captured->captured(), expected,
                  "a fractional loop wraps on the exact rational period");
    for (std::size_t pass = 0; pass < 10; ++pass)
        expectations.expect(captured->captured()[pass * 25] == 1.0F,
                            "every 2.5 s the wrap lands on the loop start again, without drift");
}

// Clips that end before the loop end, muted and solo clips, and a remapped clip all evaluate at
// the wrapped time on every pass.
void testLoopedClipsEvaluateAtWrappedTime(Expectations& expectations) {
    auto backend = std::make_unique<NullBackend>();
    auto* captured = backend.get();
    AudioEngine engine(std::move(backend), AudioEngine::Config{8, 1, 64});
    // Ends at 0.75 s: frames 4 and 5 sound, 6..11 are silent, on every pass.
    auto shortClip = AudioClip{monoBuffer(ramp(16)), time(0), 1.0F, false, false};
    shortClip.endTime = time(3, 4);
    static_cast<void>(engine.addClip(shortClip));
    // Muted: never contributes, on any pass.
    static_cast<void>(engine.addClip(
        AudioClip{monoBuffer(std::vector<float>(16, 100.0F)), time(0), 1.0F, true, false}));
    // Remapped: plays source time t - 0.5 s (a one-pass-late copy), scaled by 1000.
    auto remapped = AudioClip{monoBuffer(ramp(16)), time(0), 1000.0F, false, false};
    remapped.mapTime = [](const RationalTime at) -> std::optional<RationalTime> {
        if (at < time(1, 2))
            return std::nullopt;
        return RationalTime::create(at.numerator() * 2 - at.denominator(), at.denominator() * 2);
    };
    static_cast<void>(engine.addClip(std::move(remapped)));
    expectations.expect(engine.setLoopRange(LoopRange{time(1, 2), time(3, 2)}), "loop accepted");
    static_cast<void>(engine.play(time(1, 2)));
    static_cast<void>(engine.renderForTesting(8 * 4));
    std::vector<float> pass;
    for (std::int64_t frame = 4; frame < 12; ++frame) {
        const float shortSample = frame < 6 ? static_cast<float>(frame + 1) : 0.0F;
        pass.push_back(shortSample + 1000.0F * static_cast<float>(frame - 4 + 1));
    }
    std::vector<float> expected;
    for (int repeat = 0; repeat < 4; ++repeat)
        expected.insert(expected.end(), pass.begin(), pass.end());
    expectSamples(expectations, captured->captured(), expected,
                  "end-trimmed, muted and remapped clips repeat identically on every pass");

    captured->clearCaptured();
    // Solo the short clip: only it sounds, and still on every pass.
    expectations.expect(engine.updateClip(0, 1.0F, false, true), "solo updates a clip");
    static_cast<void>(engine.seek(time(1, 2)));
    static_cast<void>(engine.renderForTesting(8 * 3));
    expectSamples(expectations, captured->captured(),
                  expectedRamp(4, 24,
                               [](const std::int64_t frame) {
                                   const auto wrapped = 4 + (frame - 4) % 8;
                                   return wrapped < 6 ? wrapped : std::int64_t{-1};
                               }),
                  "a solo clip alone sounds on every pass");
}

void testLoopRangeValidationAndClearing(Expectations& expectations) {
    auto backend = std::make_unique<NullBackend>();
    auto* captured = backend.get();
    AudioEngine engine(std::move(backend), AudioEngine::Config{8, 1, 64});
    static_cast<void>(engine.addClip(AudioClip{monoBuffer(ramp(16)), time(0), 1.0F, false, false}));
    expectations.expect(!engine.setLoopRange(LoopRange{time(1), time(1)}),
                        "an empty loop is refused");
    expectations.expect(!engine.setLoopRange(LoopRange{time(1), time(1, 2)}),
                        "a reversed loop is refused");
    expectations.expect(!engine.setLoopRange(LoopRange{time(-1), time(1)}),
                        "a loop before zero is refused");
    expectations.expect(!engine.loopRange().has_value(), "a refused loop changes nothing");
    expectations.expect(engine.setLoopRange(std::nullopt), "no loop is always accepted");
    static_cast<void>(engine.play(time(1)));
    static_cast<void>(engine.renderForTesting(12));
    expectSamples(
        expectations, captured->captured(),
        expectedRamp(
            8, 12, [](const std::int64_t frame) { return frame < 16 ? frame : std::int64_t{-1}; }),
        "with looping off a run plays past the end into silence, unchanged");
}

// Underrun frames already passed on the device clock; the mix skips them instead of lagging the
// clock (and every looped wrap after it) for the rest of the run.
void testUnderrunKeepsMixOnDeviceClock(Expectations& expectations) {
    auto backend = std::make_unique<NullBackend>();
    auto* captured = backend.get();
    AudioEngine engine(std::move(backend), AudioEngine::Config{8, 1, 64});
    static_cast<void>(engine.addClip(AudioClip{monoBuffer(ramp(16)), time(0), 1.0F, false, false}));
    static_cast<void>(engine.play(time(0)));
    captured->render(3);
    static_cast<void>(engine.renderForTesting(4));
    expectSamples(expectations, captured->captured(), {0, 0, 0, 4, 5, 6, 7},
                  "after a three-frame underrun the mix resumes at the device position");
    expectations.expect(engine.positionNow() == time(7, 8), "the clock counts the underrun");
}

// A device that pulls only when the test asks, so the engine runs its real mixer thread.
class ManualBackend final : public Backend {
  public:
    AudioStatus start(const std::uint32_t, const std::uint32_t channels, const Callback callback,
                      void* userData) noexcept override {
        const std::scoped_lock lock(mutex_);
        channels_ = channels;
        callback_ = callback;
        userData_ = userData;
        return std::nullopt;
    }
    void stop() noexcept override {
        const std::scoped_lock lock(mutex_);
        callback_ = nullptr;
    }
    std::vector<float> pull(const std::size_t frames) {
        const std::scoped_lock lock(mutex_);
        std::vector<float> output(frames * channels_, 0.0F);
        if (callback_ != nullptr)
            callback_(userData_, output);
        return output;
    }

  private:
    std::mutex mutex_;
    Callback callback_ = nullptr;
    void* userData_ = nullptr;
    std::uint32_t channels_ = 1;
};

[[nodiscard]] bool waitForQueued(const AudioEngine& engine, const std::size_t frames) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (engine.queuedFramesForTesting() < frames) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Changing the loop while the threaded mixer has already queued audio past the change re-mixes
// exactly the unheard frames: no device restart, no gap, no repeated or skipped sample.
void testMidPlayLoopChangeRewindsQueuedAudio(Expectations& expectations) {
    auto backendOwner = std::make_unique<ManualBackend>();
    auto* device = backendOwner.get();
    AudioEngine engine(std::move(backendOwner), AudioEngine::Config{8, 1, 64});
    static_cast<void>(engine.addClip(AudioClip{monoBuffer(ramp(64)), time(0), 1.0F, false, false}));
    expectations.expect(!engine.play(time(0)).has_value(), "threaded mixer starts");
    std::vector<float> heard;
    const auto pull = [&](const std::size_t frames) {
        expectations.expect(waitForQueued(engine, frames),
                            "mixer queues audio ahead of the device");
        const auto output = device->pull(frames);
        heard.insert(heard.end(), output.begin(), output.end());
    };
    pull(4);
    expectations.expect(waitForQueued(engine, 64), "the ring fills past the coming loop end");
    // Toggled on with frames 4..63 already mixed unlooped.
    expectations.expect(engine.setLoopRange(LoopRange{time(0), time(1)}), "loop toggled on");
    pull(4);  // Already handed to the device: plays as mixed, then the rest is re-mixed.
    pull(16); // Two whole passes of the new loop.
    pull(3);
    // Toggled off three frames into the fourth pass: the run keeps that pass.
    expectations.expect(engine.setLoopRange(std::nullopt), "loop toggled off");
    pull(2);
    pull(6);
    engine.stop();
    std::vector<float> expected;
    for (std::int64_t frame = 0; frame < 35; ++frame) {
        const auto source = frame < 24 ? frame % 8 : frame - 24;
        expected.push_back(static_cast<float>(source + 1));
    }
    expectSamples(expectations, heard, expected,
                  "mid-play loop changes re-mix queued audio gaplessly from the device position");
}

} // namespace

int main() {
    Expectations expectations;
    testSampleAccurateMixing(expectations);
    testSoloMuteAndSeek(expectations);
    testRateAndClockMonotonicity(expectations);
    testPlaySeekStopKeepsFrameAndAudioClock(expectations);
    testReplaceClipsSharesBufferWithoutCopy(expectations);
    testLoopWrapsEveryPassSampleExactly(expectations);
    testFractionalLoopNeverDrifts(expectations);
    testLoopedClipsEvaluateAtWrappedTime(expectations);
    testLoopRangeValidationAndClearing(expectations);
    testUnderrunKeepsMixOnDeviceClock(expectations);
    testMidPlayLoopChangeRewindsQueuedAudio(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
