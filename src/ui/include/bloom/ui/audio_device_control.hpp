#pragma once

#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <QObject>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace bloom::ui {

class TaskUiBridge;

// Application-owned audio device boundary. The UI only publishes the newest desired state and
// reads the engine's atomic clock; one BlockingIo task at a time touches device/mixer lifecycle.
// The application keeps this owner alive through the post-event-loop drain.
class AudioDeviceControl final : public QObject {
  public:
    using CompletionHandler =
        std::function<void(std::uint64_t generation, bool playing, bool transportStarted)>;

    AudioDeviceControl(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                       std::unique_ptr<media::audio::playback::AudioEngine> engine,
                       QObject* parent = nullptr);
    ~AudioDeviceControl() override;

    void setCompletionHandler(CompletionHandler handler);
    std::uint64_t replaceClips(std::vector<media::audio::playback::AudioClip> clips);
    std::uint64_t requestPlay(core::RationalTime at);
    std::uint64_t requestSeek(core::RationalTime at);
    std::uint64_t requestStop();
    // Publishes the transport loop the mixer wraps inside (nullopt: no loop). An unchanged range
    // publishes nothing and returns the current generation.
    std::uint64_t setLoopRange(std::optional<media::audio::playback::LoopRange> range);
    void requestSilence() noexcept;
    [[nodiscard]] core::RationalTime positionNow() const noexcept;
    [[nodiscard]] core::RationalTime playbackRateRatio() const noexcept;
    [[nodiscard]] bool isPlaying() const noexcept;
    [[nodiscard]] bool isIdle() const noexcept { return !active_ && !pending_; }

    // Begin is UI-safe and never joins. A permanently refused terminal task yields to the
    // explicit post-loop guard; no scheduler retry is required after admission has closed.
    void beginShutdown();
    [[nodiscard]] bool retirementComplete();
    void drainAfterEventLoop() noexcept;

  private:
    struct State;
    struct Completion;
    using CompletionPtr = std::shared_ptr<const Completion>;
    struct Desired final {
        std::shared_ptr<const std::vector<media::audio::playback::AudioClip>> clips;
        core::RationalTime at{};
        std::optional<media::audio::playback::LoopRange> loop;
        std::uint64_t mixSerial = 0;
        std::uint64_t loopSerial = 0;
        std::uint64_t transportSerial = 0;
        std::uint64_t generation = 0;
        std::uint64_t silenceGeneration = 0;
        bool playing = false;
    };
    struct Active final {
        runtime::TaskHandle<CompletionPtr> handle;
        bool terminal = false;
    };

    void publishDesired();
    void consumeReady();
    void submitPending();
    void submitTerminal();

    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    std::shared_ptr<State> state_;
    Desired desired_;
    std::optional<Active> active_;
    CompletionHandler completionHandler_;
    std::chrono::steady_clock::time_point shutdownDeadline_{};
    std::uint64_t nextGeneration_ = 0;
    bool pending_ = false;
    bool closing_ = false;
    bool fallbackRequired_ = false;
};

} // namespace bloom::ui
