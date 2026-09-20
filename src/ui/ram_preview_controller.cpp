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
                                           PreviewPreparationFunction preparation, QObject* parent,
                                           PreviewPreparationSubmitter submitter)
    : QObject(parent), session_(session), previewController_(previewController),
      scheduler_(scheduler), taskUiBridge_(taskUiBridge), preparation_(std::move(preparation)),
      submitter_(std::move(submitter)) {
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
    // "Purge preview cache" ends any run: every frame it would still land belongs to the generation
    // the command just retired, and cancel() detaches them so none can repopulate the cleared
    // cache.
    connect(&previewController_, &CompositionPreviewController::previewCachePurged, this,
            &RamPreviewController::cancel);
}

RamPreviewController::~RamPreviewController() { cancelAndDetachActive(); }

void RamPreviewController::start() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (caching_ || shuttingDown_ || !preparation_ || previewController_.cachePurgeGateActive()) {
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
    advance();
}

void RamPreviewController::rebaseRange() {
    Q_ASSERT(QThread::currentThread() == thread());
    // A rebase re-scans from the first frame of the live range, so it is only valid once every
    // frame of the previous range has settled. The caller (advance) enforces that.
    Q_ASSERT(pipeline_.empty());
    rangeDirty_ = false;
    if (!caching_)
        return;
    // Each frame resolves its own genuine snapshot in requestFrames; the run no longer pins one
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
    // A range edit is render-neutral. Mark the range dirty and let advance() land whatever is in
    // flight; only when the pipeline is empty does it rebase and rescan, so an in-flight frame is
    // neither skipped nor submitted a second time.
    rangeDirty_ = true;
    advance();
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

void RamPreviewController::advance() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!caching_ || shuttingDown_) {
        return;
    }

    // Land every result that is already waiting first: draining is what lets a rebased range see an
    // empty pipeline and proceed, and it is where a stale/out-of-order result is refused.
    if (!drainResults()) {
        return;
    }

    if (rangeDirty_) {
        if (!pipeline_.empty()) {
            // Still preparing: let those frames land, then revisit the rebase on their completion.
            return;
        }
        rebaseRange();
        if (!caching_) {
            return;
        }
    }

    requestFrames();
    if (!caching_) {
        return;
    }

    // Finish on the truth that every frame the range holds has actually landed, never merely
    // because the last frame was submitted: a second frame may still be preparing.
    if (pipeline_.empty() && cachedFrameCount_ >= totalFrameCount_) {
        finish(true);
    }
}

void RamPreviewController::requestFrames() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!caching_ || shuttingDown_ || rangeDirty_) {
        return;
    }
    while (caching_ && pipeline_.hasCapacity()) {
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

        // Frames already cached are counted without being rendered: the second RAM preview of a
        // range nobody has edited is immediate, and a range partly filled by ordinary playback
        // finishes the rest. A frame still in flight is stepped over rather than resubmitted; the
        // pipeline's own identity check is the guard against a duplicate on a rebase.
        while (nextFrameIndex_ < totalFrameCount_) {
            const auto frameTime = mapping->timeForFrame(firstFrameIndex_ + nextFrameIndex_);
            if (!frameTime.hasValue()) {
                finish(false);
                return;
            }
            const auto key = previewController_.cacheKeyForTime(*frameTime.value());
            if (!key.has_value()) {
                finish(false);
                return;
            }
            if (pipeline_.isInFlight(*key)) {
                ++nextFrameIndex_;
                continue;
            }
            if (previewController_.frameCache().contains(*key)) {
                ++nextFrameIndex_;
                ++cachedFrameCount_;
                publishProgress();
                continue;
            }
            break;
        }
        if (nextFrameIndex_ >= totalFrameCount_) {
            return;
        }

        const auto frameTime = mapping->timeForFrame(firstFrameIndex_ + nextFrameIndex_);
        if (!frameTime.hasValue() || generation_ == std::numeric_limits<std::uint64_t>::max()) {
            finish(false);
            return;
        }
        // TEMPORAL-2B: each submitted time resolves its own genuine snapshot.
        const auto& snapshot = session_.evaluationSnapshotForTime(*frameTime.value());
        const auto desiredIdentity = identityFor(*frameTime.value(), snapshot);

        // Foreground, not Interactive: a RAM preview is a background fill the artist asked for, and
        // it must not outrank the preview frame they are looking at right now. No coalescing key
        // either -- each frame of the range is its own work, and a coalescing key would let the
        // scheduler cancel the OTHER in-flight frame of this very run.
        runtime::TaskRequest request("Cache RAM preview frame",
                                     {.kind = runtime::TaskOwnerKind::Composition,
                                      .id = runtime::TaskOwnerId::fromRaw(compositionId_.value())},
                                     runtime::TaskPriority::Foreground);
        request.sourceVersion = {
            .documentRevision = desiredIdentity.sourceRevision.value(),
            .requestGeneration = desiredIdentity.requestGeneration,
        };

        runtime::TaskSubmission<PreviewPreparationResultHandle> submission;
        if (submitter_) {
            submission = submitter_(std::move(request), snapshot, desiredIdentity,
                                    previewController_.settings().pixelStorageByteLimit, {});
        } else {
            submission = scheduler_.submit<PreviewPreparationResultHandle>(
                std::move(request),
                [snapshot, desiredIdentity,
                 pixelStorageByteLimit = previewController_.settings().pixelStorageByteLimit,
                 preparation = preparation_](runtime::TaskContext& context) mutable {
                    if (context.isCancellationRequested()) {
                        return runtime::TaskResult<PreviewPreparationResultHandle>::cancelled();
                    }
                    return preparation(snapshot, desiredIdentity, pixelStorageByteLimit, {},
                                       context);
                });
        }
        if (!submission.accepted()) {
            finish(false);
            return;
        }
        pipeline_.add(RamPreviewPipeline::InFlight{.frameIndex = nextFrameIndex_,
                                                   .identity = desiredIdentity,
                                                   .handle = std::move(submission.handle)});
        ++nextFrameIndex_;
        taskUiBridge_.wake();
    }
}

bool RamPreviewController::drainResults() {
    Q_ASSERT(QThread::currentThread() == thread());
    for (auto& ready : pipeline_.takeReady()) {
        if (!caching_ || shuttingDown_) {
            return false;
        }
        if (ready.result.state() != runtime::TaskState::Succeeded) {
            // Cancelled or failed: the run ends and keeps whatever already reached the cache. A
            // partially cached range is still useful -- the transport simply plays the rest on the
            // elapsed-time clock and says so in the footer.
            finish(false);
            return false;
        }
        const auto& value = ready.result.value();
        if (!value.has_value() || *value == nullptr ||
            (*value)->status() != runtime::PreviewPreparationStatus::Prepared ||
            (*value)->frame() == nullptr) {
            finish(false);
            return false;
        }
        const auto& frame = (*value)->frame();
        // Validate the out-of-order result against the CURRENT per-time evaluation snapshot and
        // frame key before it can be retained or counted. A frame whose time now resolves to a
        // different revision is a late result from an evaluation the session has left; the cache
        // would refuse it, and the run must not count it as progress. The cache still gets the
        // insert attempt so that a still-valid overlap survives the range rebase below.
        const auto identityKey = PreviewFrameCacheKey::forIdentity(frame->desiredIdentity());
        if (rangeDirty_ || !resultIsCurrent(identityKey, ready.identity)) {
            previewController_.frameCache().insert(frame);
            rangeDirty_ = true;
            continue;
        }
        previewController_.frameCache().insert(frame);
        ++cachedFrameCount_;
        publishProgress();
        if (previewController_.frameCache().statistics().evictions != evictionsAtStart_) {
            // The range has outgrown the memory budget. Every further frame would evict one this
            // run has already cached, so the run stops with the prefix that fits rather than
            // spending the rest of the range throwing away its own beginning. A sibling already in
            // flight (at most one more) is detached instead of being allowed to churn the prefix.
            pipeline_.cancelAllAndDetach();
            finish(true);
            return false;
        }
    }
    return true;
}

bool RamPreviewController::resultIsCurrent(const PreviewFrameCacheKey& identityKey,
                                           const runtime::PreviewRequestIdentity& identity) const {
    const auto currentKey = previewController_.cacheKeyForTime(identity.time);
    return currentKey.has_value() && *currentKey == identityKey;
}

runtime::PreviewRequestIdentity
RamPreviewController::identityFor(const core::RationalTime time,
                                  const document::Snapshot& snapshot) {
    const auto& settings = previewController_.settings();
    return {
        .projectId = snapshot.project().id(),
        .compositionId = compositionId_,
        .sourceRevision = snapshot.revision(),
        .requestGeneration = ++generation_,
        .time = time,
        .output = runtime::PreviewOutput::Composition,
        .resolution = previewController_.resolution(),
        .quality = settings.quality,
        .colorIntent = settings.colorIntent,
        .resolutionPolicy = settings.resolutionPolicy,
        .roi = previewController_.regionOfInterest(),
        .viewAdjust = {},
        .displayName = settings.displayName,
        .viewName = settings.viewName,
        .showLook = settings.showLook,
    };
}

void RamPreviewController::consumeReadyResult() {
    Q_ASSERT(QThread::currentThread() == thread());
    advance();
}

void RamPreviewController::finish(const bool completed) {
    pipeline_.cancelAllAndDetach();
    caching_ = false;
    rangeDirty_ = false;
    nextFrameIndex_ = 0;
    // The counts are KEPT: a surface (or a test) asking what the run that just ended achieved gets
    // the truth, and isCaching() is what says whether they are still moving. start() resets them.
    previewController_.endRamPreviewProgress();
    emit stateChanged();
    emit cachingFinished(completed);
}

void RamPreviewController::cancelAndDetachActive() noexcept { pipeline_.cancelAllAndDetach(); }

void RamPreviewController::publishProgress() {
    previewController_.setRamPreviewProgress(cachedFrameCount_);
    emit stateChanged();
}

} // namespace bloom::ui
