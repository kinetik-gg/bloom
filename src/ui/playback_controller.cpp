#include <bloom/ui/playback_controller.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/audio_device_control.hpp>

#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/document/project.hpp>

#include <QAction>
#include <QKeySequence>
#include <QThread>
#include <QWidget>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
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

// An authored endpoint can become off-grid after a rate change. The first admitted frame must
// lie at or after that endpoint, even when nearest rounding would choose the preceding frame.
[[nodiscard]] std::optional<std::uint64_t>
firstFrameAtOrAfter(const core::FrameTimeMapping& mapping,
                    const core::RationalTime start) noexcept {
    const auto floor = mapping.frameOffsetForTime(start);
    if (!floor)
        return std::nullopt;
    const auto floorTime = mapping.timeForFrame(*floor);
    if (!floorTime)
        return std::nullopt;
    if (*floorTime.value() >= start)
        return floor;
    if (*floor == std::numeric_limits<std::uint64_t>::max())
        return std::nullopt;
    return *floor + 1;
}

// Refuse invalid injected extremes BEFORE signed subtraction or duration conversion.
[[nodiscard]] std::optional<std::uint64_t>
elapsedNanoseconds(const std::chrono::steady_clock::time_point now,
                   const std::chrono::steady_clock::time_point start) noexcept {
    using Clock = std::chrono::steady_clock;
    using Rep = Clock::duration::rep;
    if (now < start)
        return std::nullopt;
    const auto current = now.time_since_epoch().count();
    const auto origin = start.time_since_epoch().count();
    if constexpr (std::is_signed_v<Rep>) {
        if (origin < 0 && current > std::numeric_limits<Rep>::max() + origin)
            return std::nullopt;
    }
    const Clock::duration elapsed{current - origin};
    if constexpr (!std::ratio_less_v<Clock::period, std::nano>) {
        if (elapsed > std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds::max()))
            return std::nullopt;
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

} // namespace

PlaybackController::PlaybackController(CompositionSession& session,
                                       CompositionPreviewController& previewController,
                                       ClockFunction clock,
                                       const std::chrono::milliseconds tickInterval,
                                       QObject* parent)
    : QObject(parent), session_(session), previewController_(previewController),
      clock_(std::move(clock)) {
    if (!clock_)
        throw std::invalid_argument("Playback clock must be callable");
    if (tickInterval <= std::chrono::milliseconds::zero())
        throw std::invalid_argument("Playback wake interval must be positive");
    wakeCap_ = std::min(tickInterval, std::chrono::milliseconds{16});
    timer_.setSingleShot(true);
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(static_cast<int>(wakeCap_.count()));
    connect(&timer_, &QTimer::timeout, this, &PlaybackController::tick);
    connect(&previewController_, &CompositionPreviewController::interactiveScrubStarted, this,
            &PlaybackController::pause);

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
    connect(&session_, &CompositionSession::workAreaChanged, this,
            &PlaybackController::handleWorkAreaChanged);
    connect(&session_, &CompositionSession::snapshotChanged, this,
            &PlaybackController::handleSnapshotChanged);
}

PlaybackController::~PlaybackController() {
    pause();
    if (audioControl_)
        audioControl_->setCompletionHandler({});
}

void PlaybackController::installWindowShortcut(QWidget& window) {
    auto* action = new QAction(tr("Play/Pause"), &window);
    action->setObjectName(QStringLiteral("playPauseAction"));
    action->setShortcut(QKeySequence(Qt::Key_Space));
    action->setShortcutContext(Qt::WindowShortcut);
    action->setAutoRepeat(false);
    window.addAction(action);
    connect(action, &QAction::triggered, this, &PlaybackController::toggle);
}

void PlaybackController::setAudioDeviceControl(AudioDeviceControl* control) {
    Q_ASSERT(QThread::currentThread() == thread());
    pause();
    if (audioControl_)
        audioControl_->setCompletionHandler({});
    audioControl_ = control;
    if (audioControl_)
        audioControl_->setCompletionHandler(
            [this](const std::uint64_t generation, const bool playing,
                   const bool transportStarted) {
                handleAudioCompletion(generation, playing, transportStarted);
            });
    audioClockActive_ = false;
    audioStartPending_ = false;
}

void PlaybackController::useWallClock() {
    const bool wasAudioMaster = audioClockActive_;
    audioClockActive_ = false;
    // play() and work-area changes already establish the wall origin. A delayed stop/failure or
    // repeated empty mix while wall is master must not throw away elapsed time on every callback.
    if (!wasAudioMaster || state_ != PlaybackState::Playing)
        return;
    const auto* composition = session_.composition();
    const auto mapping = composition ? mappingForComposition(*composition) : std::nullopt;
    if (!mapping) {
        pause();
        return;
    }
    startClock_ = clock_();
    startFrameIndex_ = mapping->nearestFrameIndex(session_.currentTime());
    appliedOffset_ = 0;
    lastAppliedFrameIndex_ = startFrameIndex_;
    scheduleTick(wakeCap_);
}

void PlaybackController::setAudioEnabled(const bool enabled) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (audioEnabled_ == enabled) {
        return;
    }
    audioEnabled_ = enabled;
    if (!audioEnabled_ && audioControl_) {
        expectedAudioGeneration_ = audioControl_->requestStop();
        audioStartPending_ = false;
        useWallClock();
    }
}

void PlaybackController::setAudioMix(runtime::AudioMixDescription description,
                                     std::vector<media::audio::playback::AudioClip> clips) {
    Q_ASSERT(QThread::currentThread() == thread());
    const bool hadClips = audioClipsAvailable_;
    audioClipsAvailable_ = !clips.empty();
    audioMix_ = std::move(description);
    if (audioControl_) {
        expectedAudioGeneration_ = audioControl_->replaceClips(std::move(clips));
        if (!audioClipsAvailable_) {
            expectedAudioGeneration_ = audioControl_->requestStop();
            audioStartPending_ = false;
            useWallClock();
        } else if (!hadClips && state_ == PlaybackState::Playing && audioEnabled_) {
            audioAlignmentRetried_ = false;
            audioStartPending_ = true;
            expectedAudioGeneration_ = audioControl_->requestSeek(session_.currentTime());
        }
    }
}

PlaybackState PlaybackController::state() const noexcept { return state_; }

bool PlaybackController::isLooping() const noexcept { return looping_; }

void PlaybackController::setLooping(const bool looping) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (looping_ == looping) {
        return;
    }
    looping_ = looping;
    emit loopingChanged(looping_);
}

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
    const auto range = session_.workArea();
    const auto firstIndex = firstFrameAtOrAfter(*mapping, range.start);
    if (!firstIndex)
        return;
    const auto first = *firstIndex;
    const auto endMapping =
        core::FrameTimeMapping::create(range.end, composition->format().frameRate().numerator(),
                                       composition->format().frameRate().denominator());
    if (!endMapping)
        return;
    const auto last = endMapping.value()->maximumFrameIndex();
    if (last < first || last - first == std::numeric_limits<std::uint64_t>::max())
        return;
    startFrameIndex_ = std::clamp(mapping->nearestFrameIndex(session_.currentTime()), first, last);
    if (session_.currentTime() < range.start || session_.currentTime() >= range.end) {
        startFrameIndex_ = first;
        if (const auto time = mapping->timeForFrame(first); time)
            (void)session_.setCurrentTime(*time.value());
    }
    appliedOffset_ = 0;
    lastAppliedFrameIndex_ = startFrameIndex_;
    runRateNumerator_ = mapping->rateNumerator();
    runRateDenominator_ = mapping->rateDenominator();
    runRangeStart_ = range.start;
    runRangeEnd_ = range.end;

    audioClockActive_ = false;
    audioStartPending_ = false;
    if (audioEnabled_ && audioControl_ && audioClipsAvailable_) {
        const auto anchor = mapping->timeForFrame(startFrameIndex_);
        if (!anchor)
            return;
        // Match the established nearest-grid wall origin while preserving immediate session time.
        audioAlignmentRetried_ = false;
        audioStartPending_ = true;
        expectedAudioGeneration_ = audioControl_->requestPlay(*anchor.value());
    }

    state_ = PlaybackState::Playing;
    previewController_.setPlaybackActive(true);
    previewController_.beginDroppedFrameCounting();
    timer_.start();
    emit stateChanged(state_);
}

void PlaybackController::pause() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_ != PlaybackState::Playing) {
        return;
    }
    state_ = PlaybackState::Stopped;
    audioStartPending_ = false;
    timer_.stop();
    if (audioControl_)
        expectedAudioGeneration_ = audioControl_->requestStop();
    audioClockActive_ = false;
    previewController_.setPlaybackActive(false);
    previewController_.endDroppedFrameCounting();
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

    if (mapping->rateNumerator() != runRateNumerator_ ||
        mapping->rateDenominator() != runRateDenominator_) {
        handleWorkAreaChanged();
        return;
    }
    // Every early refusal still rearms a bounded positive wake; no timer spin or missed restart.
    scheduleTick(wakeCap_);
    std::optional<std::uint64_t> frameOffset;
    std::optional<std::uint64_t> untilNextFrame;
    if (audioClockActive_ && audioControl_) {
        const auto position = audioControl_->positionNow();
        const auto audioFrame = mapping->frameOffsetForTime(position);
        if (!audioFrame || *audioFrame < startFrameIndex_) {
            return;
        }
        frameOffset = *audioFrame - startFrameIndex_;
        untilNextFrame =
            mapping->nanosecondsUntilNextFrame(position, audioControl_->playbackRateRatio());
    } else {
        const auto now = clock_();
        const auto elapsed = elapsedNanoseconds(now, startClock_);
        if (!elapsed)
            return;
        const auto elapsedCount = *elapsed;

        // Real-time, drop-frames-never-slow policy: the target frame is derived from TOTAL elapsed
        // time since play()'s fixed start every tick (never `lastFrame + 1`, never an accumulated
        // running clock), so a slow tick jumps straight to the frame elapsed time now demands
        // instead of catching up frame-by-frame.
        frameOffset = mapping->frameOffsetForElapsedNanoseconds(elapsedCount);
        const auto elapsedTime =
            core::RationalTime::create(static_cast<std::int64_t>(elapsedCount), 1'000'000'000);
        if (elapsedTime)
            untilNextFrame = mapping->nanosecondsUntilNextFrame(*elapsedTime);
    }
    if (!frameOffset || !untilNextFrame ||
        *untilNextFrame > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        // Checked-arithmetic overflow guard (unreachable for any realistic elapsed duration and
        // frame rate) -- no-op rather than wrap.
        return;
    }

    const auto range = session_.workArea();
    const auto firstIndex = firstFrameAtOrAfter(*mapping, range.start);
    if (!firstIndex) {
        pause();
        return;
    }
    const auto first = *firstIndex;
    const auto rate = composition->format().frameRate();
    const auto endMapping =
        core::FrameTimeMapping::create(range.end, rate.numerator(), rate.denominator());
    if (!endMapping)
        return;
    const auto last = endMapping.value()->maximumFrameIndex();
    if (last < first || last - first == std::numeric_limits<std::uint64_t>::max() ||
        startFrameIndex_ < first || startFrameIndex_ > last) {
        handleWorkAreaChanged();
        return;
    }
    const auto frameCount = last - first + 1;
    const auto frameBudget = std::chrono::nanoseconds{static_cast<std::int64_t>(*untilNextFrame)};
    scheduleTick(frameBudget);
    if (*frameOffset <= appliedOffset_) {
        // The frame this run is already showing is still the one elapsed time asks for.
        return;
    }
    const auto nextOffset = *frameOffset;
    if (nextOffset > std::numeric_limits<std::uint64_t>::max() - startFrameIndex_) {
        // Same checked-overflow discipline applied to the addition below.
        return;
    }
    // Looping (design decision 2): wraps within [0, duration) via exact modulo of the frame count.
    // The offset is exact in either master clock, so repeated wraps
    // never drift.
    //
    // Task VIEW-1 made that wrap conditional rather than unconditional. With looping off, the run
    // lands on the LAST frame of the work area exactly once and pauses there: it neither wraps nor
    // freezes still-playing on a frame it already showed, and the offset arithmetic below is the
    // same exact integer arithmetic either way -- only the reduction differs.
    const auto rawOffset = startFrameIndex_ - first + nextOffset;
    const bool pastEnd = !looping_ && rawOffset >= frameCount;
    const auto targetFrameIndex =
        looping_ ? first + rawOffset % frameCount : first + std::min(rawOffset, frameCount - 1);
    previewController_.noteDroppedFrames(nextOffset - appliedOffset_ - 1);
    appliedOffset_ = nextOffset;

    const auto targetTime = mapping->timeForFrame(targetFrameIndex);
    if (!targetTime.hasValue()) {
        return;
    }

    lastAppliedFrameIndex_ = targetFrameIndex;
    applyingOwnTimeChange_ = true;
    (void)session_.setCurrentTime(*targetTime.value());
    applyingOwnTimeChange_ = false;
    previewController_.presentPlaybackFrame(frameBudget);
    if (pastEnd) {
        // After the frame is applied and presented, so the artist is left looking at the last
        // frame of the range rather than at whatever the run showed one tick earlier.
        pause();
    }
}

void PlaybackController::scheduleTick(const std::chrono::nanoseconds untilNextFrame) {
    if (state_ != PlaybackState::Playing)
        return;
    const auto positive = std::max(untilNextFrame, std::chrono::nanoseconds{1});
    const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(positive);
    timer_.start(static_cast<int>(std::min(milliseconds, wakeCap_).count()));
}

void PlaybackController::handleWorkAreaChanged() {
    if (state_ != PlaybackState::Playing)
        return;
    const auto* composition = session_.composition();
    const auto mapping = composition ? mappingForComposition(*composition) : std::nullopt;
    if (!mapping) {
        pause();
        return;
    }
    const auto area = session_.workArea();
    const auto firstIndex = firstFrameAtOrAfter(*mapping, area.start);
    if (!firstIndex) {
        pause();
        return;
    }
    const auto first = *firstIndex;
    const auto end = core::FrameTimeMapping::create(area.end, mapping->rateNumerator(),
                                                    mapping->rateDenominator());
    if (!end || end.value()->maximumFrameIndex() < first ||
        end.value()->maximumFrameIndex() - first == std::numeric_limits<std::uint64_t>::max()) {
        pause();
        return;
    }
    const auto index =
        std::clamp(mapping->nearestFrameIndex(session_.currentTime() < area.start ||
                                                      session_.currentTime() >= area.end
                                                  ? area.start
                                                  : session_.currentTime()),
                   first, end.value()->maximumFrameIndex());
    const auto anchor = mapping->timeForFrame(index);
    if (!anchor) {
        pause();
        return;
    }
    if (runRateNumerator_ == mapping->rateNumerator() &&
        runRateDenominator_ == mapping->rateDenominator() && runRangeStart_ == area.start &&
        runRangeEnd_ == area.end)
        return;
    runRateNumerator_ = mapping->rateNumerator();
    runRateDenominator_ = mapping->rateDenominator();
    runRangeStart_ = area.start;
    runRangeEnd_ = area.end;
    startClock_ = clock_();
    startFrameIndex_ = index;
    appliedOffset_ = 0;
    lastAppliedFrameIndex_ = index;
    const bool moved = session_.currentTime() != *anchor.value();
    applyingOwnTimeChange_ = true;
    (void)session_.setCurrentTime(*anchor.value());
    applyingOwnTimeChange_ = false;
    if (audioEnabled_ && audioControl_ && audioClipsAvailable_) {
        useWallClock();
        audioAlignmentRetried_ = false;
        audioStartPending_ = true;
        expectedAudioGeneration_ = audioControl_->requestSeek(*anchor.value());
    }
    const auto period =
        audioEnabled_ && audioControl_ && audioClipsAvailable_
            ? mapping->nanosecondsUntilNextFrame(*anchor.value(), audioControl_->playbackRateRatio())
            : mapping->nanosecondsUntilNextFrame(core::RationalTime{});
    if (!period || *period > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        pause();
        return;
    }
    const auto budget = std::chrono::nanoseconds{static_cast<std::int64_t>(*period)};
    scheduleTick(budget);
    if (moved)
        previewController_.presentPlaybackFrame(budget);
}

void PlaybackController::handleSnapshotChanged() {
    const auto* composition = session_.composition();
    if (state_ == PlaybackState::Playing && composition &&
        (composition->format().frameRate().numerator() != runRateNumerator_ ||
         composition->format().frameRate().denominator() != runRateDenominator_))
        handleWorkAreaChanged();
}

void PlaybackController::handleCompositionChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    pause();
    audioMix_.reset();
    audioClipsAvailable_ = false;
    if (audioControl_)
        (void)audioControl_->replaceClips({});
}

void PlaybackController::handleCurrentTimeChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_ == PlaybackState::Playing && !applyingOwnTimeChange_) {
        pause();
    }
}

void PlaybackController::handleAudioCompletion(const std::uint64_t generation, const bool playing,
                                               const bool transportStarted) {
    if (generation != expectedAudioGeneration_ || state_ != PlaybackState::Playing ||
        !audioEnabled_ || !audioControl_)
        return;
    if (!playing) {
        audioStartPending_ = false;
        useWallClock();
        return;
    }
    // A mix-only update during a live run must not compare the unwrapped device clock with the
    // loop-wrapped session time. It is only a start handoff when an earlier start was still pending.
    if (!transportStarted && !audioStartPending_)
        return;
    if (!audioStartPending_)
        return;
    const auto* composition = session_.composition();
    const auto mapping = composition ? mappingForComposition(*composition) : std::nullopt;
    if (!mapping)
        return;
    const auto deviceFrame = mapping->frameOffsetForTime(audioControl_->positionNow());
    const auto currentFrame = mapping->nearestFrameIndex(session_.currentTime());
    if (!deviceFrame || *deviceFrame < currentFrame ||
        (currentFrame != std::numeric_limits<std::uint64_t>::max() &&
         *deviceFrame > currentFrame + 1)) {
        // A delayed device start must not pull the already advancing visual clock backward.
        useWallClock();
        if (!audioAlignmentRetried_) {
            audioAlignmentRetried_ = true;
            audioStartPending_ = true;
            expectedAudioGeneration_ = audioControl_->requestSeek(session_.currentTime());
        } else {
            audioStartPending_ = false;
            expectedAudioGeneration_ = audioControl_->requestStop();
        }
        return;
    }
    audioClockActive_ = true;
    audioStartPending_ = false;
    startFrameIndex_ = currentFrame;
    appliedOffset_ = 0;
    lastAppliedFrameIndex_ = currentFrame;
    scheduleTick(wakeCap_);
}

} // namespace bloom::ui
