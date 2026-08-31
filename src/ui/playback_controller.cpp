#include <bloom/ui/playback_controller.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>

#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/document/project.hpp>

#include <QThread>

#include <limits>
#include <utility>

namespace bloom::ui {
namespace {

// The one place that turns the current composition's duration/frame rate into a checked
// bloom::core::FrameTimeMapping, refusing (std::nullopt) exactly the cases design decision 5's
// "playback of an empty/zero-duration composition is a no-op" covers, plus any other invalid
// rate/duration FrameTimeMapping::create() itself refuses. play() and tick() both call this rather
// than re-deriving it, so they cannot silently drift from each other's notion of valid.
[[nodiscard]] std::optional<core::FrameTimeMapping>
mappingForComposition(const document::Composition& composition) noexcept {
    const auto rate = composition.format().frameRate();
    const auto result = core::FrameTimeMapping::create(composition.duration(), rate.numerator(),
                                                       rate.denominator());
    if (!result.hasValue()) {
        return std::nullopt;
    }
    return *result.value();
}

} // namespace

PlaybackController::PlaybackController(CompositionSession& session,
                                       CompositionPreviewController& previewController,
                                       ClockFunction clock,
                                       const std::chrono::milliseconds tickInterval,
                                       QObject* parent)
    : QObject(parent), session_(session), previewController_(previewController),
      clock_(std::move(clock)) {
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(static_cast<int>(tickInterval.count()));
    connect(&timer_, &QTimer::timeout, this, &PlaybackController::tick);

    // Design decision 2: a composition switch or document rebind stops playback rather than
    // leaving it running against whatever composition happens to be live next.
    // CompositionSession::setComposition()/rebind() both emit compositionChanged() (verified by
    // reading composition_session.cpp), so this one connection covers both triggers.
    connect(&session_, &CompositionSession::compositionChanged, this,
            &PlaybackController::handleCompositionChanged);
    // Scrub-during-playback rule (design decision 3, reported per this task's "when done" list):
    // a scrub gesture PAUSES playback -- the simplest honest rule, chosen because playback and
    // scrub both drive CompositionSession::setCurrentTime() and TimelineRuler's own scrub gesture
    // has no way to know playback is running and coordinate with it; letting both write
    // concurrently would make the playhead fight the artist's drag every tick. currentTimeChanged()
    // fires for every setCurrentTime() call regardless of caller, so applyingOwnTimeChange_
    // distinguishes tick()'s own writes (ignored here) from every other source (scrub, direct
    // time entry, undo/redo) reaching the session while playing.
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            &PlaybackController::handleCurrentTimeChanged);
}

PlaybackState PlaybackController::state() const noexcept { return state_; }

void PlaybackController::play() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_ == PlaybackState::Playing) {
        return;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    const auto mapping = mappingForComposition(*composition);
    if (!mapping.has_value()) {
        // Covers the zero/invalid-duration no-op (design decision 5) and any other rate/duration
        // FrameTimeMapping itself refuses.
        return;
    }

    // A later play() after a pause resumes from the CURRENT session time, never the original
    // start (design decision 2) -- read fresh here every time, not cached from a prior play().
    startClock_ = clock_();
    startFrameIndex_ = mapping->nearestFrameIndex(session_.currentTime());
    lastAppliedFrameIndex_ = startFrameIndex_;

    state_ = PlaybackState::Playing;
    // Same arming CompositionPreviewController::beginInteractiveScrub()/notifyScrubEnded()
    // TimelineRuler's own scrub gesture and the Viewer's position-drag gesture already use (see
    // timeline_ruler.cpp's mousePressEvent/mouseReleaseEvent and viewer_editor.cpp's endDrag()):
    // read directly in composition_preview_controller.cpp, handleCurrentTimeChanged() submits at
    // Interactive priority precisely while this flag is armed, and this is the one place that
    // grants that priority to session-time changes -- no new request kind needed.
    previewController_.beginInteractiveScrub();
    timer_.start();
    emit stateChanged(state_);
}

void PlaybackController::pause() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_ != PlaybackState::Playing) {
        return;
    }
    state_ = PlaybackState::Stopped;
    timer_.stop();
    // Bypasses any remaining trailing-cadence delay for the last applied time, mirroring
    // TimelineRuler::mouseReleaseEvent()/ViewerEditor::endDrag()'s own notifyScrubEnded() call on
    // gesture end.
    previewController_.notifyScrubEnded();
    emit stateChanged(state_);
}

void PlaybackController::toggle() {
    if (state_ == PlaybackState::Playing) {
        pause();
    } else {
        play();
    }
}

void PlaybackController::tick() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_ != PlaybackState::Playing) {
        return;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        pause();
        return;
    }
    const auto mapping = mappingForComposition(*composition);
    if (!mapping.has_value()) {
        pause();
        return;
    }

    const auto now = clock_();
    if (now < startClock_) {
        // A non-monotonic injected clock (only reachable from a test double) -- no-op rather than
        // treat a negative duration as unsigned wraparound.
        return;
    }
    const auto elapsed = now - startClock_;
    const auto elapsedNanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());

    // Real-time, drop-frames-never-slow policy: the target frame is derived from TOTAL elapsed
    // time since play()'s fixed start every tick (never `lastFrame + 1`, never an accumulated
    // running clock), so a slow tick jumps straight to the frame elapsed time now demands instead
    // of catching up frame-by-frame.
    const auto frameOffset = mapping->frameOffsetForElapsedNanoseconds(elapsedNanoseconds);
    if (!frameOffset.has_value()) {
        // Checked-arithmetic overflow guard (unreachable for any realistic elapsed duration and
        // frame rate) -- no-op rather than wrap.
        return;
    }

    const auto frameCount = mapping->maximumFrameIndex() + 1;
    if (*frameOffset > std::numeric_limits<std::uint64_t>::max() - startFrameIndex_) {
        // Same checked-overflow discipline applied to the addition below.
        return;
    }
    // Looping (design decision 2): wraps within [0, duration) via exact modulo of the frame count.
    // Recomputed fresh from the fixed start/elapsed every tick -- never an accumulated running
    // index -- so repeated wraps never drift.
    const auto targetFrameIndex = (startFrameIndex_ + *frameOffset) % frameCount;

    if (lastAppliedFrameIndex_.has_value() && *lastAppliedFrameIndex_ == targetFrameIndex) {
        return;
    }
    const auto targetTime = mapping->timeForFrame(targetFrameIndex);
    if (!targetTime.hasValue()) {
        return;
    }

    lastAppliedFrameIndex_ = targetFrameIndex;
    applyingOwnTimeChange_ = true;
    (void)session_.setCurrentTime(*targetTime.value());
    applyingOwnTimeChange_ = false;
}

void PlaybackController::handleCompositionChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    pause();
}

void PlaybackController::handleCurrentTimeChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_ == PlaybackState::Playing && !applyingOwnTimeChange_) {
        pause();
    }
}

} // namespace bloom::ui
