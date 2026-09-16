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
    // A document edit or a composition switch makes every frame this run would still cache belong
    // to a document that is no longer live, so the run ends rather than caching frames of a
    // revision the artist has already left behind.
    connect(&session_, &CompositionSession::snapshotChanged, this, &RamPreviewController::cancel);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &RamPreviewController::cancel);
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

    snapshot_ = session_.snapshot();
    compositionId_ = session_.compositionId();
    const auto range = session_.workArea();
    const auto rate = composition->format().frameRate();
    const auto endMapping =
        core::FrameTimeMapping::create(range.end, rate.numerator(), rate.denominator());
    if (!endMapping)
        return;
    firstFrameIndex_ = mapping->nearestFrameIndex(range.start);
    totalFrameCount_ = endMapping.value()->maximumFrameIndex() - firstFrameIndex_ + 1;
    nextFrameIndex_ = 0;
    cachedFrameCount_ = 0;
    evictionsAtStart_ = previewController_.frameCache().statistics().evictions;
    caching_ = true;
    previewController_.beginRamPreviewProgress(totalFrameCount_);
    emit stateChanged();
    submitNextFrame();
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
    if (!caching_ || !snapshot_.has_value()) {
        return;
    }
    // Copied once, rather than dereferenced through the optional below: a Snapshot is a revision
    // plus a shared identity handle, and every path out of here that calls finish() clears
    // snapshot_.
    const document::Snapshot snapshot = *snapshot_;
    const auto* composition = snapshot.project().findComposition(compositionId_);
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
    previewController_.frameCache().insert((*value)->frame());
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
    snapshot_.reset();
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
