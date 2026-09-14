#pragma once

#include <bloom/core/rational_time.hpp>

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

// Transport state for composition preview playback (issue #105). Only two states exist: a paused
// transport and a playing one both freeze/advance the SAME CompositionSession::currentTime(), so
// there is no separate "paused at a different time" state to model.
enum class PlaybackState : std::uint8_t {
    Stopped,
    Playing,
};

// Exact elapsed-time transport with a frame-by-frame cached path. Cache misses never wait:
// the preview controller admits Visible work only when its delivery estimate fits the next tick.
// A cached next frame advances once per tick, preserving the explicit RAM Preview's exact mode.
// Uncached catch-up skips indices and includes those skips in the run's dropped-frame count.
class PlaybackController final : public QObject {
    Q_OBJECT

  public:
    // Injectable monotonic clock (design decision 1): production reads
    // std::chrono::steady_clock::now(); tests substitute a manually-advanced fake with no
    // dependency on real elapsed wall time.
    using ClockFunction = std::function<std::chrono::steady_clock::time_point()>;
    // Whether the frame at this composition frame index is already in the RAM preview cache, which
    // is what decides which of the two clocks above a tick uses. Empty means "ask the preview
    // controller's own cache", which is what production wants and what this controller does by
    // default; a test injects one to pin either clock without rendering a frame.
    using FrameCachedPredicate = std::function<bool(std::uint64_t frameIndex)>;

    explicit PlaybackController(
        CompositionSession& session, CompositionPreviewController& previewController,
        ClockFunction clock = &std::chrono::steady_clock::now,
        std::chrono::milliseconds tickInterval = std::chrono::milliseconds{16},
        FrameCachedPredicate frameCached = {}, QObject* parent = nullptr);

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
    // mid-run neither restarts nor re-seeks playback. Turning looping off while a run is already
    // past the end is not a case that exists: the run pauses the moment it reaches the last frame.
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
    [[nodiscard]] bool isFrameCached(const core::FrameTimeMapping& mapping,
                                     std::uint64_t frameIndex) const;

    CompositionSession& session_;
    CompositionPreviewController& previewController_;
    ClockFunction clock_;
    FrameCachedPredicate frameCached_;
    QTimer timer_;
    PlaybackState state_ = PlaybackState::Stopped;
    std::chrono::steady_clock::time_point startClock_{};
    std::uint64_t startFrameIndex_ = 0;
    // How many frames this run has advanced past startFrameIndex_. The uncached clock sets it to
    // whatever total elapsed time demands (so it can jump); the cached clock raises it by exactly
    // one.
    std::uint64_t appliedOffset_ = 0;
    std::optional<std::uint64_t> lastAppliedFrameIndex_;
    // Guards handleCurrentTimeChanged()'s scrub-during-playback detection: set around
    // PlaybackController's own session_.setCurrentTime() call so that signal is not mistaken for
    // an external scrub/direct-manipulation change while playing (see tick()'s only caller of
    // setCurrentTime() and handleCurrentTimeChanged()'s own comment).
    bool applyingOwnTimeChange_ = false;
    // True by default: every transport Bloom has shipped so far loops, and the footer's toggle is
    // what makes the other half of that statement reachable rather than a promise.
    bool looping_ = true;
};

} // namespace bloom::ui
