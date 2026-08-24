#include <bloom/ui/composition_preview_controller.hpp>

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <bloom/runtime/snapshot_compiler.hpp>

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
            "Preview preparation could not start because the task queue is full");
    case runtime::TaskSubmissionStatus::ShuttingDown:
        return CompositionPreviewController::tr(
            "Preview preparation was cancelled during shutdown");
    case runtime::TaskSubmissionStatus::InvalidRequest:
        return CompositionPreviewController::tr("Preview preparation request was invalid");
    case runtime::TaskSubmissionStatus::UnknownGroup:
    case runtime::TaskSubmissionStatus::CancelledGroup:
        return CompositionPreviewController::tr("Preview preparation could not use its task group");
    case runtime::TaskSubmissionStatus::GroupRegistryFull:
        return CompositionPreviewController::tr(
            "Preview preparation could not allocate a task group");
    case runtime::TaskSubmissionStatus::IdExhausted:
        return CompositionPreviewController::tr("The task identifier limit was reached");
    case runtime::TaskSubmissionStatus::Accepted:
        break;
    }
    return CompositionPreviewController::tr("Preview preparation could not start");
}

QString firstDiagnosticSummary(const std::vector<runtime::TaskDiagnostic>& diagnostics,
                               QString fallback) {
    if (diagnostics.empty() || diagnostics.front().summary.empty()) {
        return fallback;
    }
    return QString::fromStdString(diagnostics.front().summary);
}

QString firstCompileDiagnosticSummary(const runtime::SnapshotCompileResult& result,
                                      QString fallback) {
    if (result.diagnostics.empty() || result.diagnostics.front().summary.empty()) {
        return fallback;
    }
    return QString::fromStdString(result.diagnostics.front().summary);
}

} // namespace

CompositionPreviewController::CompositionPreviewController(CompositionSession& session,
                                                           runtime::TaskScheduler& scheduler,
                                                           TaskUiBridge& taskUiBridge,
                                                           PreviewPreparationFunction preparation,
                                                           QObject* parent)
    : QObject(parent), session_(session), scheduler_(scheduler), taskUiBridge_(taskUiBridge),
      preparation_(std::move(preparation)) {
    connect(&session_, &CompositionSession::snapshotChanged, this,
            &CompositionPreviewController::requestRefresh);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &CompositionPreviewController::requestRefresh);
    connect(&taskUiBridge_, &TaskUiBridge::snapshotsPolled, this,
            &CompositionPreviewController::consumeReadyResult);
    requestPreview();
}

CompositionPreviewController::~CompositionPreviewController() { cancelAndDetachActive(); }

const CompositionPreviewState& CompositionPreviewController::state() const noexcept {
    return state_;
}

bool CompositionPreviewController::isShuttingDown() const noexcept { return shuttingDown_; }

void CompositionPreviewController::requestRefresh() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!shuttingDown_) {
        requestPreview();
    }
}

void CompositionPreviewController::beginShutdown() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_) {
        return;
    }

    shuttingDown_ = true;
    disconnect(&session_, nullptr, this, nullptr);
    cancelAndDetachActive();

    CompositionPreviewState cancelled = state_;
    cancelled.status = CompositionPreviewStatus::Cancelled;
    cancelled.taskId.reset();
    cancelled.compileResult.reset();
    cancelled.diagnostics.clear();
    cancelled.message = tr("Preview preparation was cancelled during application shutdown");
    publish(std::move(cancelled));
}

void CompositionPreviewController::requestPreview() {
    Q_ASSERT(QThread::currentThread() == thread());
    cancelAndDetachActive();

    const document::Snapshot snapshot = session_.snapshot();
    const document::CompositionId compositionId = session_.compositionId();
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        publish({.status = CompositionPreviewStatus::Failed,
                 .compositionId = compositionId,
                 .sourceRevision = snapshot.revision(),
                 .requestGeneration = generation_,
                 .taskId = std::nullopt,
                 .compileResult = {},
                 .diagnostics = {},
                 .message = tr("The preview request generation limit was reached")});
        return;
    }
    const std::uint64_t generation = ++generation_;

    if (snapshot.project().findComposition(compositionId) == nullptr) {
        publish({.status = CompositionPreviewStatus::Unsupported,
                 .compositionId = compositionId,
                 .sourceRevision = snapshot.revision(),
                 .requestGeneration = generation,
                 .taskId = std::nullopt,
                 .compileResult = {},
                 .diagnostics = {},
                 .message = tr("No composition is available for preview preparation")});
        return;
    }
    if (!preparation_) {
        publish({.status = CompositionPreviewStatus::Failed,
                 .compositionId = compositionId,
                 .sourceRevision = snapshot.revision(),
                 .requestGeneration = generation,
                 .taskId = std::nullopt,
                 .compileResult = {},
                 .diagnostics = {},
                 .message = tr("No preview preparation service is available")});
        return;
    }

    runtime::TaskRequest request("Prepare composition preview",
                                 {.kind = runtime::TaskOwnerKind::Composition,
                                  .id = runtime::TaskOwnerId::fromRaw(compositionId.value())},
                                 runtime::TaskPriority::Visible);
    request.coalescingKey = "bloom.preview.prepare";
    request.sourceVersion = {.documentRevision = snapshot.revision().value(),
                             .requestGeneration = generation};

    auto preparation = preparation_;
    auto submission = scheduler_.submit<SnapshotCompileResultHandle>(
        std::move(request), [snapshot, compositionId, preparation = std::move(preparation)](
                                runtime::TaskContext& context) mutable {
            if (context.isCancellationRequested()) {
                return runtime::TaskResult<SnapshotCompileResultHandle>::cancelled();
            }
            return preparation(snapshot, compositionId, context);
        });

    if (!submission.accepted()) {
        CompositionPreviewState rejected{
            .status = submission.status == runtime::TaskSubmissionStatus::ShuttingDown
                          ? CompositionPreviewStatus::Cancelled
                          : CompositionPreviewStatus::Failed,
            .compositionId = compositionId,
            .sourceRevision = snapshot.revision(),
            .requestGeneration = generation,
            .taskId = std::nullopt,
            .compileResult = {},
            .diagnostics = {},
            .message = submissionFailureMessage(submission.status),
        };
        if (submission.diagnostic.has_value()) {
            rejected.diagnostics.push_back(std::move(*submission.diagnostic));
        }
        publish(std::move(rejected));
        return;
    }

    const runtime::TaskId taskId = submission.handle.id();
    active_.emplace(ActiveRequest{.handle = std::move(submission.handle),
                                  .compositionId = compositionId,
                                  .sourceRevision = snapshot.revision(),
                                  .generation = generation});
    publish({.status = CompositionPreviewStatus::Preparing,
             .compositionId = compositionId,
             .sourceRevision = snapshot.revision(),
             .requestGeneration = generation,
             .taskId = taskId,
             .compileResult = {},
             .diagnostics = {},
             .message = tr("Preparing the current composition plan")});
    taskUiBridge_.wake();
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
    if (shuttingDown_ || !isCurrent(completed)) {
        return;
    }

    if (session_.compositionId() != completed.compositionId ||
        session_.snapshot().revision() != completed.sourceRevision) {
        requestPreview();
        return;
    }

    CompositionPreviewState next{.status = CompositionPreviewStatus::Failed,
                                 .compositionId = completed.compositionId,
                                 .sourceRevision = completed.sourceRevision,
                                 .requestGeneration = completed.generation,
                                 .taskId = completed.handle.id(),
                                 .compileResult = {},
                                 .diagnostics = result->diagnostics(),
                                 .message = {}};

    switch (result->state()) {
    case runtime::TaskState::Succeeded: {
        const auto& value = result->value();
        if (!value.has_value() || *value == nullptr) {
            next.message = tr("Preview preparation returned no compiled result");
            break;
        }
        next.compileResult = *value;
        switch (next.compileResult->status) {
        case runtime::SnapshotCompileStatus::Compiled:
            if (next.compileResult->plan == nullptr) {
                next.message = tr("Preview preparation returned no compiled plan");
                break;
            }
            if (next.compileResult->plan->sourceRevision != completed.sourceRevision ||
                next.compileResult->plan->compositionId != completed.compositionId ||
                next.compileResult->plan->projectId != session_.snapshot().project().id()) {
                next.message = tr("Preview preparation returned a plan for different source data");
                next.compileResult.reset();
                break;
            }
            next.status = CompositionPreviewStatus::Ready;
            next.message = tr("The composition plan is ready; pixel evaluation is not connected");
            break;
        case runtime::SnapshotCompileStatus::Unsupported:
            next.status = CompositionPreviewStatus::Unsupported;
            next.message = firstCompileDiagnosticSummary(
                *next.compileResult, tr("The composition contains unsupported preview operations"));
            break;
        case runtime::SnapshotCompileStatus::Cancelled:
            next.status = CompositionPreviewStatus::Cancelled;
            next.message = tr("Preview preparation was cancelled");
            break;
        case runtime::SnapshotCompileStatus::Failed:
            next.message = firstCompileDiagnosticSummary(*next.compileResult,
                                                         tr("Preview preparation failed"));
            break;
        }
        break;
    }
    case runtime::TaskState::Cancelled:
        next.status = CompositionPreviewStatus::Cancelled;
        next.message =
            firstDiagnosticSummary(next.diagnostics, tr("Preview preparation was cancelled"));
        break;
    case runtime::TaskState::Failed:
        next.message = firstDiagnosticSummary(next.diagnostics, tr("Preview preparation failed"));
        break;
    case runtime::TaskState::Queued:
    case runtime::TaskState::Running:
        next.message = tr("Preview preparation returned an invalid non-terminal result");
        break;
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
    state_ = std::move(state);
    emit stateChanged();
}

bool CompositionPreviewController::isCurrent(const ActiveRequest& request) const noexcept {
    return request.generation == generation_ && state_.requestGeneration == request.generation &&
           state_.compositionId == request.compositionId &&
           state_.sourceRevision == request.sourceRevision &&
           state_.status == CompositionPreviewStatus::Preparing &&
           state_.taskId == request.handle.id();
}

} // namespace bloom::ui
