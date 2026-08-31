#pragma once

#include <QObject>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

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

// Composes with CompositionSession and CompositionPreviewController without modifying either
// (docs/architecture/animation-and-time.md, "Session Time And Scrubbing"; this task's frozen
// design). Advances session time through the SAME CompositionSession::setCurrentTime() mutator a
// scrub gesture uses, and arms/disarms the preview controller's existing Interactive-cadence gate
// through its existing beginInteractiveScrub()/notifyScrubEnded() seam
// (CompositionPreviewController:: handleCurrentTimeChanged() already grants Interactive priority to
// any current-time change made while that arming flag is set -- verified by reading
// composition_preview_controller.cpp; no new request kind was added). The
// one-active/one-newest-pending gate, cadence, and stale-result rejection inside
// CompositionPreviewController are never touched.
//
// Real-time, drop-frames-never-slow policy: every tick recomputes the target frame from the TOTAL
// elapsed time since play() captured its start clock/frame (never `lastFrame + 1` and never an
// accumulated running clock), so a slow evaluation causes the next tick to skip straight to
// whatever frame elapsed time now demands rather than slowing played-back motion down. Frame
// arithmetic is exact and checked throughout: index -> time uses
// bloom::core::FrameTimeMapping::timeForFrame() (exact rational multiplication, no rounding), and
// elapsed-time -> frame-offset uses the new FrameTimeMapping::frameOffsetForElapsedNanoseconds()
// (issue #105; checked multiword arithmetic, floors rather than rounds -- see its own
// documentation for why playback needs floor semantics where scrub's nearestFrameIndex() rounds to
// nearest). No floating-point time accumulates across ticks.
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

    [[nodiscard]] PlaybackState state() const noexcept;

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

    // Advances playback by one tick using the injected clock's current reading. Production wires
    // the internal ~16 ms QTimer's timeout() here (mirroring CompositionPreviewController's
    // injectable-cadence constructor seam); tests call this directly after advancing the injected
    // clock, with no dependency on a real event loop or real elapsed wall time -- the timer is
    // still constructed and started in production but a test that never pumps Qt's event loop
    // never observes it fire, so manual tick() calls stay fully deterministic.
    void tick();

  signals:
    void stateChanged(PlaybackState state);

  private:
    void handleCompositionChanged();
    void handleCurrentTimeChanged();

    CompositionSession& session_;
    CompositionPreviewController& previewController_;
    ClockFunction clock_;
    QTimer timer_;
    PlaybackState state_ = PlaybackState::Stopped;
    std::chrono::steady_clock::time_point startClock_{};
    std::uint64_t startFrameIndex_ = 0;
    std::optional<std::uint64_t> lastAppliedFrameIndex_;
    // Guards handleCurrentTimeChanged()'s scrub-during-playback detection: set around
    // PlaybackController's own session_.setCurrentTime() call so that signal is not mistaken for
    // an external scrub/direct-manipulation change while playing (see tick()'s only caller of
    // setCurrentTime() and handleCurrentTimeChanged()'s own comment).
    bool applyingOwnTimeChange_ = false;
};

} // namespace bloom::ui
