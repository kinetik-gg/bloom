#include <bloom/ui/composition_preview_controller.hpp>

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <bloom/document/project.hpp>

#include <QThread>

#include <limits>
#include <string>
#include <utility>

namespace bloom::ui {
namespace {

QString submissionFailureMessage(const runtime::TaskSubmissionStatus status) {
    switch (status) {
    case runtime::TaskSubmissionStatus::QueueFull:
        return CompositionPreviewController::tr(
            "The preview could not start because the task queue is full");
    case runtime::TaskSubmissionStatus::ShuttingDown:
        return CompositionPreviewController::tr("The preview was cancelled during shutdown");
    case runtime::TaskSubmissionStatus::InvalidRequest:
        return CompositionPreviewController::tr("The preview request was invalid");
    case runtime::TaskSubmissionStatus::UnknownGroup:
    case runtime::TaskSubmissionStatus::CancelledGroup:
        return CompositionPreviewController::tr("The preview could not use its task group");
    case runtime::TaskSubmissionStatus::GroupRegistryFull:
        return CompositionPreviewController::tr("The preview could not allocate a task group");
    case runtime::TaskSubmissionStatus::IdExhausted:
        return CompositionPreviewController::tr("The task identifier limit was reached");
    case runtime::TaskSubmissionStatus::Accepted:
        break;
    }
    return CompositionPreviewController::tr("The preview could not start");
}

QString firstDiagnosticSummary(const std::vector<runtime::TaskDiagnostic>& diagnostics,
                               QString fallback) {
    if (diagnostics.empty() || diagnostics.front().summary.empty()) {
        return fallback;
    }
    return QString::fromStdString(diagnostics.front().summary);
}

FrameFreshness freshnessFor(const PreparedPreviewFrameHandle& frame,
                            const std::optional<runtime::PreviewRequestIdentity>& desiredIdentity) {
    if (frame == nullptr) {
        return FrameFreshness::None;
    }
    return desiredIdentity.has_value() && frame->desiredIdentity() == *desiredIdentity
               ? FrameFreshness::Current
               : FrameFreshness::Stale;
}

} // namespace

CompositionPreviewController::CompositionPreviewController(
    CompositionSession& session, runtime::TaskScheduler& scheduler, TaskUiBridge& taskUiBridge,
    PreviewPreparationFunction preparation, CompositionPreviewSettings settings, QObject* parent)
    : QObject(parent), session_(session), scheduler_(scheduler), taskUiBridge_(taskUiBridge),
      preparation_(std::move(preparation)), settings_(settings) {
    connect(&session_, &CompositionSession::snapshotChanged, this,
            &CompositionPreviewController::requestRefresh);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &CompositionPreviewController::handleCompositionChanged);
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            &CompositionPreviewController::handleCurrentTimeChanged);
    connect(&taskUiBridge_, &TaskUiBridge::snapshotsPolled, this,
            &CompositionPreviewController::consumeReadyResult);
    requestPreview(true);
}

CompositionPreviewController::~CompositionPreviewController() { cancelAndDetachActive(); }

const CompositionPreviewState& CompositionPreviewController::state() const noexcept {
    return state_;
}

bool CompositionPreviewController::isShuttingDown() const noexcept { return shuttingDown_; }

void CompositionPreviewController::requestRefresh() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!shuttingDown_) {
        requestPreview(false);
    }
}

void CompositionPreviewController::handleCompositionChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!shuttingDown_) {
        requestPreview(true);
    }
}

void CompositionPreviewController::handleCurrentTimeChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_) {
        return;
    }
    if (state_.desiredIdentity.has_value() &&
        state_.desiredIdentity->compositionId == session_.compositionId() &&
        state_.desiredIdentity->time == session_.currentTime()) {
        return;
    }
    requestPreview(false);
}

void CompositionPreviewController::beginShutdown() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_) {
        return;
    }

    shuttingDown_ = true;
    disconnect(&session_, nullptr, this, nullptr);
    pending_.reset();
    cancelAndDetachActive();

    CompositionPreviewState cancelled = state_;
    cancelled.activity = PreviewActivity::Cancelled;
    cancelled.freshness = freshnessFor(cancelled.frame, cancelled.desiredIdentity);
    cancelled.taskId.reset();
    cancelled.diagnostics.clear();
    cancelled.message = tr("Preview rendering was cancelled during application shutdown");
    publish(std::move(cancelled));
}

void CompositionPreviewController::requestPreview(const bool clearLastGoodFrame) {
    Q_ASSERT(QThread::currentThread() == thread());

    const document::Snapshot snapshot = session_.snapshot();
    const document::CompositionId compositionId = session_.compositionId();
    PreparedPreviewFrameHandle retainedFrame = clearLastGoodFrame ? nullptr : state_.frame;
    if (retainedFrame != nullptr &&
        (retainedFrame->desiredIdentity().projectId != snapshot.project().id() ||
         retainedFrame->desiredIdentity().compositionId != compositionId)) {
        retainedFrame.reset();
    }

    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        pending_.reset();
        if (active_.has_value()) {
            active_->handle.cancel();
        }
        CompositionPreviewState failed{
            .activity = PreviewActivity::Failed,
            .freshness = FrameFreshness::None,
            .desiredIdentity = std::nullopt,
            .taskId = std::nullopt,
            .frame = std::move(retainedFrame),
            .diagnostics = {},
            .message = tr("The preview request generation limit was reached"),
        };
        failed.freshness = freshnessFor(failed.frame, failed.desiredIdentity);
        publish(std::move(failed));
        return;
    }
    const std::uint64_t generation = ++generation_;
    const runtime::PreviewRequestIdentity desiredIdentity{
        .projectId = snapshot.project().id(),
        .compositionId = compositionId,
        .sourceRevision = snapshot.revision(),
        .requestGeneration = generation,
        .time = session_.currentTime(),
        .output = runtime::PreviewOutput::Composition,
        .resolution = settings_.resolution,
        .quality = settings_.quality,
        .colorIntent = settings_.colorIntent,
    };

    const auto publishTerminal = [this, &desiredIdentity,
                                  &retainedFrame](const PreviewActivity activity, QString message) {
        pending_.reset();
        if (active_.has_value()) {
            active_->handle.cancel();
        }
        CompositionPreviewState terminal{
            .activity = activity,
            .freshness = FrameFreshness::None,
            .desiredIdentity = desiredIdentity,
            .taskId = std::nullopt,
            .frame = retainedFrame,
            .diagnostics = {},
            .message = std::move(message),
        };
        terminal.freshness = freshnessFor(terminal.frame, terminal.desiredIdentity);
        publish(std::move(terminal));
    };

    if (snapshot.project().findComposition(compositionId) == nullptr) {
        publishTerminal(PreviewActivity::Unsupported,
                        tr("No composition is available for preview rendering"));
        return;
    }
    if (!preparation_) {
        publishTerminal(PreviewActivity::Failed,
                        tr("No composition preview pipeline is available"));
        return;
    }
    if (settings_.pixelStorageByteLimit == 0) {
        publishTerminal(PreviewActivity::Failed,
                        tr("The composition preview memory budget is invalid"));
        return;
    }

    PendingRequest pendingRequest{.snapshot = snapshot,
                                  .desiredIdentity = desiredIdentity,
                                  .pixelStorageByteLimit = settings_.pixelStorageByteLimit};

    if (active_.has_value()) {
        active_->handle.cancel();
        // The cancelled handle remains the admission gate until its terminal result is observed.
        pending_.emplace(std::move(pendingRequest));
        publishRendering(desiredIdentity, std::nullopt, std::move(retainedFrame));
        taskUiBridge_.wake();
        return;
    }

    Q_ASSERT(!pending_.has_value());
    submitPreview(std::move(pendingRequest), std::move(retainedFrame));
}

void CompositionPreviewController::submitPreview(PendingRequest pendingRequest,
                                                 PreparedPreviewFrameHandle retainedFrame) {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(!active_.has_value());

    const auto desiredIdentity = pendingRequest.desiredIdentity;

    runtime::TaskRequest request(
        "Render composition preview",
        {.kind = runtime::TaskOwnerKind::Composition,
         .id = runtime::TaskOwnerId::fromRaw(desiredIdentity.compositionId.value())},
        runtime::TaskPriority::Visible);
    request.coalescingKey = "bloom.preview.render";
    request.sourceVersion = {
        .documentRevision = desiredIdentity.sourceRevision.value(),
        .requestGeneration = desiredIdentity.requestGeneration,
    };

    auto preparation = preparation_;
    const std::size_t pixelStorageByteLimit = pendingRequest.pixelStorageByteLimit;
    auto submission = scheduler_.submit<PreviewPreparationResultHandle>(
        std::move(request),
        [snapshot = std::move(pendingRequest.snapshot), desiredIdentity, pixelStorageByteLimit,
         preparation = std::move(preparation)](runtime::TaskContext& context) mutable {
            if (context.isCancellationRequested()) {
                return runtime::TaskResult<PreviewPreparationResultHandle>::cancelled();
            }
            return preparation(snapshot, desiredIdentity, pixelStorageByteLimit, context);
        });

    if (!submission.accepted()) {
        CompositionPreviewState rejected{
            .activity = submission.status == runtime::TaskSubmissionStatus::ShuttingDown
                            ? PreviewActivity::Cancelled
                            : PreviewActivity::Failed,
            .freshness = FrameFreshness::None,
            .desiredIdentity = desiredIdentity,
            .taskId = std::nullopt,
            .frame = std::move(retainedFrame),
            .diagnostics = {},
            .message = submissionFailureMessage(submission.status),
        };
        if (submission.diagnostic.has_value()) {
            rejected.diagnostics.push_back(std::move(*submission.diagnostic));
        }
        rejected.freshness = freshnessFor(rejected.frame, rejected.desiredIdentity);
        publish(std::move(rejected));
        return;
    }

    const runtime::TaskId taskId = submission.handle.id();
    active_.emplace(
        ActiveRequest{.handle = std::move(submission.handle), .desiredIdentity = desiredIdentity});
    publishRendering(desiredIdentity, taskId, std::move(retainedFrame));
    taskUiBridge_.wake();
}

void CompositionPreviewController::publishRendering(runtime::PreviewRequestIdentity desiredIdentity,
                                                    std::optional<runtime::TaskId> taskId,
                                                    PreparedPreviewFrameHandle retainedFrame) {
    CompositionPreviewState rendering{
        .activity = PreviewActivity::Rendering,
        .freshness = FrameFreshness::None,
        .desiredIdentity = desiredIdentity,
        .taskId = taskId,
        .frame = std::move(retainedFrame),
        .diagnostics = {},
        .message = {},
    };
    rendering.freshness = freshnessFor(rendering.frame, rendering.desiredIdentity);
    rendering.message = rendering.freshness == FrameFreshness::Stale
                            ? tr("Rendering the current composition; showing the previous frame")
                            : tr("Rendering the current composition");
    publish(std::move(rendering));
}

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
    if (pending_.has_value()) {
        PendingRequest pendingRequest = std::move(*pending_);
        pending_.reset();
        submitPreview(std::move(pendingRequest), state_.frame);
        return;
    }
    if (!isCurrent(completed)) {
        return;
    }
    if (!liveSessionMatches(completed.desiredIdentity)) {
        requestPreview(session_.compositionId() != completed.desiredIdentity.compositionId);
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
            if (!frame->displayBuffer().isValid()) {
                next.message = tr("Preview rendering returned an invalid display buffer");
                break;
            }
            next.activity = PreviewActivity::Ready;
            next.freshness = FrameFreshness::Current;
            next.frame = frame;
            next.message = tr("The current composition frame is ready");
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
        next.freshness = freshnessFor(next.frame, next.desiredIdentity);
    }
    publish(std::move(next));
}

void CompositionPreviewController::cancelAndDetachActive() noexcept {
    if (!active_.has_value()) {
        return;
    }
    active_->handle.cancel();
    active_.reset();
}

void CompositionPreviewController::publish(CompositionPreviewState state) {
    Q_ASSERT((state.freshness == FrameFreshness::None) == (state.frame == nullptr));
    Q_ASSERT(state.freshness != FrameFreshness::Current ||
             (state.desiredIdentity.has_value() && state.frame != nullptr &&
              state.frame->desiredIdentity() == *state.desiredIdentity));
    Q_ASSERT(
        state.freshness != FrameFreshness::Stale ||
        (state.frame != nullptr && (!state.desiredIdentity.has_value() ||
                                    state.frame->desiredIdentity() != *state.desiredIdentity)));
    Q_ASSERT(state.activity != PreviewActivity::Ready ||
             state.freshness == FrameFreshness::Current);
    state_ = std::move(state);
    emit stateChanged();
}

bool CompositionPreviewController::isCurrent(const ActiveRequest& request) const {
    return request.desiredIdentity.requestGeneration == generation_ &&
           state_.desiredIdentity.has_value() &&
           *state_.desiredIdentity == request.desiredIdentity &&
           state_.taskId == request.handle.id();
}

bool CompositionPreviewController::liveSessionMatches(
    const runtime::PreviewRequestIdentity& desiredIdentity) const noexcept {
    const auto& snapshot = session_.snapshot();
    return session_.compositionId() == desiredIdentity.compositionId &&
           snapshot.revision() == desiredIdentity.sourceRevision &&
           snapshot.project().id() == desiredIdentity.projectId;
}

} // namespace bloom::ui
