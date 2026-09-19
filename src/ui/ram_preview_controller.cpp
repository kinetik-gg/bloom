#include <bloom/ui/ram_preview_controller.hpp>

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/document/project.hpp>

#include <QThread>

#include <limits>
#include <utility>

namespace bloom::ui {
namespace {

// The same checked duration/frame-rate mapping the transport uses (playback_controller.cpp),
// refusing exactly the cases it refuses -- a zero or invalid duration has no frames to cache.
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

RamPreviewController::RamPreviewController(CompositionSession& session,
                                           CompositionPreviewController& previewController,
                                           runtime::TaskScheduler& scheduler,
                                           TaskUiBridge& taskUiBridge,
                                           PreviewPreparationFunction preparation, QObject* parent)
    : QObject(parent), session_(session), previewController_(previewController),
      scheduler_(scheduler), taskUiBridge_(taskUiBridge), preparation_(std::move(preparation)) {
    connect(&taskUiBridge_, &TaskUiBridge::snapshotsPolled, this,
            &RamPreviewController::consumeReadyResult);
    // A render-affecting edit or a composition switch makes every frame this run would still cache
    // belong to an evaluation the artist has left behind, so the run ends rather than mixing
    // revisions. A verified layout-only edit publishes no evaluationChanged, so the run keeps its
    // ONE retained evaluation snapshot and completes with reusable frames.
    connect(&session_, &CompositionSession::evaluationChanged, this, &RamPreviewController::cancel);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &RamPreviewController::cancel);
    // A render-neutral work-area edit adapts the run instead of ending it: expanding or shifting
    // fills only the missing frames, shrinking keeps the overlap and stops at the new end.
    connect(&session_, &CompositionSession::workAreaChanged, this,
            &RamPreviewController::handleWorkAreaChanged);
    // TEMPORAL-2B: a finite clip-range edit re-scopes which times are already valid. The run adapts
    // and rescans; retained frames hit, only the changed interval is submitted. A non-document
    // evaluation transition (display qualification/colour) still cancels, because the whole cache
    // is re-qualified.
    connect(&session_, &CompositionSession::documentEvaluationChanged, this,
            &RamPreviewController::handleDocumentEvaluationChanged);
    // A range must not mix factors or policies when the viewer changes resolution mid-run.
    connect(&previewController_, &CompositionPreviewController::resolutionChanged, this,
            &RamPreviewController::cancel);
}

RamPreviewController::~RamPreviewController() { cancelAndDetachActive(); }

void RamPreviewController::start() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (caching_ || shuttingDown_ || !preparation_) {
        return;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    const auto mapping = mappingForComposition(*composition);
    if (!mapping.has_value()) {
        return;
    }

    compositionId_ = session_.compositionId();
    caching_ = true;
    rangeDirty_ = false;
    rebaseRange();
    if (!caching_)
        return;
    evictionsAtStart_ = previewController_.frameCache().statistics().evictions;
    submitNextFrame();
}

void RamPreviewController::rebaseRange() {
    Q_ASSERT(QThread::currentThread() == thread());
    rangeDirty_ = false;
    if (!caching_)
        return;
    // Each frame resolves its own genuine snapshot in submitNextFrame; the run no longer pins one
    // snapshot, because a finite edit legitimately leaves several retained revisions along the
    // timeline.
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        finish(false);
        return;
    }
    const auto mapping = mappingForComposition(*composition);
    if (!mapping.has_value()) {
        finish(false);
        return;
    }
    const auto range = session_.workArea();
    const auto rate = composition->format().frameRate();
    const auto endMapping =
        core::FrameTimeMapping::create(range.end, rate.numerator(), rate.denominator());
    if (!endMapping) {
        finish(false);
        return;
    }
    firstFrameIndex_ = mapping->nearestFrameIndex(range.start);
    totalFrameCount_ = endMapping.value()->maximumFrameIndex() - firstFrameIndex_ + 1;
    nextFrameIndex_ = 0;
    cachedFrameCount_ = 0;
    // A range edit rebases the budget baseline too: the run's own range management must never look
    // like the eviction that ends a run.
    evictionsAtStart_ = previewController_.frameCache().statistics().evictions;
    previewController_.beginRamPreviewProgress(totalFrameCount_);
    emit stateChanged();
}

void RamPreviewController::handleWorkAreaChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!caching_)
        return;
    if (active_.has_value()) {
        // A frame is in flight for the old range. Let it land -- the cache refuses it if the new
        // range excludes it -- and rebase when it does, so no frame of the new range is skipped or
        // submitted twice.
        rangeDirty_ = true;
        return;
    }
    rebaseRange();
    if (caching_)
        submitNextFrame();
}

void RamPreviewController::handleDocumentEvaluationChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!caching_)
        return;
    const auto ranges = session_.evaluationSnapshotRanges();
    const auto* composition = session_.composition();
    const bool wholeRangeInvalidated =
        composition != nullptr && ranges.size() <= 1 &&
        (ranges.empty() || (ranges.front().start == core::RationalTime{} &&
                            ranges.front().end == composition->duration() &&
                            ranges.front().snapshot.revision() == session_.snapshot().revision()));
    if (wholeRangeInvalidated) {
        // Every cached frame belongs to an evaluation the artist left behind.
        cancel();
        return;
    }
    // A finite edit: adapt and rescan. Retained frames hit, only the changed interval is submitted.
    handleWorkAreaChanged();
}

void RamPreviewController::cancel() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!caching_) {
        return;
    }
    cancelAndDetachActive();
    finish(false);
}

void RamPreviewController::toggle() {
    if (caching_) {
        cancel();
    } else {
        start();
    }
}

void RamPreviewController::beginShutdown() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;
    disconnect(&session_, nullptr, this, nullptr);
    disconnect(&previewController_, nullptr, this, nullptr);
    cancelAndDetachActive();
    if (caching_) {
        finish(false);
    }
}

void RamPreviewController::submitNextFrame() {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(!active_.has_value());
    if (!caching_) {
        return;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        finish(false);
        return;
    }
    const auto mapping = mappingForComposition(*composition);
    if (!mapping.has_value()) {
        finish(false);
        return;
    }

    // Frames already cached are counted without being rendered: the second RAM preview of a range
    // nobody has edited is immediate, and a range partly filled by ordinary playback finishes the
    // rest.
    while (nextFrameIndex_ < totalFrameCount_) {
        const auto frameTime = mapping->timeForFrame(firstFrameIndex_ + nextFrameIndex_);
        if (!frameTime.hasValue()) {
            finish(false);
            return;
        }
        // TEMPORAL-2B: each submitted time resolves its own genuine snapshot.
        const auto& snapshot = session_.evaluationSnapshotForTime(*frameTime.value());
        const PreviewFrameCacheKey key{
            .projectId = snapshot.project().id(),
            .compositionId = compositionId_,
            .sourceRevision = snapshot.revision(),
            .time = *frameTime.value(),
            .output = runtime::PreviewOutput::Composition,
            .resolution = previewController_.resolution(),
            .quality = previewController_.settings().quality,
            .colorIntent = previewController_.settings().colorIntent,
            .resolutionPolicy = previewController_.settings().resolutionPolicy,
            .displayName = previewController_.settings().displayName,
            .viewName = previewController_.settings().viewName,
            .showLook = previewController_.settings().showLook,
        };
        if (!previewController_.frameCache().contains(key)) {
            break;
        }
        ++nextFrameIndex_;
        ++cachedFrameCount_;
        publishProgress();
    }
    if (nextFrameIndex_ >= totalFrameCount_) {
        finish(true);
        return;
    }

    const auto frameTime = mapping->timeForFrame(firstFrameIndex_ + nextFrameIndex_);
    if (!frameTime.hasValue() || generation_ == std::numeric_limits<std::uint64_t>::max()) {
        finish(false);
        return;
    }
    const auto& snapshot = session_.evaluationSnapshotForTime(*frameTime.value());
    const runtime::PreviewRequestIdentity desiredIdentity{
        .projectId = snapshot.project().id(),
        .compositionId = compositionId_,
        .sourceRevision = snapshot.revision(),
        .requestGeneration = ++generation_,
        .time = *frameTime.value(),
        .output = runtime::PreviewOutput::Composition,
        .resolution = previewController_.resolution(),
        .quality = previewController_.settings().quality,
        .colorIntent = previewController_.settings().colorIntent,
        .resolutionPolicy = previewController_.settings().resolutionPolicy,
        .displayName = previewController_.settings().displayName,
        .viewName = previewController_.settings().viewName,
        .showLook = previewController_.settings().showLook,
    };

    // Foreground, not Interactive: a RAM preview is a background fill the artist asked for, and it
    // must not outrank the preview frame they are looking at right now. No coalescing key either --
    // every frame of the range is its own work, and coalescing would discard frames the run needs.
    runtime::TaskRequest request("Cache RAM preview frame",
                                 {.kind = runtime::TaskOwnerKind::Composition,
                                  .id = runtime::TaskOwnerId::fromRaw(compositionId_.value())},
                                 runtime::TaskPriority::Foreground);
    request.sourceVersion = {
        .documentRevision = desiredIdentity.sourceRevision.value(),
        .requestGeneration = desiredIdentity.requestGeneration,
    };

    auto submission = scheduler_.submit<PreviewPreparationResultHandle>(
        std::move(request),
        [snapshot, desiredIdentity,
         pixelStorageByteLimit = previewController_.settings().pixelStorageByteLimit,
         preparation = preparation_](runtime::TaskContext& context) mutable {
            if (context.isCancellationRequested()) {
                return runtime::TaskResult<PreviewPreparationResultHandle>::cancelled();
            }
            return preparation(snapshot, desiredIdentity, pixelStorageByteLimit, {}, context);
        });
    if (!submission.accepted()) {
        finish(false);
        return;
    }
    active_.emplace(std::move(submission.handle));
    taskUiBridge_.wake();
}

void RamPreviewController::consumeReadyResult() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!active_.has_value()) {
        return;
    }
    auto result = active_->tryTakeResult();
    if (!result.has_value()) {
        return;
    }
    active_.reset();
    if (!caching_ || shuttingDown_) {
        return;
    }

    if (result->state() != runtime::TaskState::Succeeded) {
        // Cancelled or failed: the run ends and keeps whatever already reached the cache. A
        // partially cached range is still useful -- the transport simply plays the rest on the
        // elapsed-time clock and says so in the footer.
        finish(false);
        return;
    }
    const auto& value = result->value();
    if (!value.has_value() || *value == nullptr ||
        (*value)->status() != runtime::PreviewPreparationStatus::Prepared ||
        (*value)->frame() == nullptr) {
        finish(false);
        return;
    }
    // The cache refuses an out-of-range frame, so a completion for the old range can neither
    // resurrect a pruned entry nor corrupt the new range's progress: the rebase below rescans from
    // the new first frame and skips whatever survived.
    previewController_.frameCache().insert((*value)->frame());
    if (rangeDirty_) {
        rebaseRange();
        if (!caching_)
            return;
        submitNextFrame();
        return;
    }
    ++nextFrameIndex_;
    ++cachedFrameCount_;
    publishProgress();
    if (previewController_.frameCache().statistics().evictions != evictionsAtStart_) {
        // The range has outgrown the memory budget. Every further frame would evict one this run
        // has already cached, so the run stops with the prefix that fits rather than spending the
        // rest of the range throwing away its own beginning.
        finish(true);
        return;
    }
    submitNextFrame();
}

void RamPreviewController::finish(const bool completed) {
    caching_ = false;
    rangeDirty_ = false;
    nextFrameIndex_ = 0;
    // The counts are KEPT: a surface (or a test) asking what the run that just ended achieved gets
    // the truth, and isCaching() is what says whether they are still moving. start() resets them.
    previewController_.endRamPreviewProgress();
    emit stateChanged();
    emit cachingFinished(completed);
}

void RamPreviewController::cancelAndDetachActive() noexcept {
    if (!active_.has_value()) {
        return;
    }
    active_->cancel();
    active_.reset();
}

void RamPreviewController::publishProgress() {
    previewController_.setRamPreviewProgress(cachedFrameCount_);
    emit stateChanged();
}

} // namespace bloom::ui
