#include <bloom/ui/composition_preview_controller.hpp>

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

namespace bloom::ui {
runtime::TaskSubmission<PreviewPreparationResultHandle>
CompositionPreviewController::submitViewerAnalysis(
    const runtime::PreviewRequestIdentity& identity) {
    if (shuttingDown_ || !liveSessionMatches(identity))
        return {};
    runtime::TaskRequest request(
        "Prepare viewer analysis",
        {.kind = runtime::TaskOwnerKind::Composition,
         .id = runtime::TaskOwnerId::fromRaw(identity.compositionId.value())},
        runtime::TaskPriority::Visible);
    request.sourceVersion = {.documentRevision = identity.sourceRevision.value(),
                             .requestGeneration = identity.requestGeneration};
    const auto snapshot = session_.snapshot();
    const auto overrides = session_.transformInteractionOverrides();
    const auto budget = settings_.pixelStorageByteLimit;
    auto submission = scheduler_.submit<PreviewPreparationResultHandle>(
        std::move(request), [snapshot, identity, overrides, budget,
                             preparation = preparation_](runtime::TaskContext& context) {
            if (context.isCancellationRequested())
                return runtime::TaskResult<PreviewPreparationResultHandle>::cancelled();
            return preparation(snapshot, identity, budget, overrides, context);
        });
    if (submission.accepted())
        taskUiBridge_.wake();
    return submission;
}
} // namespace bloom::ui
