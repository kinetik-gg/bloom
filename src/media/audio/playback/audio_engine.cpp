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
        clip.buffer->channels == 0 ||
        (!clip.buffer->stream && clip.buffer->planes.size() != clip.buffer->channels)) {
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
    if (sourceFrame >= clip.buffer->frames ||
        (!clip.buffer->stream && sourceFrame >= clip.buffer->planes.front().size()))
        return std::nullopt;
    return sourceFrame;
}

[[nodiscard]] AudioStatus errorStatus(const AudioErrorCode code) noexcept {
    return AudioStatus{AudioError::codeOnly(code)};
}

[[nodiscard]] std::optional<std::int64_t> checkedProduct(const std::int64_t lhs,
                                                         const std::int64_t rhs) noexcept {
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    if (lhs == 0 || rhs == 0)
        return std::int64_t{0};
    const bool overflow = lhs > 0 ? (rhs > 0 ? lhs > max / rhs : rhs < min / lhs)
                                  : (rhs > 0 ? lhs < min / rhs : rhs < max / lhs);
    if (overflow)
        return std::nullopt;
    return lhs * rhs;
}

// `time` in units of 1/scale output frames, exactly; nullopt when it is not representable.
[[nodiscard]] std::optional<std::int64_t>
scaledFrames(const RationalTime time, const std::int64_t scale, const std::uint32_t rate) noexcept {
    const auto numerator = checkedProduct(time.numerator(), scale / time.denominator());
    return numerator ? checkedProduct(*numerator, static_cast<std::int64_t>(rate)) : std::nullopt;
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

    // Producer side, only while the consumer is stopped.
    void clear() noexcept {
        const auto write = writeFrame_.load(std::memory_order_acquire);
        readFrame_.store(write, std::memory_order_release);
    }

    [[nodiscard]] std::size_t queued() const noexcept {
        return writeFrame_.load(std::memory_order_acquire) -
               readFrame_.load(std::memory_order_acquire);
    }

    // Producer side: true when nothing is queued, so no frame can be in flight to the consumer.
    [[nodiscard]] bool empty() const noexcept {
        return writeFrame_.load(std::memory_order_relaxed) ==
               readFrame_.load(std::memory_order_acquire);
    }

    // Consumer side: drops every queued frame and returns how many were dropped.
    [[nodiscard]] std::size_t discardQueued() noexcept {
        const auto read = readFrame_.load(std::memory_order_relaxed);
        const auto write = writeFrame_.load(std::memory_order_acquire);
        readFrame_.store(write, std::memory_order_release);
        return write - read;
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
      mixScratch_(kMixerBlockFrames * config.channels) {
    clockState_.store(std::make_shared<const ClockState>(), std::memory_order_release);
}

AudioEngine::~AudioEngine() { stop(); }

AudioEngine::ClipId AudioEngine::addClip(AudioClip clip) {
    std::scoped_lock lock(controlMutex_);
    clips_.push_back(std::move(clip));
    return clips_.size() - 1U;
}

void AudioEngine::replaceClips(std::vector<AudioClip> clips) {
    {
        std::scoped_lock lock(controlMutex_);
        clips_.swap(clips);
    } // Old decoded refs are released outside the mixer mutex.
}

void AudioEngine::clearClips() noexcept {
    std::vector<AudioClip> old;
    {
        std::scoped_lock lock(controlMutex_);
        old.swap(clips_);
    }
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

AudioStatus AudioEngine::play(const RationalTime at) { return play(at, silenceGeneration()); }

AudioStatus AudioEngine::play(const RationalTime at, const std::uint64_t silenceGeneration) {
    stopAfterSilenceRequest();
    if (!backend_ || config_.rate == 0 || config_.channels == 0 || config_.ringBufferFrames == 0) {
        return errorStatus(AudioErrorCode::InvalidAudioFormat);
    }
    {
        std::scoped_lock lock(controlMutex_);
        clockOrigin_ = at;
        writtenAtOrigin_ = framesWritten_.load(std::memory_order_acquire);
        clockState_.store(std::make_shared<const ClockState>(
                              ClockState{clockOrigin_, writtenAtOrigin_, playbackRate_,
                                         playbackRateNumerator_, playbackRateDenominator_}),
                          std::memory_order_release);
        resetTransportLocked();
    }
    ringWakeEpoch_.fetch_add(1, std::memory_order_release);
    ringWakeEpoch_.notify_all();
    playing_.store(true, std::memory_order_release);
    const auto status = backend_->start(config_.rate, config_.channels, &backendCallback, this);
    if (status.has_value()) {
        playing_.store(false, std::memory_order_release);
        return status;
    }
    if (dynamic_cast<NullBackend*>(backend_.get()) == nullptr) {
        mixerThread_ = std::thread(&AudioEngine::mixerLoop, this);
    }
    auto expected = (silenceGeneration << 1) | 1;
    (void)silenceState_.compare_exchange_strong(expected, silenceGeneration << 1,
                                                std::memory_order_acq_rel);
    return std::nullopt;
}

void AudioEngine::stop() noexcept {
    requestSilence();
    stopAfterSilenceRequest();
}

void AudioEngine::stopAfterSilenceRequest() noexcept {
    silenceState_.fetch_or(1, std::memory_order_release);
    playing_.store(false, std::memory_order_release);
    ringWakeEpoch_.fetch_add(1, std::memory_order_release);
    ringWakeEpoch_.notify_all();
    if (backend_) {
        backend_->stop();
    }
    joinMixer();
}

AudioStatus AudioEngine::seek(const RationalTime at) {
    // Allocate before stopping the backend. A failed allocation must not strand a live mixer with
    // its device stopped and no restart path.
    auto nextClock = std::make_shared<ClockState>();
    mixGeneration_.fetch_add(1, std::memory_order_acq_rel);
    const auto wasPlaying = playing_.load(std::memory_order_acquire);
    if (wasPlaying && backend_) {
        backend_->stop();
    }
    {
        std::scoped_lock lock(controlMutex_);
        clockOrigin_ = at;
        writtenAtOrigin_ = framesWritten_.load(std::memory_order_acquire);
        *nextClock = ClockState{clockOrigin_, writtenAtOrigin_, playbackRate_,
                                playbackRateNumerator_, playbackRateDenominator_};
        clockState_.store(std::move(nextClock), std::memory_order_release);
        resetTransportLocked();
    }
    ringWakeEpoch_.fetch_add(1, std::memory_order_release);
    ringWakeEpoch_.notify_all();
    if (wasPlaying && backend_) {
        const auto status = backend_->start(config_.rate, config_.channels, &backendCallback, this);
        if (status.has_value()) {
            playing_.store(false, std::memory_order_release);
            ringWakeEpoch_.fetch_add(1, std::memory_order_release);
            ringWakeEpoch_.notify_all();
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
    std::shared_ptr<const ClockState> state;
    try {
        state = std::make_shared<const ClockState>(ClockState{
            clockOrigin_, writtenAtOrigin_, rate, scaled / divisor, 1'000'000 / divisor});
    } catch (...) {
        return false;
    }
    playbackRate_ = rate;
    playbackRateNumerator_ = scaled / divisor;
    playbackRateDenominator_ = 1'000'000 / divisor;
    clockState_.store(std::move(state), std::memory_order_release);
    return true;
}

bool AudioEngine::setLoopRange(const std::optional<LoopRange> range) noexcept {
    std::optional<LoopFrames> frames;
    if (range) {
        if (range->start < RationalTime{} || range->end <= range->start)
            return false;
        const auto startDenominator = range->start.denominator();
        const auto endDenominator = range->end.denominator();
        const auto scale = checkedProduct(
            startDenominator / std::gcd(startDenominator, endDenominator), endDenominator);
        if (!scale)
            return false;
        const auto start = scaledFrames(range->start, *scale, config_.rate);
        const auto end = scaledFrames(range->end, *scale, config_.rate);
        if (!start || !end || *end <= *start)
            return false;
        frames = LoopFrames{*scale, *start, *end};
    }
    {
        std::scoped_lock lock(controlMutex_);
        if (requestedLoop_ == range)
            return true;
        requestedLoop_ = range;
        requestedLoopFrames_ = frames;
        if (!playing_.load(std::memory_order_acquire)) {
            loopFrames_ = frames;
            loopDirty_ = false;
            heldPassFrames_ = 0;
            return true;
        }
        loopDirty_ = true;
        // A rewind already in progress covers this change too: the mixer has pushed nothing since
        // it was requested, and applies the newest requested loop once it settles.
        auto idle = Rewind::Idle;
        (void)rewind_.compare_exchange_strong(idle, Rewind::Requested, std::memory_order_acq_rel);
    }
    ringWakeEpoch_.fetch_add(1, std::memory_order_release);
    ringWakeEpoch_.notify_all();
    return true;
}

std::optional<LoopRange> AudioEngine::loopRange() const {
    std::scoped_lock lock(controlMutex_);
    return requestedLoop_;
}

void AudioEngine::resetTransportLocked() noexcept {
    framesProduced_ = 0;
    heldPassFrames_ = 0;
    if (loopDirty_) {
        loopFrames_ = requestedLoopFrames_;
        loopDirty_ = false;
    }
    rewind_.store(Rewind::Idle, std::memory_order_release);
    rewindDiscarded_.store(0, std::memory_order_relaxed);
    underrunFrames_.store(0, std::memory_order_relaxed);
    ring_->clear();
}

bool AudioEngine::settleRewind() noexcept {
    auto state = rewind_.load(std::memory_order_acquire);
    if (state == Rewind::Requested && ring_->empty() &&
        rewind_.compare_exchange_strong(state, Rewind::Done, std::memory_order_acq_rel)) {
        // Nothing queued, so nothing to discard: the mixer settles the request itself.
        rewindDiscarded_.store(0, std::memory_order_relaxed);
        state = Rewind::Done;
    }
    if (state == Rewind::Done) {
        const auto discarded = rewindDiscarded_.load(std::memory_order_relaxed);
        framesProduced_ = framesProduced_ >= discarded ? framesProduced_ - discarded : 0;
        rewind_.store(Rewind::Idle, std::memory_order_release);
        state = Rewind::Idle;
    }
    if (state != Rewind::Idle)
        return false;
    if (loopDirty_)
        applyLoop(framesProduced_);
    return true;
}

void AudioEngine::applyLoop(const std::uint64_t producedFrame) noexcept {
    if (requestedLoopFrames_) {
        heldPassFrames_ = 0;
    } else if (loopFrames_) {
        // Keep the pass this run is in: the offset that pass applies stays fixed, so the run plays
        // on through that pass's end like one that never looped, rather than jumping to the
        // unwrapped timeline.
        const auto absolute = timeToFrames(clockOrigin_, config_.rate) +
                              saturatingFrameOffset(producedFrame, playbackRate_);
        heldPassFrames_ = absolute - loopedFrame(absolute);
    }
    loopFrames_ = requestedLoopFrames_;
    loopDirty_ = false;
}

std::int64_t AudioEngine::loopedFrame(const std::int64_t absoluteFrame) const noexcept {
    if (!loopFrames_) {
        return absoluteFrame >= std::numeric_limits<std::int64_t>::min() + heldPassFrames_
                   ? absoluteFrame - heldPassFrames_
                   : std::numeric_limits<std::int64_t>::min();
    }
    const auto& loop = *loopFrames_;
    const auto length = loop.end - loop.start;
    if (const auto scaled = checkedProduct(absoluteFrame, loop.scale); scaled) {
        if (*scaled < loop.end)
            return absoluteFrame;
        // start >= 0 and scaled >= end > start, so every term is non-negative and the division
        // floors.
        return (loop.start + (*scaled - loop.start) % length) / loop.scale;
    }
    // Only reachable after centuries of looping at an ordinary frame-grid scale.
    const auto scaled =
        static_cast<long double>(absoluteFrame) * static_cast<long double>(loop.scale);
    if (scaled < static_cast<long double>(loop.end))
        return absoluteFrame;
    const auto wrapped =
        static_cast<long double>(loop.start) +
        std::fmod(scaled - static_cast<long double>(loop.start), static_cast<long double>(length));
    return static_cast<std::int64_t>(std::floor(wrapped / static_cast<long double>(loop.scale)));
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

std::size_t AudioEngine::queuedFramesForTesting() const noexcept { return ring_->queued(); }

RationalTime AudioEngine::playbackRateRatio() const noexcept {
    const auto state = clockState_.load(std::memory_order_acquire);
    return RationalTime::create(state->rateNumerator, state->rateDenominator)
        .value_or(RationalTime{});
}

RationalTime AudioEngine::positionNow() const noexcept {
    const auto state = clockState_.load(std::memory_order_acquire);
    const auto totalWritten = framesWritten_.load(std::memory_order_acquire);
    const auto frames = totalWritten - state->writtenAtOrigin;
    if (state->rateNumerator == state->rateDenominator) {
        const auto numerator = static_cast<long double>(state->origin.numerator()) * config_.rate +
                               static_cast<long double>(frames) *
                                   static_cast<long double>(state->origin.denominator());
        const auto denominator =
            static_cast<long double>(state->origin.denominator()) * config_.rate;
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
    const auto seconds = state->origin.toSeconds() + static_cast<double>(frames) * state->rate /
                                                         static_cast<double>(config_.rate);
    constexpr auto precision = 1'000'000'000.0;
    const auto scaled = seconds * precision;
    if (!std::isfinite(scaled) ||
        scaled < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        scaled > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return state->origin;
    }
    return RationalTime::create(static_cast<std::int64_t>(std::llround(scaled)),
                                static_cast<std::int64_t>(precision))
        .value_or(state->origin);
}

void AudioEngine::backendCallback(void* userData, const std::span<float> output) noexcept {
    auto& engine = *static_cast<AudioEngine*>(userData);
    const auto requested = output.size() / engine.config_.channels;
    const auto frames = engine.ring_->pop(output);
    const auto samplesWritten = frames * engine.config_.channels;
    std::fill(output.begin() + static_cast<std::ptrdiff_t>(samplesWritten), output.end(), 0.0F);
    if ((engine.silenceState_.load(std::memory_order_acquire) & 1U) != 0)
        std::fill(output.begin(), output.end(), 0.0F);
    bool rewound = false;
    if (auto state = engine.rewind_.load(std::memory_order_acquire);
        state == Rewind::Requested &&
        engine.rewind_.compare_exchange_strong(state, Rewind::Claimed, std::memory_order_acq_rel)) {
        // The frames just played stay played; everything still queued is re-mixed.
        engine.rewindDiscarded_.store(engine.ring_->discardQueued(), std::memory_order_relaxed);
        engine.rewind_.store(Rewind::Done, std::memory_order_release);
        rewound = true;
    }
    if (frames < requested)
        engine.underrunFrames_.fetch_add(requested - frames, std::memory_order_relaxed);
    engine.framesWritten_.fetch_add(requested, std::memory_order_release);
    if (frames != 0 || rewound) {
        engine.ringWakeEpoch_.fetch_add(1, std::memory_order_release);
        engine.ringWakeEpoch_.notify_one();
    }
}

void AudioEngine::mixerLoop() noexcept {
    while (playing_.load(std::memory_order_acquire)) {
        const auto frames = std::min(kMixerBlockFrames, ring_->freeFrames());
        if (frames == 0) {
            const auto epoch = ringWakeEpoch_.load(std::memory_order_acquire);
            if (playing_.load(std::memory_order_acquire) && ring_->freeFrames() == 0)
                ringWakeEpoch_.wait(epoch, std::memory_order_acquire);
            continue;
        }
        if (!produceBlock(frames)) {
            const auto epoch = ringWakeEpoch_.load(std::memory_order_acquire);
            const auto rewind = rewind_.load(std::memory_order_acquire);
            if (playing_.load(std::memory_order_acquire) &&
                (ring_->freeFrames() == 0 || rewind == Rewind::Requested ||
                 rewind == Rewind::Claimed))
                ringWakeEpoch_.wait(epoch, std::memory_order_acquire);
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
        if (!settleRewind())
            return false;
        // Frames the device played while the ring was empty already passed on the clock.
        framesProduced_ += underrunFrames_.exchange(0, std::memory_order_relaxed);
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
    const auto generation = mixGeneration_.load(std::memory_order_acquire);
    const CancelAudioWork cancelled = [this, generation] {
        return !playing_.load(std::memory_order_acquire) ||
               generation != mixGeneration_.load(std::memory_order_acquire);
    };
    const auto firstAbsoluteFrame = timeToFrames(clockOrigin_, config_.rate) +
                                    saturatingFrameOffset(firstProducedFrame, playbackRate_);
    const auto anySolo = std::any_of(clips_.begin(), clips_.end(), [](const AudioClip& clip) {
        return clip.solo && !clip.muted;
    });
    for (std::size_t outputFrame = 0; outputFrame < output.size() / config_.channels;
         ++outputFrame) {
        const auto absoluteFrame =
            loopedFrame(firstAbsoluteFrame + saturatingFrameOffset(outputFrame, playbackRate_));
        for (const auto& clip : clips_) {
            if (anySolo && !clip.solo)
                continue;
            const auto sourceFrame = clipSampleFrame(clip, absoluteFrame, config_.rate);
            if (!sourceFrame)
                continue;
            for (std::size_t channel = 0; channel < config_.channels; ++channel) {
                const auto sourceChannel = clip.buffer->channels == 1 ? std::size_t{0} : channel;
                if (sourceChannel >= clip.buffer->channels) {
                    continue;
                }
                if (clip.buffer->stream) {
                    if (const auto sample = clip.buffer->stream->sample(
                            *sourceFrame, static_cast<std::uint32_t>(sourceChannel), cancelled))
                        output[outputFrame * config_.channels + channel] += *sample * clip.level;
                } else if (*sourceFrame < clip.buffer->planes[sourceChannel].size()) {
                    output[outputFrame * config_.channels + channel] +=
                        clip.buffer->planes[sourceChannel][*sourceFrame] * clip.level;
                }
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
