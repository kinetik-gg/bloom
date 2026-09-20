#include "composition_editor_support.hpp"
#include "composition_export_dialog.hpp"
#include <QCryptographicHash>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

namespace bloom::ui {
void FrameExportController::requestCompositionExport() {
    if (!canExport())
        return;
    const auto* composition = session_.composition();
    if (!composition)
        return;
    const auto mapping = core::FrameTimeMapping::create(
        composition->duration(), composition->format().frameRate().numerator(),
        composition->format().frameRate().denominator());
    if (!mapping)
        return;
    std::uint32_t rate = 48000;
    for (const auto& asset : session_.snapshot().project().assets())
        if (asset.rate > 0 && asset.channels > 0) {
            rate = asset.rate;
            break;
        }
    QString projectKey;
    if (projectPathProvider_) {
        if (const auto path = projectPathProvider_(); path)
            projectKey = QString::fromLatin1(
                QCryptographicHash::hash(
                    QByteArray::fromStdString(path->lexically_normal().generic_string()),
                    QCryptographicHash::Sha256)
                    .toHex());
    }
    const auto request =
        compositionExportDialog(mapping.value()->maximumFrameIndex(), rate, projectKey);
    if (request)
        beginCompositionExport(*request);
}
void FrameExportController::beginCompositionExport(CompositionExportRequest request) {
    if (!canExport() || request.range.destination.empty()) {
        emit exportFinished(FrameExportOutcome::Refused, tr("Composition export is unavailable."));
        return;
    }
    if (request.preset == output::OutputPresetV1::PngRgba8SrgbV1 ||
        request.preset == output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1 ||
        request.preset == output::OutputPresetV1::TiffRgba16SrgbV1) {
        beginRangeExport(std::move(request.range));
        return;
    }
    const auto* composition = session_.composition();
    if (!composition)
        return;
    host::SequenceExportRequestV1 captured{
        .composition = {session_.snapshot(), session_.compositionId()},
        .range = {.destination = request.range.destination,
                  .firstFrame = request.range.firstFrame,
                  .lastFrame = request.range.lastFrame,
                  .frameRate = composition->format().frameRate(),
                  .duration = composition->duration()},
        .preset = request.preset,
        .profile = request.profile,
        .audio = request.audio,
        .hardware = request.hardware,
        .openh264Consent = request.openh264Consent,
        .sampleRate = request.sampleRate,
        .pcmCodec =
            request.preset == output::OutputPresetV1::PcmWavV1 ? request.profile : "pcm_s16le",
        .bwfDescription = {},
        .assetBaseDirectory = {},
        .worker = {},
        .startFrame = request.range.naming.startFrame,
        .framePadding = request.range.naming.framePadding,
        .namePattern = request.range.naming.namePattern,
        .workingColorSpaceId = std::string(session_.colorIntent().workingColorSpaceId),
        .ocioConfigRevision = session_.colorIntent().ocioConfigRevision,
        .ocioConfigUri = std::string(session_.colorIntent().ocioConfigUri)};
    captured.gpuProvider = gpuExportProvider_;
    if (request.deliverable == DeliverablePreset::Review) {
        captured.displayName = "Rec.1886 Rec.709 - Display";
        captured.viewName = "ACES 1.0 - SDR Video";
    }
    mediaExport_ = std::make_unique<host::SequenceExportRunnerV1>(
        scheduler_, compiler_, publicationCoordinator_, artifactCoordinator_, *ledger_,
        std::move(captured));
    pendingDestination_ = std::move(request.range.destination);
    pendingPreset_ = request.preset;
    setActivity(FrameExportActivity::CompilingPlan);
    emit rangeProgressChanged();
    taskUiBridge_.wake();
}
void FrameExportController::pollCompositionExport() {
    // The approval prompt runs a nested Qt event loop, during which the task-bridge timer can fire
    // this poll re-entrantly. A nested completion resets mediaExport_, and the outer frame would
    // then dereference null; the guard makes the poll non-reentrant and tolerates a cleared export.
    if (pollingComposition_ || !mediaExport_)
        return;
    pollingComposition_ = true;
    struct PollGuard final {
        bool& flag;
        ~PollGuard() { flag = false; }
    } pollGuard{pollingComposition_};
    const auto previous = mediaExport_->encodedFrames();
    mediaExport_->poll();
    if (mediaExport_->encodedFrames() != previous)
        emit rangeProgressChanged();
    if (mediaExport_->stage() == host::SequenceExportStageV1::AwaitingApproval) {
        const auto* analysis = mediaExport_->analysis();
        const auto frameDigest = mediaExport_->frameApprovalDigest();
        if (!analysis || !frameDigest) {
            mediaExport_->cancel();
            return;
        }
        FrameExportApprovalPrompt prompt;
        prompt.destination = pendingDestination_;
        prompt.preset = pendingPreset_;
        prompt.width = analysis->settings.width;
        prompt.height = analysis->settings.height;
        if (const auto identity = output::outputPresetIdentityV1(pendingPreset_))
            prompt.presetName =
                QString::fromUtf8(identity->serializedId.data(),
                                  static_cast<qsizetype>(identity->serializedId.size()));
        prompt.profile = QString::fromStdString(analysis->settings.profile);
        prompt.implementationNote = QString::fromStdString(analysis->implementationNote);
        const auto hex = analysis->digest.toLowercaseHex();
        prompt.digestShortForm = QString::fromLatin1(hex.data(), 16);
        for (const auto& facet : analysis->facets) {
            if (facet.preservation == output::OutputPreservationStateV1::Exact)
                ++prompt.facets.exactFacetCount;
            else {
                ++prompt.facets.nonExactFacetCount;
                prompt.facets.nonExactFacetNames << QString::fromStdString(facet.description);
            }
        }
        setActivity(FrameExportActivity::AwaitingApproval);
        if (!approvalDecisionProvider_ ||
            approvalDecisionProvider_(prompt) != FrameExportApprovalDecision::Export ||
            !mediaExport_->approve(analysis->digest, *frameDigest))
            mediaExport_->cancel();
        setActivity(FrameExportActivity::Publishing);
        taskUiBridge_.wake();
    }
    if (const auto& result = mediaExport_->result(); result) {
        FrameExportOutcome outcome = FrameExportOutcome::Failed;
        QString message;
        if (result->published()) {
            outcome =
                result->publication.outcome ==
                        platform::StagedArtifactPublicationOutcome::PublishedWithDurabilityWarning
                    ? FrameExportOutcome::PublishedWithWarning
                    : FrameExportOutcome::Published;
            message = tr("Exported %1 frames to %2.")
                          .arg(result->encodedFrames)
                          .arg(QString::fromStdString(pendingDestination_.filename().string()));
        } else if (result->failure) {
            if (result->failure->reason == media::provider::Error::Cancelled)
                outcome = FrameExportOutcome::Cancelled;
            message = QString::fromStdString(result->failure->detail);
        } else
            message = tr("Composition export was superseded or could not be published.");
        mediaExport_.reset();
        setActivity(FrameExportActivity::Idle);
        emit rangeProgressChanged();
        finish(outcome, std::move(message));
    }
}
} // namespace bloom::ui
