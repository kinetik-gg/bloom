#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using bloom::core::RationalTime;
using bloom::media::audio::AudioBuffer;
using bloom::media::audio::playback::AudioClip;
using bloom::media::audio::playback::AudioEngine;
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

[[nodiscard]] AudioBuffer stereoBuffer(const std::vector<float>& left,
                                       const std::vector<float>& right,
                                       const std::uint32_t rate = 8) {
    return AudioBuffer{rate, 2, left.size(), {left, right}};
}

[[nodiscard]] AudioBuffer monoBuffer(const std::vector<float>& samples,
                                     const std::uint32_t rate = 8) {
    return AudioBuffer{rate, 1, samples.size(), {samples}};
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

} // namespace

int main() {
    Expectations expectations;
    testSampleAccurateMixing(expectations);
    testSoloMuteAndSeek(expectations);
    testRateAndClockMonotonicity(expectations);
    testPlaySeekStopKeepsFrameAndAudioClock(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
