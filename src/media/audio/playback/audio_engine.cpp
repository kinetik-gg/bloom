#include <bloom/media/audio/playback/audio_engine.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <numeric>
#include <utility>

namespace {

using bloom::core::RationalTime;
using bloom::media::audio::AudioError;
using bloom::media::audio::AudioErrorCode;
using bloom::media::audio::playback::AudioClip;
using bloom::media::audio::playback::AudioEngine;
using bloom::media::audio::playback::AudioStatus;

[[nodiscard]] std::int64_t timeToFrames(const RationalTime time,
                                        const std::uint32_t rate) noexcept {
    const auto value = static_cast<long double>(time.numerator()) * static_cast<long double>(rate) /
                       static_cast<long double>(time.denominator());
    if (value <= static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    if (value >= static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(std::floor(value));
}

[[nodiscard]] std::int64_t saturatingFrameOffset(const std::uint64_t produced,
                                                 const double rate) noexcept {
    const auto value = static_cast<long double>(produced) * static_cast<long double>(rate);
    if (value >= static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(std::floor(value));
}

[[nodiscard]] std::size_t sourceFrameFor(const std::int64_t localOutputFrame,
                                         const std::uint32_t sourceRate,
                                         const std::uint32_t outputRate) noexcept {
    const auto value = static_cast<long double>(localOutputFrame) *
                       static_cast<long double>(sourceRate) / static_cast<long double>(outputRate);
    if (value < 0.0L ||
        value >= static_cast<long double>(std::numeric_limits<std::size_t>::max())) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(std::floor(value));
}

[[nodiscard]] std::optional<std::size_t> clipSampleFrame(const AudioClip& clip,
                                                         std::int64_t absoluteFrame,
                                                         const std::uint32_t outputRate) noexcept {
    if (clip.muted || clip.buffer == nullptr || clip.buffer->rate == 0 ||
        clip.buffer->channels == 0 || clip.buffer->planes.size() != clip.buffer->channels) {
        return std::nullopt;
    }
    if (clip.mapTime) {
        const auto time = RationalTime::create(absoluteFrame, outputRate);
        if (!time)
            return std::nullopt;
        const auto mapped = clip.mapTime(*time);
        if (!mapped)
            return std::nullopt;
        absoluteFrame = timeToFrames(*mapped, outputRate);
    }
    const auto startFrame = timeToFrames(clip.startTime, outputRate);
    if (absoluteFrame < startFrame) {
        return std::nullopt;
    }
    if (clip.endTime.has_value() && absoluteFrame >= timeToFrames(*clip.endTime, outputRate)) {
        return std::nullopt;
    }
    const auto localFrame = absoluteFrame - startFrame;
    if (localFrame < 0) {
        return std::nullopt;
    }
    const auto sourceFrame = sourceFrameFor(localFrame, clip.buffer->rate, outputRate);
    if (sourceFrame >= clip.buffer->frames || sourceFrame >= clip.buffer->planes.front().size())
        return std::nullopt;
    return sourceFrame;
}

[[nodiscard]] AudioStatus errorStatus(const AudioErrorCode code) noexcept {
    return AudioStatus{AudioError::codeOnly(code)};
}

} // namespace

namespace bloom::media::audio::playback {

AudioStatus NullBackend::start(const std::uint32_t rate, const std::uint32_t channels,
                               const Callback callback, void* userData) noexcept {
    if (rate == 0 || channels == 0 || callback == nullptr) {
        return errorStatus(AudioErrorCode::InvalidAudioFormat);
    }
    rate_ = rate;
    channels_ = channels;
    callback_ = callback;
    userData_ = userData;
    started_ = true;
    return std::nullopt;
}

void NullBackend::stop() noexcept {
    started_ = false;
    callback_ = nullptr;
    userData_ = nullptr;
}

void NullBackend::render(const std::size_t frames) {
    if (!started_ || channels_ == 0 || frames == 0) {
        return;
    }
    auto output = std::vector<float>(frames * channels_, 0.0F);
    if (callback_ != nullptr) {
        callback_(userData_, output);
    }
    captured_.insert(captured_.end(), output.begin(), output.end());
}

void NullBackend::clearCaptured() noexcept { captured_.clear(); }

class AudioEngine::SpscRing final {
  public:
    SpscRing(const std::size_t frameCapacity, const std::uint32_t channels)
        : frameCapacity_(std::max<std::size_t>(frameCapacity, 1U)),
          channels_(std::max<std::uint32_t>(channels, 1U)), samples_(frameCapacity_ * channels_) {}

    [[nodiscard]] std::size_t freeFrames() const noexcept {
        const auto write = writeFrame_.load(std::memory_order_relaxed);
        const auto read = readFrame_.load(std::memory_order_acquire);
        return frameCapacity_ - (write - read);
    }

    [[nodiscard]] bool push(const std::span<const float> input) noexcept {
        const auto frames = input.size() / channels_;
        if (frames == 0 || input.size() % channels_ != 0 || frames > freeFrames()) {
            return false;
        }
        const auto write = writeFrame_.load(std::memory_order_relaxed);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto ringFrame = (write + frame) % frameCapacity_;
            std::copy_n(input.data() + frame * channels_, channels_,
                        samples_.data() + ringFrame * channels_);
        }
        writeFrame_.store(write + frames, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t pop(const std::span<float> output) noexcept {
        const auto requestedFrames = output.size() / channels_;
        if (requestedFrames == 0 || output.size() % channels_ != 0) {
            return 0;
        }
        const auto read = readFrame_.load(std::memory_order_relaxed);
        const auto write = writeFrame_.load(std::memory_order_acquire);
        const auto frames = std::min(requestedFrames, write - read);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto ringFrame = (read + frame) % frameCapacity_;
            std::copy_n(samples_.data() + ringFrame * channels_, channels_,
                        output.data() + frame * channels_);
        }
        readFrame_.store(read + frames, std::memory_order_release);
        return frames;
    }

    void clear() noexcept {
        const auto write = writeFrame_.load(std::memory_order_acquire);
        readFrame_.store(write, std::memory_order_release);
    }

  private:
    const std::size_t frameCapacity_;
    const std::uint32_t channels_;
    std::vector<float> samples_;
    std::atomic<std::size_t> writeFrame_{0};
    std::atomic<std::size_t> readFrame_{0};
};

AudioEngine::AudioEngine(std::unique_ptr<Backend> backend)
    : AudioEngine(std::move(backend), Config{}) {}

AudioEngine::AudioEngine(std::unique_ptr<Backend> backend, const Config config)
    : backend_(std::move(backend)), config_(config),
      ring_(std::make_unique<SpscRing>(config.ringBufferFrames, config.channels)),
      mixScratch_(kMixerBlockFrames * config.channels) {}

AudioEngine::~AudioEngine() { stop(); }

AudioEngine::ClipId AudioEngine::addClip(AudioClip clip) {
    std::scoped_lock lock(controlMutex_);
    clips_.push_back(std::move(clip));
    return clips_.size() - 1U;
}

void AudioEngine::replaceClips(std::vector<AudioClip> clips) {
    std::scoped_lock lock(controlMutex_);
    clips_ = std::move(clips);
}

void AudioEngine::clearClips() noexcept {
    std::scoped_lock lock(controlMutex_);
    clips_.clear();
}

bool AudioEngine::removeClip(const ClipId id) {
    std::scoped_lock lock(controlMutex_);
    if (id >= clips_.size()) {
        return false;
    }
    clips_.erase(clips_.begin() + static_cast<std::ptrdiff_t>(id));
    return true;
}

bool AudioEngine::updateClip(const ClipId id, const float level, const bool muted,
                             const bool solo) noexcept {
    if (!std::isfinite(level)) {
        return false;
    }
    std::scoped_lock lock(controlMutex_);
    if (id >= clips_.size()) {
        return false;
    }
    clips_[id].level = level;
    clips_[id].muted = muted;
    clips_[id].solo = solo;
    return true;
}

AudioStatus AudioEngine::play(const RationalTime at) {
    stop();
    if (!backend_ || config_.rate == 0 || config_.channels == 0 || config_.ringBufferFrames == 0) {
        return errorStatus(AudioErrorCode::InvalidAudioFormat);
    }
    {
        std::scoped_lock lock(controlMutex_);
        clockOrigin_ = at;
        framesProduced_ = 0;
        framesWritten_.store(0, std::memory_order_release);
        ring_->clear();
    }
    playing_.store(true, std::memory_order_release);
    const auto status = backend_->start(config_.rate, config_.channels, &backendCallback, this);
    if (status.has_value()) {
        playing_.store(false, std::memory_order_release);
        return status;
    }
    if (dynamic_cast<NullBackend*>(backend_.get()) == nullptr) {
        mixerThread_ = std::thread(&AudioEngine::mixerLoop, this);
    }
    return std::nullopt;
}

void AudioEngine::stop() noexcept {
    playing_.store(false, std::memory_order_release);
    if (backend_) {
        backend_->stop();
    }
    joinMixer();
}

AudioStatus AudioEngine::seek(const RationalTime at) {
    const auto wasPlaying = playing_.load(std::memory_order_acquire);
    if (wasPlaying && backend_) {
        backend_->stop();
    }
    {
        std::scoped_lock lock(controlMutex_);
        clockOrigin_ = at;
        framesProduced_ = 0;
        framesWritten_.store(0, std::memory_order_release);
        ring_->clear();
    }
    if (wasPlaying && backend_) {
        const auto status = backend_->start(config_.rate, config_.channels, &backendCallback, this);
        if (status.has_value()) {
            playing_.store(false, std::memory_order_release);
            joinMixer();
            return status;
        }
    }
    return std::nullopt;
}

bool AudioEngine::setRate(const double rate) noexcept {
    if (!std::isfinite(rate) || rate <= 0.0 || rate > 8.0) {
        return false;
    }
    const auto scaled = static_cast<std::int64_t>(std::llround(rate * 1'000'000.0));
    if (scaled <= 0) {
        return false;
    }
    const auto divisor = std::gcd(scaled, std::int64_t{1'000'000});
    std::scoped_lock lock(controlMutex_);
    playbackRate_ = rate;
    playbackRateNumerator_ = scaled / divisor;
    playbackRateDenominator_ = 1'000'000 / divisor;
    return true;
}

AudioStatus AudioEngine::renderForTesting(const std::size_t frames) {
    auto* nullBackend = dynamic_cast<NullBackend*>(backend_.get());
    if (nullBackend == nullptr) {
        return errorStatus(AudioErrorCode::BackendUnavailable);
    }
    if (!isPlaying() || frames == 0) {
        return errorStatus(AudioErrorCode::InvalidState);
    }
    std::size_t remaining = frames;
    while (remaining > 0) {
        const auto block = std::min(remaining, kMixerBlockFrames);
        if (!produceBlock(block)) {
            return errorStatus(AudioErrorCode::BackendUnavailable);
        }
        nullBackend->render(block);
        remaining -= block;
    }
    return std::nullopt;
}

RationalTime AudioEngine::positionNow() const noexcept {
    std::scoped_lock lock(controlMutex_);
    const auto frames = framesWritten_.load(std::memory_order_acquire);
    if (playbackRateNumerator_ == playbackRateDenominator_) {
        const auto numerator =
            static_cast<long double>(clockOrigin_.numerator()) * config_.rate +
            static_cast<long double>(frames) * static_cast<long double>(clockOrigin_.denominator());
        const auto denominator =
            static_cast<long double>(clockOrigin_.denominator()) * config_.rate;
        if (numerator >= static_cast<long double>(std::numeric_limits<std::int64_t>::min()) &&
            numerator <= static_cast<long double>(std::numeric_limits<std::int64_t>::max()) &&
            denominator <= static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
            const auto exact = RationalTime::create(static_cast<std::int64_t>(numerator),
                                                    static_cast<std::int64_t>(denominator));
            if (exact.has_value()) {
                return *exact;
            }
        }
    }
    const auto seconds = clockOrigin_.toSeconds() + static_cast<double>(frames) * playbackRate_ /
                                                        static_cast<double>(config_.rate);
    constexpr auto precision = 1'000'000'000.0;
    const auto scaled = seconds * precision;
    if (!std::isfinite(scaled) ||
        scaled < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        scaled > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return clockOrigin_;
    }
    return RationalTime::create(static_cast<std::int64_t>(std::llround(scaled)),
                                static_cast<std::int64_t>(precision))
        .value_or(clockOrigin_);
}

void AudioEngine::backendCallback(void* userData, const std::span<float> output) noexcept {
    auto& engine = *static_cast<AudioEngine*>(userData);
    const auto frames = engine.ring_->pop(output);
    const auto samplesWritten = frames * engine.config_.channels;
    std::fill(output.begin() + static_cast<std::ptrdiff_t>(samplesWritten), output.end(), 0.0F);
    engine.framesWritten_.fetch_add(output.size() / engine.config_.channels,
                                    std::memory_order_release);
}

void AudioEngine::mixerLoop() noexcept {
    while (playing_.load(std::memory_order_acquire)) {
        const auto frames = std::min(kMixerBlockFrames, ring_->freeFrames());
        if (frames == 0) {
            std::this_thread::yield();
            continue;
        }
        if (!produceBlock(frames)) {
            std::this_thread::yield();
        }
    }
}

bool AudioEngine::produceBlock(const std::size_t frames) noexcept {
    if (frames == 0 || frames > kMixerBlockFrames || frames > ring_->freeFrames()) {
        return false;
    }
    const auto sampleCount = frames * config_.channels;
    const std::span<float> output{mixScratch_.data(), sampleCount};
    {
        std::scoped_lock lock(controlMutex_);
        mixBlock(framesProduced_, output);
        if (!ring_->push(output)) {
            return false;
        }
        framesProduced_ += frames;
    }
    return true;
}

void AudioEngine::mixBlock(const std::uint64_t firstProducedFrame,
                           const std::span<float> output) noexcept {
    std::fill(output.begin(), output.end(), 0.0F);
    const auto firstAbsoluteFrame = timeToFrames(clockOrigin_, config_.rate) +
                                    saturatingFrameOffset(firstProducedFrame, playbackRate_);
    for (std::size_t outputFrame = 0; outputFrame < output.size() / config_.channels;
         ++outputFrame) {
        const auto absoluteFrame =
            firstAbsoluteFrame + saturatingFrameOffset(outputFrame, playbackRate_);
        const auto anySolo = std::any_of(clips_.begin(), clips_.end(), [](const AudioClip& clip) {
            return clip.solo && !clip.muted;
        });
        for (const auto& clip : clips_) {
            if (anySolo && !clip.solo)
                continue;
            const auto sourceFrame = clipSampleFrame(clip, absoluteFrame, config_.rate);
            if (!sourceFrame)
                continue;
            for (std::size_t channel = 0; channel < config_.channels; ++channel) {
                const auto sourceChannel = clip.buffer->channels == 1 ? std::size_t{0} : channel;
                if (sourceChannel >= clip.buffer->planes.size() ||
                    *sourceFrame >= clip.buffer->planes[sourceChannel].size()) {
                    continue;
                }
                output[outputFrame * config_.channels + channel] +=
                    clip.buffer->planes[sourceChannel][*sourceFrame] * clip.level;
            }
        }
    }
}

void AudioEngine::joinMixer() noexcept {
    if (mixerThread_.joinable()) {
        mixerThread_.join();
    }
}

} // namespace bloom::media::audio::playback
