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

namespace {

// The one allocation this module performs itself, ahead of executeSavePublication() (which is
// already noexcept and reports its own allocation failures as a typed SavePublicationFailure): a
// copy of the captured retained-requirements vector, kept alive for CanonicalManifestV1's span for
// the duration of the executeSavePublication() call. A failure here is attributed to the
// Publication stage at SavePublicationStage::None -- honestly "failed before the publication
// pipeline was ever entered, while assembling its input" -- reusing SavePublicationFailure's own
// resource-exhausted payload rather than inventing a fourth stage for one allocation.
[[nodiscard]] SessionSaveResult assemblyResourceExhausted() noexcept {
    return SessionSaveResult::publicationFailure(
        SavePublicationFailure(SavePublicationStage::None, SavePublicationResourceExhausted{}),
        std::nullopt);
}

} // namespace

SessionSaveResult saveProjectSession(ProjectSession& session, PublicationCoordinator& coordinator,
                                     platform::StagedArtifactCoordinator& artifacts,
                                     const SessionSaveRequest& request,
                                     project::ProjectIoOperationMemory operation) noexcept {
    std::optional<SessionSaveInputResult> captured;
    try {
        captured.emplace(session.captureSaveInput(request.intent));
    } catch (const std::bad_alloc&) {
        return SessionSaveResult::captureFailure(SessionSaveResourceExhausted{});
    } catch (...) {
        return SessionSaveResult::captureFailure(SessionSaveUnexpectedFailure{});
    }
    if (!*captured) {
        return SessionSaveResult::captureFailure(captured->status());
    }
    const auto* input = captured->value();
    if (input == nullptr) {
        // Unreachable: `*captured` above already proved status() == Captured, which guarantees
        // value() is non-null (see SessionSaveInputResult::operator bool()). Handled anyway so
        // this function's control flow stays provably total.
        return SessionSaveResult::captureFailure(SessionSaveUnexpectedFailure{});
    }

    std::vector<project::ManifestRequirement> requirements;
    try {
        requirements = input->retainedRequirements();
    } catch (const std::bad_alloc&) {
        return assemblyResourceExhausted();
    }

    const project::CanonicalManifestV1 manifest{
        .documentSchemaVersion = {1, input->schemaMinor()},
        .requirements = requirements,
    };
    const project::CanonicalDocumentV1 documentInput{
        .snapshot = &input->snapshot(),
        .colorSettings = &input->colorSettings(),
        .roundTrip = input->roundTrip(),
        .schemaMinor = input->schemaMinor(),
    };
    const SavePublicationRequest publicationRequest{
        .targetPath = request.targetPath,
        .overwritePolicy = request.overwritePolicy,
        .expectedTarget = request.expectedTarget,
        .manifest = &manifest,
        .document = &documentInput,
        .limits = request.limits,
        .preAdmitted = request.preAdmitted,
        .cancellationFlag = request.cancellationFlag,
    };

    auto publicationResult =
        executeSavePublication(coordinator, artifacts, publicationRequest, std::move(operation));
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
        if (request.intent.kind() == SessionPathIntentKind::ReplacementPath) {
            publishedPath = ProjectDisplayPath::create(request.targetPath);
        }
        const auto savepointStatus =
            session.acceptSavepoint(request.intent, intentId, input->revision(), publishedPath);
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

} // namespace bloom::host
