#pragma once

#include <bloom/core/rational_time.hpp>

#include <QObject>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

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
// TWO clocks, and which one a tick uses depends on one honest question: is the next frame already
// in the RAM preview cache?
//
// NOT CACHED -- the original real-time, drop-frames-never-slow policy, unchanged. Every tick
// recomputes the target frame from the TOTAL elapsed time since play() captured its start
// clock/frame, so a slow evaluation causes the next tick to skip straight to whatever frame elapsed
// time now demands rather than slowing played-back motion down, and the Viewer footer reports what
// the preview path dropped.
//
// CACHED -- the frame-accurate clock (task PERF1, item 4). A cached frame costs a lookup, so there
// is nothing to drop and skipping one would be a lie about what the composition does: the target
// therefore advances by exactly ONE frame, and the due moment is still computed from the total
// elapsed time since the same fixed start, so presentations track the ideal frame grid instead of
// accumulating a per-tick rounding error. The one consequence worth stating plainly: if the host
// stalls long enough to owe several frames, the transport plays every one of them -- at one frame
// per tick until the debt is paid -- rather than skipping to the frame the wall clock now demands.
//
// Frame
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
};

} // namespace bloom::ui
