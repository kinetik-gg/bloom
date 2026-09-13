#include <QThread>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <utility>

namespace bloom::ui {
namespace {
QString firstDiagnosticSummary(const std::vector<runtime::TaskDiagnostic>& diagnostics,
                               QString fallback) {
    if (diagnostics.empty() || diagnostics.front().summary.empty()) {
        return fallback;
    }
    return QString::fromStdString(diagnostics.front().summary);
}

} // namespace

void CompositionPreviewController::consumeReadyResult() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!active_.has_value()) {
        return;
    }

    auto result = active_->handle.tryTakeResult();
    if (!result.has_value()) {
        return;
    }

    ActiveRequest completed = std::move(*active_);
    active_.reset();
    if (shuttingDown_) {
        return;
    }
    const auto deliveredAt = std::chrono::steady_clock::now();
    const auto& prepared = result->value();
    if (result->state() == runtime::TaskState::Succeeded && prepared.has_value() &&
        *prepared != nullptr && (*prepared)->frame() != nullptr &&
        (*prepared)->frame()->desiredIdentity() == completed.desiredIdentity &&
        !completed.carriedInteractionOverride) {
        recordPreparationDuration(completed.desiredIdentity, deliveredAt - completed.submittedAt);
    }
    if (completed.playbackDeadline.has_value() &&
        (deliveredAt > *completed.playbackDeadline || !isCurrent(completed))) {
        if (completed.playbackOutstanding) {
            noteDroppedFrame();
        }
        // Late frames can still serve a later loop, but must never replace the current picture.
        const auto key = cacheKeyForTime(completed.desiredIdentity.time);
        if (key.has_value() &&
            *key == PreviewFrameCacheKey::forIdentity(completed.desiredIdentity) &&
            prepared.has_value() && *prepared != nullptr && (*prepared)->frame() != nullptr &&
            (*prepared)->frame()->desiredIdentity() == completed.desiredIdentity) {
            frameCache_->insert((*prepared)->frame());
        }
        if (isCurrent(completed)) {
            auto skipped = state_;
            skipped.activity = PreviewActivity::Cancelled;
            skipped.taskId.reset();
            skipped.message = tr("Playback skipped this frame; showing the previous frame");
            publish(std::move(skipped));
        }
        if (!pending_.has_value()) {
            return;
        }
    }
    if (pending_.has_value()) {
        PendingRequest pendingRequest = std::move(*pending_);
        pending_.reset();
        // The completed frame is discarded unpublished in favour of the newer pending request: the
        // work was done and the artist never saw it, which is exactly a dropped frame. Counted
        // after the pending request is taken, so the optional is provably disengaged across the
        // call.
        if (!completed.playbackDeadline.has_value()) {
            noteDroppedFrame();
        }
        submitPreview(std::move(pendingRequest), state_.frame);
        return;
    }
    if (!isCurrent(completed)) {
        return;
    }
    if (!liveSessionMatches(completed.desiredIdentity)) {
        requestPreview(session_.compositionId() != completed.desiredIdentity.compositionId,
                       PreviewRequestKind::Visible);
        return;
    }

    CompositionPreviewState next{
        .activity = PreviewActivity::Failed,
        .freshness = FrameFreshness::None,
        .desiredIdentity = completed.desiredIdentity,
        .taskId = completed.handle.id(),
        .frame = state_.frame,
        .diagnostics = result->diagnostics(),
        .message = {},
    };

    switch (result->state()) {
    case runtime::TaskState::Succeeded: {
        const auto& value = result->value();
        if (!value.has_value() || *value == nullptr) {
            next.message = tr("Preview rendering returned no result");
            break;
        }
        const auto& preparation = **value;
        switch (preparation.status()) {
        case runtime::PreviewPreparationStatus::Prepared: {
            const auto& frame = preparation.frame();
            if (frame == nullptr) {
                next.message = tr("Preview rendering returned no prepared frame");
                break;
            }
            if (frame->desiredIdentity() != completed.desiredIdentity) {
                next.message = tr("Preview rendering returned pixels for a different request");
                break;
            }
            // Alternative-agnostic (issue #97, task C3): frame may carry either the reference or
            // the qualified display product (PreparedPreviewFrame's closed alternative), and
            // displayBufferView() normalizes both to the same validity/shape check rather than
            // assuming the reference-only displayBuffer() accessor.
            if (!frame->displayBufferView().has_value()) {
                next.message = tr("Preview rendering returned an invalid display buffer");
                break;
            }
            next.activity = PreviewActivity::Ready;
            next.freshness = FrameFreshness::Current;
            next.frame = frame;
            next.message = tr("The current composition frame is ready");
            // Playing without a cache keeps today's behavior but fills the cache as it goes, so the
            // second pass over the same range is a sequence of lookups (task PERF1, item 3). A
            // frame rendered under an interactive override is the exception: its pixels belong to a
            // gesture, and its identity cannot say so.
            if (!completed.carriedInteractionOverride) {
                frameCache_->insert(frame);
            }
            break;
        }
        case runtime::PreviewPreparationStatus::Unsupported:
            next.activity = PreviewActivity::Unsupported;
            next.message = firstDiagnosticSummary(
                next.diagnostics, tr("The composition contains unsupported preview operations"));
            break;
        }
        break;
    }
    case runtime::TaskState::Cancelled:
        next.activity = PreviewActivity::Cancelled;
        next.message =
            firstDiagnosticSummary(next.diagnostics, tr("Preview rendering was cancelled"));
        break;
    case runtime::TaskState::Failed:
        next.message = firstDiagnosticSummary(next.diagnostics, tr("Preview rendering failed"));
        break;
    case runtime::TaskState::Queued:
    case runtime::TaskState::Running:
        next.message = tr("Preview rendering returned an invalid non-terminal result");
        break;
    }

    if (next.activity != PreviewActivity::Ready) {
        if (completed.playbackOutstanding) {
            noteDroppedFrame();
        }
        next.freshness = freshnessFor(next.frame, next.desiredIdentity);
    }
    publish(std::move(next));
}

} // namespace bloom::ui
