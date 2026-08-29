#include <bloom/host/session_save.hpp>

#include <new>
#include <utility>
#include <vector>

namespace bloom::host {

SessionSaveResult SessionSaveResult::captureFailure(SessionSaveCaptureOutcome outcome) noexcept {
    SessionSaveResult result;
    result.stage_ = SessionSaveStage::Capture;
    result.captureOutcome_ = outcome;
    return result;
}

SessionSaveResult SessionSaveResult::publicationFailure(
    SavePublicationFailure failure,
    const std::optional<platform::StagedArtifactError> rejectDiagnostic) noexcept {
    SessionSaveResult result;
    result.stage_ = SessionSaveStage::Publication;
    result.publicationFailure_.emplace(std::move(failure));
    result.rejectDiagnostic_ = rejectDiagnostic;
    return result;
}

SessionSaveResult
SessionSaveResult::unpublished(platform::StagedArtifactPublicationResult publication,
                               const PublicationIntentId intentId) noexcept {
    SessionSaveResult result;
    result.stage_ = SessionSaveStage::Savepoint;
    result.publication_ = publication;
    result.intentId_ = intentId;
    return result;
}

SessionSaveResult
SessionSaveResult::published(platform::StagedArtifactPublicationResult publication,
                             const PublicationIntentId intentId,
                             const ProjectSessionSavepointStatus savepointStatus) noexcept {
    SessionSaveResult result;
    result.stage_ = SessionSaveStage::Savepoint;
    result.publication_ = publication;
    result.intentId_ = intentId;
    result.savepointStatus_ = savepointStatus;
    return result;
}

SessionSaveResult SessionSaveResult::publishedSavepointBookkeepingFailed(
    platform::StagedArtifactPublicationResult publication, const PublicationIntentId intentId,
    const SessionSaveSavepointBookkeepingFailure failure) noexcept {
    SessionSaveResult result;
    result.stage_ = SessionSaveStage::Savepoint;
    result.publication_ = publication;
    result.intentId_ = intentId;
    result.savepointBookkeepingFailure_ = failure;
    return result;
}

SavePublicationResult executeSessionSaveMiddle(
    PublicationCoordinator& coordinator, platform::StagedArtifactCoordinator& artifacts,
    const SessionSaveOwningInput& input, PublicationAdmission* const preAdmitted,
    const std::atomic_bool* const cancellationFlag,
    project::ProjectIoOperationMemory operation) noexcept {
    const project::CanonicalManifestV1 manifest{
        .documentSchemaVersion = {1, input.capturedInput.schemaMinor()},
        .requirements = input.capturedInput.retainedRequirements(),
    };
    const project::CanonicalDocumentV1 documentInput{
        .snapshot = &input.capturedInput.snapshot(),
        .colorSettings = &input.capturedInput.colorSettings(),
        .roundTrip = input.capturedInput.roundTrip(),
        .schemaMinor = input.capturedInput.schemaMinor(),
    };
    const SavePublicationRequest publicationRequest{
        .targetPath = input.targetPath,
        .overwritePolicy = input.overwritePolicy,
        .expectedTarget = input.expectedTarget,
        .manifest = &manifest,
        .document = &documentInput,
        .limits = input.limits,
        .preAdmitted = preAdmitted,
        .cancellationFlag = cancellationFlag,
    };
    return executeSavePublication(coordinator, artifacts, publicationRequest, std::move(operation));
}

SessionSaveResult acceptSessionSavePublication(ProjectSession& session,
                                               SavePublicationResult publicationResult,
                                               const SessionPathIntentCapture intent,
                                               const document::Revision capturedRevision,
                                               const std::filesystem::path& targetPath) noexcept {
    if (!publicationResult) {
        return SessionSaveResult::publicationFailure(*publicationResult.failure(),
                                                     publicationResult.rejectDiagnostic());
    }

    const auto* publication = publicationResult.publication();
    const auto intentId = publicationResult.intentId().value_or(PublicationIntentId{});
    if (publication == nullptr || !publication->targetWasPublished()) {
        // Superseded / CancelledBeforePublication / ExternalModificationConflict /
        // FailedBeforePublication: session untouched, no acceptSavepoint() attempt (frozen
        // design: Save As abandonment on a non-published outcome is the caller's choice).
        return SessionSaveResult::unpublished(
            publication != nullptr ? *publication : platform::StagedArtifactPublicationResult{},
            intentId);
    }

    try {
        std::optional<ProjectDisplayPath> publishedPath;
        if (intent.kind() == SessionPathIntentKind::ReplacementPath) {
            publishedPath = ProjectDisplayPath::create(targetPath);
        }
        const auto savepointStatus =
            session.acceptSavepoint(intent, intentId, capturedRevision, publishedPath);
        return SessionSaveResult::published(*publication, intentId, savepointStatus);
    } catch (const std::bad_alloc&) {
        // The file was already durably published (`*publication`/`intentId` are both still in
        // scope and name the real replacement); only the LOCAL acceptSavepoint() bookkeeping
        // allocation failed before returning a status. Never relabeled as a publication failure
        // or silently dropped -- see SessionSaveResult::publishedSavepointBookkeepingFailed()'s
        // comment for the truthfulness principle this preserves.
        return SessionSaveResult::publishedSavepointBookkeepingFailed(
            *publication, intentId, SessionSaveSavepointBookkeepingFailure::ResourceExhausted);
    } catch (...) {
        return SessionSaveResult::publishedSavepointBookkeepingFailed(
            *publication, intentId, SessionSaveSavepointBookkeepingFailure::UnexpectedFailure);
    }
}

SessionSaveResult saveProjectSession(ProjectSession& session, PublicationCoordinator& coordinator,
                                     platform::StagedArtifactCoordinator& artifacts,
                                     const SessionSaveRequest& request,
                                     project::ProjectIoOperationMemory operation) noexcept {
    std::optional<SessionSaveOwningInput> owning;
    try {
        SessionSaveInputResult captured = session.captureSaveInput(request.intent);
        if (!captured) {
            return SessionSaveResult::captureFailure(captured.status());
        }
        owning.emplace(SessionSaveOwningInput{
            .capturedInput = std::move(captured).takeValue(),
            .targetPath = request.targetPath,
            .overwritePolicy = request.overwritePolicy,
            .expectedTarget = request.expectedTarget,
            .limits = request.limits,
        });
    } catch (const std::bad_alloc&) {
        return SessionSaveResult::captureFailure(SessionSaveResourceExhausted{});
    } catch (...) {
        return SessionSaveResult::captureFailure(SessionSaveUnexpectedFailure{});
    }

    const auto intent = owning->capturedInput.pathIntent();
    const auto capturedRevision = owning->capturedInput.revision();
    auto publicationResult =
        executeSessionSaveMiddle(coordinator, artifacts, *owning, request.preAdmitted,
                                 request.cancellationFlag, std::move(operation));
    return acceptSessionSavePublication(session, std::move(publicationResult), intent,
                                        capturedRevision, owning->targetPath);
}

} // namespace bloom::host
