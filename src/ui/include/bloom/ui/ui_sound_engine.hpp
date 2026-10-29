#pragma once

// The SoundPlayer behind UiFeedback: a small, separate audio output for UI sounds.
//
// It owns its own media::audio::playback::Backend (miniaudio in the application, a recording fake
// in tests) and never touches the transport AudioEngine, so UI sounds cannot disturb timeline or
// preview audio. All sounds are read and decoded once, on a worker thread, before the device is
// opened; the UI thread only ever enqueues a small request into a lock-free ring that the device
// callback drains. If the device cannot be opened the engine stays silent and reports a diagnostic.

#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/ui/ui_feedback.hpp>

#include <QByteArray>

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bloom::ui {

// Mono 16-bit 48 kHz PCM from a RIFF/WAVE image; empty on any other format. Pure and thread-safe.
[[nodiscard]] std::vector<float> decodeFeedbackWav(const QByteArray& image);

// The file bytes of one bundled sound ("render_succeeded" and so on) from the application's
// resources. Safe on any thread.
[[nodiscard]] QByteArray bundledFeedbackSoundBytes(FeedbackSound sound);

class UiSoundEngine final : public SoundPlayer {
  public:
    using SoundSource = std::function<QByteArray(FeedbackSound)>;
    using Diagnostic = std::function<void(const std::string&)>;

    static constexpr std::uint32_t kRate = 48'000;
    static constexpr std::uint32_t kChannels = 2;

    enum class State : std::uint8_t { Starting, Ready, Unavailable, Stopped };

    // `source` defaults to the bundled resources. `diagnostic` may be called on the worker thread.
    UiSoundEngine(std::unique_ptr<media::audio::playback::Backend> backend,
                  SoundSource source = bundledFeedbackSoundBytes, Diagnostic diagnostic = {});
    ~UiSoundEngine() override;
    UiSoundEngine(const UiSoundEngine&) = delete;
    UiSoundEngine& operator=(const UiSoundEngine&) = delete;

    // Begins the asynchronous load and device start. Returns at once.
    void start();

    void play(FeedbackSound sound, float gain) noexcept override;
    void shutdown() noexcept override;

    [[nodiscard]] State state() const noexcept { return state_.load(std::memory_order_acquire); }
    // Blocks until the worker has finished (Ready, Unavailable or Stopped). For tests.
    void waitUntilSettled();
    // The thread that decoded and started the backend; default-constructed before start().
    [[nodiscard]] std::thread::id workerThread() const;

  private:
    struct Request final {
        std::uint8_t sound = 0;
        float gain = 0.0F;
    };
    struct Voice final {
        const float* data = nullptr;
        std::size_t length = 0;
        std::size_t position = 0;
        float gain = 0.0F;
    };
    static constexpr std::size_t kRing = 32;
    static constexpr std::size_t kVoices = 8;

    static void callback(void* userData, std::span<float> output) noexcept;
    void render(std::span<float> output) noexcept;
    void run();

    std::unique_ptr<media::audio::playback::Backend> backend_;
    SoundSource source_;
    Diagnostic diagnostic_;
    std::array<std::vector<float>, kFeedbackSoundCount> bank_;
    std::array<Request, kRing> ring_{};
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
    std::array<Voice, kVoices> voices_{};
    std::atomic<State> state_{State::Starting};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> started_{false};
    mutable std::mutex mutex_;
    std::thread worker_;
    std::thread::id workerId_;
};

} // namespace bloom::ui
