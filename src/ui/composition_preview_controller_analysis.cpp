#include <bloom/ui/composition_preview_controller.hpp>

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

namespace bloom::ui {
runtime::TaskSubmission<PreviewPreparationResultHandle>
CompositionPreviewController::submitViewerAnalysis(
    const runtime::PreviewRequestIdentity& identity) {
    return submitViewerPreparation(identity, false);
}

runtime::TaskSubmission<PreviewPreparationResultHandle>
CompositionPreviewController::submitViewerDisplay(const runtime::PreviewRequestIdentity& identity) {
    return submitViewerPreparation(identity, true);
}

runtime::TaskSubmission<ViewerChannelDisplayHandle>
CompositionPreviewController::submitViewerChannelDisplay(PreparedPreviewFrameHandle frame,
                                                         const ViewerChannel channel,
                                                         const std::size_t retainedDisplayBytes) {
    const auto pixelStorageByteLimit = effectivePixelStorageByteLimit();
    if (shuttingDown_ || !frame || !liveSessionMatches(frame->desiredIdentity()) ||
        retainedDisplayBytes >= pixelStorageByteLimit)
        return {};
    runtime::TaskRequest request(
        "Prepare Viewer channel",
        {.kind = runtime::TaskOwnerKind::PanelRequest,
         .id = runtime::TaskOwnerId::fromRaw(frame->desiredIdentity().compositionId.value())},
        runtime::TaskPriority::Visible);
    const auto budget = pixelStorageByteLimit - retainedDisplayBytes;
    auto submission = scheduler_.submit<ViewerChannelDisplayHandle>(
        std::move(request),
        [frame = std::move(frame), channel, budget](runtime::TaskContext& context) {
            return prepareViewerChannelDisplay(frame, channel, budget, context);
        });
    if (submission.accepted())
        taskUiBridge_.wake();
    return submission;
}

runtime::TaskSubmission<std::shared_ptr<const runtime::ChannelInspectionListing>>
CompositionPreviewController::submitChannelListing(const document::NodeId node, std::string port) {
    if (shuttingDown_ || !channelListingProvider_)
        return {};
    const auto time = session_.currentTime();
    auto snapshot = session_.evaluationSnapshotForTime(time);
    const auto composition = session_.compositionId();
    if (snapshot.project().findComposition(composition) == nullptr)
        return {};
    runtime::TaskRequest request(
        "List Viewer channels",
        {.kind = runtime::TaskOwnerKind::PanelRequest,
         .id = runtime::TaskOwnerId::fromRaw(composition.value())},
        runtime::TaskPriority::Visible);
    auto submission = scheduler_.submit<std::shared_ptr<const runtime::ChannelInspectionListing>>(
        std::move(request),
        [provider = channelListingProvider_, snapshot = std::move(snapshot), composition, node,
         port = std::move(port), time,
         colorIntent = settings_.colorIntent](runtime::TaskContext& context) {
            using Result =
                runtime::TaskResult<std::shared_ptr<const runtime::ChannelInspectionListing>>;
            if (context.isCancellationRequested())
                return Result::cancelled();
            auto listing = provider(snapshot, composition, node, port, time, colorIntent,
                                    context.cancellation());
            if (context.isCancellationRequested())
                return Result::cancelled();
            return Result::succeeded(
                std::make_shared<const runtime::ChannelInspectionListing>(std::move(listing)));
        });
    if (submission.accepted())
        taskUiBridge_.wake();
    return submission;
}

runtime::TaskSubmission<PreviewPreparationResultHandle>
CompositionPreviewController::submitViewerPreparation(
    const runtime::PreviewRequestIdentity& identity, const bool displayRoute) {
    if (shuttingDown_ || !liveSessionMatches(identity))
        return {};

    // Genuine provenance. An analysis identity is derived from a displayed frame, so its
    // sourceRevision names the snapshot that frame's pixels came from: the session's genuine
    // snapshot for THIS time (which a finite clip-range edit may have retained at an older
    // revision) for a committed frame, or the live snapshot for an override frame. The request must
    // carry THAT snapshot -- makeCompositionPreviewPipeline rejects a request whose identity and
    // snapshot revisions disagree -- rather than always the live one.
    const auto& live = session_.snapshot();
    const auto& evaluation = session_.evaluationSnapshotForTime(identity.time);
    const bool liveProvenance = identity.sourceRevision == live.revision();
    const bool evaluationProvenance = identity.sourceRevision == evaluation.revision();
    if (!liveProvenance && !evaluationProvenance)
        return {};

    // Active overrides were frozen against the live revision. They may only ride a request whose
    // identity names that same revision; if they cannot match it, refuse rather than pair
    // incompatible data.
    auto overrides = session_.transformInteractionOverrides();
    const auto valueOverrides = session_.valueEditOverrides();
    overrides.insert(overrides.end(), valueOverrides.begin(), valueOverrides.end());
    for (const auto& override : overrides) {
        if (override.sourceRevision != identity.sourceRevision)
            return {};
    }
    if (!liveProvenance) {
        // The retained committed frame has no overrides of its own, and any active live overrides
        // were already refused above for naming a different revision.
        overrides.clear();
    }
    const document::Snapshot snapshot = liveProvenance ? live : evaluation;

    runtime::TaskRequest request(
        "Prepare viewer analysis",
        {.kind = runtime::TaskOwnerKind::Composition,
         .id = runtime::TaskOwnerId::fromRaw(identity.compositionId.value())},
        runtime::TaskPriority::Visible);
    request.sourceVersion = {.documentRevision = identity.sourceRevision.value(),
                             .requestGeneration = identity.requestGeneration};
    const auto budget = effectivePixelStorageByteLimit();
    if (displayRoute && submitter_) {
        auto submission = submitter_(std::move(request), snapshot, identity, budget, overrides);
        if (submission.accepted())
            taskUiBridge_.wake();
        return submission;
    }
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
