#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <QObject>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

class QWidget;

namespace bloom::core {
class FrameTimeMapping;
} // namespace bloom::core

namespace bloom::ui {

class CompositionSession;
class CompositionPreviewController;
class AudioDeviceControl;

// Transport state for composition preview playback (issue #105). Only two states exist: a paused
// transport and a playing one both freeze/advance the SAME CompositionSession::currentTime(), so
// there is no separate "paused at a different time" state to model.
enum class PlaybackState : std::uint8_t {
    Stopped,
    Playing,
};

// Real-time transport uses total elapsed time or the existing audio master clock. Cache
// membership affects preparation only; it never selects a slower clock or accumulates tick debt.
// A run anchors both clocks to the nearest valid grid frame (halfway ties greater). Ordinary
// nearest rounding aligns audio by at most half a frame; clamping to the valid grid in the work
// area can require larger alignment, including at off-grid authored endpoints after rate edits.
// play() leaves the immediate stored session time unchanged unless the existing outside-range
// clamp is needed.
class PlaybackController final : public QObject {
    Q_OBJECT

  public:
    // Injectable monotonic clock (design decision 1): production reads
    // std::chrono::steady_clock::now(); tests substitute a manually-advanced fake with no
    // dependency on real elapsed wall time.
    using ClockFunction = std::function<std::chrono::steady_clock::time_point()>;
    explicit PlaybackController(
        CompositionSession& session, CompositionPreviewController& previewController,
        ClockFunction clock = &std::chrono::steady_clock::now,
        std::chrono::milliseconds tickInterval = std::chrono::milliseconds{16},
        QObject* parent = nullptr);

    ~PlaybackController() override;
    [[nodiscard]] PlaybackState state() const noexcept;
    // Task VIEW-1: whether reaching the end of the work area wraps to its start (the behavior this
    // transport has always had, and still the default) or stops on the last frame. The viewer
    // footer's loop control is a real toggle rather than the status glyph the timeline used to
    // show, so there is now a command behind it -- see setLooping() below.
    [[nodiscard]] bool isLooping() const noexcept;
    // One action on the window, independent of panel visibility or lifetime. Text entry keeps
    // Space via Qt's ShortcutOverride mechanism, just like the window's backtick shortcut.
    void installWindowShortcut(QWidget& window);
    void setAudioDeviceControl(AudioDeviceControl* control);
    void setAudioEnabled(bool enabled);
    [[nodiscard]] bool isAudioEnabled() const noexcept { return audioEnabled_; }
    [[nodiscard]] bool audioClockActive() const noexcept { return audioClockActive_; }
    void setAudioMix(runtime::AudioMixDescription description,
                     std::vector<media::audio::playback::AudioClip> clips);

  public slots:
    // No-op (guarded) if already playing, if no composition is available, or if the composition's
    // duration/frame rate cannot form a valid bloom::core::FrameTimeMapping (covers the
    // zero-duration case). Captures the CURRENT session time as the new start time -- a later
    // play() after a pause resumes from wherever the session is now, never from the original start
    // (design decision 2).
    void play();
    // No-op if already stopped. Freezes session time exactly where the last applied tick (or
    // play() itself, if no tick has landed yet) left it -- no snap-back.
    void pause();
    void toggle();
    // Takes effect on the NEXT tick; it never moves session time by itself, so toggling looping
    // mid-run neither restarts nor re-seeks playback. Turning looping off mid-run keeps the passes
    // already played: the run finishes the pass it is in and pauses on the work area's last frame.
    // The audio mixer follows the same toggle in place, without a device restart.
    void setLooping(bool looping);

    // Advances playback by one tick using the injected clock's current reading. Production wires
    // the internal ~16 ms QTimer's timeout() here (mirroring CompositionPreviewController's
    // injectable-cadence constructor seam); tests call this directly after advancing the injected
    // clock, with no dependency on a real event loop or real elapsed wall time -- the timer is
    // still constructed and started in production but a test that never pumps Qt's event loop
    // never observes it fire, so manual tick() calls stay fully deterministic.
    void tick();

  signals:
    void stateChanged(PlaybackState state);
    void loopingChanged(bool looping);

  private:
    void handleCompositionChanged();
    void handleCurrentTimeChanged();
    void handleWorkAreaChanged();
    void handleSnapshotChanged();
    void handleAudioCompletion(std::uint64_t generation, bool playing, bool transportStarted);
    void useWallClock();
    // Hands the mixer the work area's frame-grid loop (or no loop) so audio wraps with the video.
    void publishAudioLoop();
    void scheduleTick(std::chrono::nanoseconds untilNextFrame);

    CompositionSession& session_;
    CompositionPreviewController& previewController_;
    ClockFunction clock_;
    std::chrono::milliseconds wakeCap_{16};
    QTimer timer_;
    PlaybackState state_ = PlaybackState::Stopped;
    std::chrono::steady_clock::time_point startClock_{};
    std::uint64_t startFrameIndex_ = 0;
    std::uint32_t runRateNumerator_ = 0, runRateDenominator_ = 0;
    core::RationalTime runRangeStart_{}, runRangeEnd_{};
    // Elapsed/audio-derived offset from the fixed run origin, never accumulated timer ticks.
    std::uint64_t appliedOffset_ = 0;
    // Whole loop passes already played when looping was turned off mid-run (a multiple of the
    // work-area frame count), so the non-looping end test starts from the current pass.
    std::uint64_t completedPassFrames_ = 0;
    std::optional<std::uint64_t> lastAppliedFrameIndex_;
    // Guards handleCurrentTimeChanged()'s scrub-during-playback detection: set around
    // PlaybackController's own session_.setCurrentTime() call so that signal is not mistaken for
    // an external scrub/direct-manipulation change while playing (see tick()'s only caller of
    // setCurrentTime() and handleCurrentTimeChanged()'s own comment).
    bool applyingOwnTimeChange_ = false;
    // True by default: every transport Bloom has shipped so far loops, and the footer's toggle is
    // what makes the other half of that statement reachable rather than a promise.
    bool looping_ = true;
    AudioDeviceControl* audioControl_ = nullptr;
    std::uint64_t expectedAudioGeneration_ = 0;
    bool audioAlignmentRetried_ = false;
    std::optional<runtime::AudioMixDescription> audioMix_;
    bool audioClipsAvailable_ = false;
    bool audioEnabled_ = true;
    bool audioClockActive_ = false;
    bool audioStartPending_ = false;
};

} // namespace bloom::ui
