#include <bloom/ui/ui_sound_engine.hpp>

#include <QFile>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace bloom::ui {

namespace {

[[nodiscard]] std::uint32_t le32(const char* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}
[[nodiscard]] std::uint16_t le16(const char* p) {
    std::uint16_t v = 0;
    std::memcpy(&v, p, 2);
    return v;
}

} // namespace

std::vector<float> decodeFeedbackWav(const QByteArray& image) {
    const char* data = image.constData();
    const auto size = static_cast<std::size_t>(image.size());
    if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        return {};
    }
    bool formatOk = false;
    std::size_t at = 12;
    while (at + 8 <= size) {
        const char* chunk = data + at;
        const std::size_t length = le32(chunk + 4);
        const std::size_t body = at + 8;
        if (body + length > size) {
            return {};
        }
        if (std::memcmp(chunk, "fmt ", 4) == 0 && length >= 16) {
            formatOk = le16(data + body) == 1 && le16(data + body + 2) == 1 &&
                       le32(data + body + 4) == UiSoundEngine::kRate &&
                       le16(data + body + 14) == 16;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            if (!formatOk) {
                return {};
            }
            std::vector<float> samples(length / 2);
            for (std::size_t i = 0; i < samples.size(); ++i) {
                std::int16_t s = 0;
                std::memcpy(&s, data + body + i * 2, 2);
                samples[i] = static_cast<float>(s) / 32768.0F;
            }
            return samples;
        }
        at = body + length + (length & 1U);
    }
    return {};
}

QByteArray bundledFeedbackSoundBytes(const FeedbackSound sound) {
    QFile file(QStringLiteral(":/bloom/sound/%1.wav").arg(QLatin1StringView(feedbackSoundName(sound))));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

UiSoundEngine::UiSoundEngine(std::unique_ptr<media::audio::playback::Backend> backend,
                             SoundSource source, Diagnostic diagnostic)
    : backend_(std::move(backend)), source_(std::move(source)),
      diagnostic_(std::move(diagnostic)) {}

UiSoundEngine::~UiSoundEngine() { shutdown(); }

void UiSoundEngine::start() {
    const std::lock_guard lock(mutex_);
    if (worker_.joinable() || stopRequested_.load()) {
        return;
    }
    started_.store(true);
    worker_ = std::thread([this] { run(); });
}

void UiSoundEngine::run() {
    {
        const std::lock_guard lock(mutex_);
        workerId_ = std::this_thread::get_id();
    }
    std::size_t decoded = 0;
    for (std::size_t i = 0; i < kFeedbackSoundCount && !stopRequested_.load(); ++i) {
        bank_[i] = decodeFeedbackWav(source_(static_cast<FeedbackSound>(i)));
        if (bank_[i].empty()) {
            if (diagnostic_) {
                diagnostic_(std::string{"UI sound unavailable: "} +
                            feedbackSoundName(static_cast<FeedbackSound>(i)));
            }
        } else {
            ++decoded;
        }
    }
    if (stopRequested_.load()) {
        state_.store(State::Stopped, std::memory_order_release);
        return;
    }
    if (decoded == 0) {
        state_.store(State::Unavailable, std::memory_order_release);
        return;
    }
    if (const auto error = backend_->start(kRate, kChannels, &UiSoundEngine::callback, this)) {
        if (diagnostic_) {
            diagnostic_("UI sounds disabled: no audio output device (backend error " +
                        std::to_string(error->observed) + ")");
        }
        state_.store(State::Unavailable, std::memory_order_release);
        return;
    }
    state_.store(stopRequested_.load() ? State::Stopped : State::Ready,
                 std::memory_order_release);
}

void UiSoundEngine::play(const FeedbackSound sound, const float gain) noexcept {
    if (state_.load(std::memory_order_acquire) != State::Ready) {
        return;
    }
    const auto tail = tail_.load(std::memory_order_relaxed);
    const auto next = (tail + 1) % kRing;
    if (next == head_.load(std::memory_order_acquire)) {
        return; // Full: drop. A UI sound is never worth waiting for.
    }
    ring_[tail] = Request{static_cast<std::uint8_t>(sound), gain};
    tail_.store(next, std::memory_order_release);
}

void UiSoundEngine::callback(void* userData, std::span<float> output) noexcept {
    static_cast<UiSoundEngine*>(userData)->render(output);
}

void UiSoundEngine::render(std::span<float> output) noexcept {
    std::fill(output.begin(), output.end(), 0.0F);
    auto head = head_.load(std::memory_order_relaxed);
    const auto tail = tail_.load(std::memory_order_acquire);
    while (head != tail) {
        const Request request = ring_[head];
        head = (head + 1) % kRing;
        const auto& samples = bank_[request.sound];
        if (samples.empty()) {
            continue;
        }
        // A free voice, else the one nearest its end.
        Voice* slot = &voices_[0];
        for (auto& voice : voices_) {
            if (voice.data == nullptr) {
                slot = &voice;
                break;
            }
            if (voice.length - voice.position < slot->length - slot->position) {
                slot = &voice;
            }
        }
        *slot = Voice{samples.data(), samples.size(), 0, request.gain};
    }
    head_.store(head, std::memory_order_release);

    const std::size_t frames = output.size() / kChannels;
    for (auto& voice : voices_) {
        if (voice.data == nullptr) {
            continue;
        }
        const std::size_t count = std::min(frames, voice.length - voice.position);
        for (std::size_t f = 0; f < count; ++f) {
            const float s = voice.data[voice.position + f] * voice.gain;
            for (std::uint32_t c = 0; c < kChannels; ++c) {
                output[f * kChannels + c] += s;
            }
        }
        voice.position += count;
        if (voice.position >= voice.length) {
            voice = Voice{};
        }
    }
    for (auto& s : output) {
        s = std::clamp(s, -1.0F, 1.0F);
    }
}

void UiSoundEngine::shutdown() noexcept {
    stopRequested_.store(true);
    std::thread worker;
    {
        const std::lock_guard lock(mutex_);
        worker = std::move(worker_);
    }
    if (worker.joinable()) {
        worker.join();
    }
    if (backend_ != nullptr) {
        backend_->stop();
    }
    state_.store(State::Stopped, std::memory_order_release);
}

void UiSoundEngine::waitUntilSettled() {
    while (state() == State::Starting && started_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
}

std::thread::id UiSoundEngine::workerThread() const {
    const std::lock_guard lock(mutex_);
    return workerId_;
}

} // namespace bloom::ui
