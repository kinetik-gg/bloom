#include <QPointer>
#include <QThread>
#include <algorithm>
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
    if (trace_)
        trace_->noteForegroundDelivery();

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
    if (trace_) {
        const bool hasFrame = prepared.has_value() && *prepared && (*prepared)->frame();
        const auto outcome =
            result->state() == runtime::TaskState::Cancelled ? PreviewTraceOutcome::Cancelled
            : result->state() == runtime::TaskState::Failed  ? PreviewTraceOutcome::Failed
            : hasFrame                                       ? PreviewTraceOutcome::Accepted
            : prepared && *prepared &&
                    (*prepared)->status() == runtime::PreviewPreparationStatus::Unsupported
                ? PreviewTraceOutcome::Unsupported
                : PreviewTraceOutcome::Invalid;
        // Delivery is the UI's observation of a terminal mailbox, not a worker/GPU timestamp.
        trace_->record(
            hasFrame ? (*prepared)->frame()->desiredIdentity() : completed.desiredIdentity,
            PreviewTraceStage::Delivered, outcome,
            hasFrame ? static_cast<std::uint8_t>((*prepared)->frame()->provenance().provider) : 255,
            completed.handle.id().value());
    }
    if (result->state() == runtime::TaskState::Succeeded && prepared.has_value() &&
        *prepared != nullptr && (*prepared)->frame() != nullptr &&
        (*prepared)->frame()->desiredIdentity() == completed.desiredIdentity &&
        !completed.carriedInteractionOverride) {
        recordPreparationDuration(completed.desiredIdentity, deliveredAt - completed.submittedAt);
    }
    const auto ownKey =
        completed.playbackDeadline ? cacheKeyForTime(completed.desiredIdentity.time) : std::nullopt;
    const auto deliveryGeneration = generation_;
    const bool validPlayback =
        playbackActive_ && !shuttingDown_ && !cachePurgeGate_ &&
        (!pending_ || pending_->kind == PreviewRequestKind::Playback) &&
        completed.playbackDeadline && !completed.carriedInteractionOverride &&
        result->state() == runtime::TaskState::Succeeded && result->diagnostics().empty() &&
        prepared && *prepared &&
        (*prepared)->status() == runtime::PreviewPreparationStatus::Prepared &&
        (*prepared)->frame() &&
        (*prepared)->frame()->desiredIdentity() == completed.desiredIdentity &&
        (*prepared)->frame()->isDisplayValid() && ownKey &&
        *ownKey == PreviewFrameCacheKey::forIdentity(completed.desiredIdentity);
    if (completed.playbackDeadline &&
        (deliveredAt > *completed.playbackDeadline || !isCurrent(completed))) {
        if (trace_)
            trace_->record(completed.desiredIdentity, PreviewTraceStage::Delivered,
                           PreviewTraceOutcome::Late, 255, completed.handle.id().value());
        if (completed.playbackOutstanding) {
            noteDroppedFrame();
            completed.playbackOutstanding = false;
        }
    }
    if (generation_ != deliveryGeneration || shuttingDown_ || cachePurgeGate_)
        return;
    if (validPlayback && prepared && !isCurrent(completed)) {
        // This immutable product keeps its own exact time/revision. Never restamp it as a newer
        // pending time; late but valid pixels can warm the loop and describe useful stale output.
        frameCache_->insert((*prepared)->frame());
        if (generation_ != deliveryGeneration || shuttingDown_ || cachePurgeGate_)
            return;
        if (playbackActive_ && !isCurrent(completed) && state_.desiredIdentity &&
            (!state_.frame || state_.frame->desiredIdentity().requestGeneration <
                                  completed.desiredIdentity.requestGeneration)) {
            auto domain = completed.desiredIdentity;
            domain.time = state_.desiredIdentity->time;
            domain.sourceRevision = state_.desiredIdentity->sourceRevision;
            domain.requestGeneration = state_.desiredIdentity->requestGeneration;
            if (domain == *state_.desiredIdentity) {
                const auto newest = *state_.desiredIdentity;
                publishRendering(newest, std::nullopt, (*prepared)->frame());
                // A synchronous state observer may rebind, purge or close the recovery domain.
                if (generation_ != deliveryGeneration || shuttingDown_ || cachePurgeGate_)
                    return;
            }
        }
    }
    if (pending_.has_value()) {
        // A completed live frame is useful feedback even if input has advanced. Publish it as
        // stale while the newest override is prepared; never cache it as committed truth.
        const bool liveFrame = completed.carriedInteractionOverride &&
                               !pending_->interactionOverride.empty() &&
                               liveSessionMatches(completed.desiredIdentity) &&
                               completed.desiredIdentity.time == pending_->desiredIdentity.time &&
                               result->state() == runtime::TaskState::Succeeded &&
                               prepared.has_value() && *prepared && (*prepared)->frame() &&
                               (*prepared)->frame()->desiredIdentity() == completed.desiredIdentity;
        if (liveFrame && prepared.has_value())
            publishRendering(pending_->desiredIdentity, std::nullopt, (*prepared)->frame());
        // A superseded result that was not shown counts as a dropped frame.
        if (!liveFrame && !completed.playbackDeadline.has_value()) {
            noteDroppedFrame();
        }
        flushCadence();
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
            // Alternative-agnostic (issue #97, task C3): frame may carry the reference product, the
            // qualified product, the CPU display-only product, or the GPU-resident product
            // (PreparedPreviewFrame's closed alternative). displayBufferView() normalizes the
            // first three; the resident arm deliberately has no CPU pixels, so its validity is the
            // live lease and is checked via isDisplayValid(). The resident arm is identified by its
            // honest provenance because residentFrame() is resident-only and would read an inactive
            // variant member on a CPU frame.
            const bool resident =
                frame->provenance().provider == runtime::PreviewDisplayProvider::GpuResident;
            if (resident ? !frame->isDisplayValid() : !frame->displayBufferView().has_value()) {
                next.message = tr("Preview rendering returned an invalid display buffer");
                break;
            }
            next.activity = PreviewActivity::Ready;
            next.freshness = FrameFreshness::Current;
            next.frame = frame;
            next.message = firstDiagnosticSummary(next.diagnostics,
                                                  tr("The current composition frame is ready"));
            // Playing without a cache keeps today's behavior but fills the cache as it goes, so the
            // second pass over the same range is a sequence of lookups (task PERF1, item 3). A
            // frame rendered under an interactive override is the exception: its pixels belong to a
            // gesture, and its identity cannot say so.
            if (!completed.carriedInteractionOverride && next.diagnostics.empty()) {
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
    if (processingPolicy_ && result->state() == runtime::TaskState::Failed) {
        for (const auto& diagnostic : next.diagnostics) {
            if (diagnostic.admissionRetry) {
                deferredAdmission_ = diagnostic.admissionRetry;
                break;
            }
        }
    }
    const QPointer<CompositionPreviewController> guard(this);
    publish(std::move(next));
    if (guard)
        guard->notifyProcessingCapacityChanged();
}

} // namespace bloom::ui
