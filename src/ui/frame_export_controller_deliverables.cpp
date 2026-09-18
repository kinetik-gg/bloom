#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

namespace bloom::ui {
void FrameExportController::setProjectPathProvider(FrameExportDestinationProvider provider) {
    projectPathProvider_ = std::move(provider);
}
void FrameExportController::beginDeliverablePreparation(
    std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt) {
    if (!sequence_)
        return;
    const auto options = sequence_->exr;
    auto submission = scheduler_.submit<std::shared_ptr<const output::OutputAnalysisAttemptV1>>(
        runtime::TaskRequest("Prepare EXR deliverable",
                             {runtime::TaskOwnerKind::Export, runtime::TaskOwnerId::fromRaw(1)},
                             runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo),
        [attempt = std::move(attempt), options, ledger = ledger_](runtime::TaskContext& context) {
            using Result =
                runtime::TaskResult<std::shared_ptr<const output::OutputAnalysisAttemptV1>>;
            if (context.isCancellationRequested())
                return Result::cancelled();
            context.reportProgress({"ColorPreparing", "EXR output colour", 0, std::nullopt});
            auto result = output::prepareFlatExrAttemptV1(*attempt, options, *ledger);
            if (context.isCancellationRequested())
                return Result::cancelled();
            return Result::succeeded(result ? result.attempt() : nullptr);
        });
    if (!submission.accepted()) {
        finishSequence(FrameExportOutcome::Failed,
                       tr("EXR colour preparation could not be started."));
        return;
    }
    inFlight_.emplace<DeliverableHandle>(std::move(submission.handle));
    taskUiBridge_.wake();
}
void FrameExportController::pollDeliverablePreparation(DeliverableHandle& handle) {
    auto result = handle.tryTakeResult();
    if (!result)
        return;
    inFlight_.emplace<std::monostate>();
    if (result->state() == runtime::TaskState::Cancelled || (sequence_ && sequence_->cancelled)) {
        finishSequence(FrameExportOutcome::Cancelled, tr("Export cancelled."));
        return;
    }
    if (!result->value() || !*result->value() || !(*result->value())->approvable()) {
        finishSequence(FrameExportOutcome::NotApprovable,
                       tr("The selected EXR output colour space or compression is unavailable in "
                          "this project."));
        return;
    }
    presentApproval(*result->value());
}
} // namespace bloom::ui
