#pragma once

#include <bloom/media/audio/audio.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace bloom::media::audio::playback {

using AudioStatus = std::optional<AudioError>;

class Backend {
  public:
    using Callback = void (*)(void* userData, std::span<float> output) noexcept;

    virtual ~Backend() = default;
    virtual AudioStatus start(std::uint32_t rate, std::uint32_t channels, Callback callback,
                              void* userData) noexcept = 0;
    virtual void stop() noexcept = 0;
};

class NullBackend final : public Backend {
  public:
    AudioStatus start(std::uint32_t rate, std::uint32_t channels, Callback callback,
                      void* userData) noexcept override;
    void stop() noexcept override;

    // Test-only pull. The callback receives exactly `frames` frames and the interleaved output is
    // appended to captured().
    void render(std::size_t frames);
    void clearCaptured() noexcept;

    [[nodiscard]] std::span<const float> captured() const noexcept { return captured_; }
    [[nodiscard]] std::uint32_t rate() const noexcept { return rate_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }

  private:
    Callback callback_ = nullptr;
    void* userData_ = nullptr;
    std::uint32_t rate_ = 0;
    std::uint32_t channels_ = 0;
    bool started_ = false;
    std::vector<float> captured_;
};

class MiniaudioBackend final : public Backend {
  public:
    MiniaudioBackend();
    ~MiniaudioBackend() override;

    MiniaudioBackend(const MiniaudioBackend&) = delete;
    MiniaudioBackend& operator=(const MiniaudioBackend&) = delete;
    MiniaudioBackend(MiniaudioBackend&&) noexcept;
    MiniaudioBackend& operator=(MiniaudioBackend&&) noexcept;

    AudioStatus start(std::uint32_t rate, std::uint32_t channels, Callback callback,
                      void* userData) noexcept override;
    void stop() noexcept override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::unique_ptr<Backend> makeMiniaudioBackend();

struct AudioClip final {
    // Null means "no sample": the clip contributes silence. Shared so the engine never
    // deep-copies decoded audio (a shared decode can be tens of megabytes) -- callers hand in
    // the same AssetController-owned buffer that feeds every clip built from that asset.
    std::shared_ptr<const AudioBuffer> buffer;
    core::RationalTime startTime{};
    float level = 1.0F;
    bool muted = false;
    bool solo = false;
    std::optional<core::RationalTime> endTime{};
    // Optional; invocation must not allocate or throw. Runs on the mixer worker, never the device
    // callback. nullopt means silence outside an enclosing composition/layer range.
    std::function<std::optional<core::RationalTime>(core::RationalTime)> mapTime{};
};

class AudioEngine final {
  public:
    using ClipId = std::size_t;

    struct Config final {
        std::uint32_t rate = 48'000;
        std::uint32_t channels = 2;
        std::size_t ringBufferFrames = 16'384;
    };

    explicit AudioEngine(std::unique_ptr<Backend> backend);
    AudioEngine(std::unique_ptr<Backend> backend, Config config);
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    AudioEngine(AudioEngine&&) = delete;
    AudioEngine& operator=(AudioEngine&&) = delete;

    [[nodiscard]] ClipId addClip(AudioClip clip);
    void replaceClips(std::vector<AudioClip> clips);
    void clearClips() noexcept;
    [[nodiscard]] bool removeClip(ClipId id);
    [[nodiscard]] bool updateClip(ClipId id, float level, bool muted, bool solo) noexcept;

    // The returned error is empty on success. `at` is the rational transport position at which
    // playback begins; the backend callback's written-frame count advances from there.
    AudioStatus play(core::RationalTime at);
    void stop() noexcept;
    AudioStatus seek(core::RationalTime at);
    [[nodiscard]] bool setRate(double rate) noexcept;

    // Pulls exactly `frames` through a NullBackend. It is deliberately unavailable for a device
    // backend so production playback always has the asynchronous mixer/callback boundary.
    AudioStatus renderForTesting(std::size_t frames);

    [[nodiscard]] core::RationalTime positionNow() const noexcept;
    [[nodiscard]] bool isPlaying() const noexcept {
        return playing_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint32_t rate() const noexcept { return config_.rate; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return config_.channels; }

  private:
    class SpscRing;

    static constexpr std::size_t kMixerBlockFrames = 512;

    static void backendCallback(void* userData, std::span<float> output) noexcept;
    void mixerLoop() noexcept;
    bool produceBlock(std::size_t frames) noexcept;
    void mixBlock(std::uint64_t firstProducedFrame, std::span<float> output) noexcept;
    void joinMixer() noexcept;

    std::unique_ptr<Backend> backend_;
    Config config_;
    std::unique_ptr<SpscRing> ring_;
    std::vector<float> mixScratch_;
    mutable std::mutex controlMutex_;
    std::vector<AudioClip> clips_;
    std::atomic<bool> playing_{false};
    std::atomic<std::uint64_t> framesWritten_{0};
    std::thread mixerThread_;
    std::uint64_t framesProduced_ = 0;
    core::RationalTime clockOrigin_{};
    double playbackRate_ = 1.0;
    std::int64_t playbackRateNumerator_ = 1;
    std::int64_t playbackRateDenominator_ = 1;
};

} // namespace bloom::media::audio::playback
