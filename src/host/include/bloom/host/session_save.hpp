#pragma once

#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/save_publication.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <variant>

// The synchronous session-level Save orchestration: composes ProjectSession::captureSaveInput()
// (docs/architecture/project-session.md, "Save Inputs And Intent") with executeSavePublication()
// (save_publication.hpp -- the application PublicationCoordinator ordered against the platform
// StagedArtifactCoordinator over project::stageSaveArchive()) and, on a published outcome, the
// existing ProjectSession::acceptSavepoint() seam (project-session.md, "Save Result Acceptance").
// This module owns none of those three seams' own semantics; it only wires them together on the
// caller's thread. An asynchronous/Jobs-driven Save is a later slice (see AGENTS.md's boundary and
// this task's non-goals): every step here runs synchronously on the calling thread.
namespace bloom::host {

// Where a saveProjectSession() call stopped. Capture: captureSaveInput() itself did not produce a
// value (see SessionSaveResult::captureOutcome()). Publication: executeSavePublication() reported
// a typed pipeline failure before ever calling publish() (see
// SessionSaveResult::publicationFailure()). Savepoint: the executor reached a real publication
// outcome (see SessionSaveResult::publication()) -- for Published/PublishedWithDurabilityWarning
// this includes the acceptSavepoint() attempt (SessionSaveResult::savepointStatus()), unless the
// local acceptSavepoint() call itself never completed (see
// SessionSaveResult::savepointBookkeepingFailure()); for every other outcome (Superseded/
// CancelledBeforePublication/ExternalModificationConflict/FailedBeforePublication) the session is
// left untouched and no acceptSavepoint() attempt is made (savepointStatus() is then absent) --
// per the frozen design, abandoning a Save As replacement intent after a non-published outcome is
// the CALLER's choice, never automatic here.
enum class SessionSaveStage : std::uint8_t {
    None,
    Capture,
    Publication,
    Savepoint,
};

// Payload types for a Capture-stage failure that is not one of ProjectSession::captureSaveInput()'s
// own typed SessionSaveInputStatus values: captureSaveInput() is not noexcept (it may copy
// document::ColorSettings and the retained-requirements vector, both real allocations), and this
// module's own entry point is noexcept, so a thrown std::bad_alloc (or, defensively, any other
// exception a composed throwing surface should never emit) is reported here rather than escaping.
struct SessionSaveResourceExhausted final {};
struct SessionSaveUnexpectedFailure final {};

using SessionSaveCaptureOutcome =
    std::variant<std::monostate, SessionSaveInputStatus, SessionSaveResourceExhausted,
                 SessionSaveUnexpectedFailure>;

// Names why a Savepoint-stage acceptSavepoint() call did not run to completion after a real
// Published/PublishedWithDurabilityWarning outcome. acceptSavepoint() is not itself noexcept (it
// can allocate -- e.g. a std::filesystem::path copy for a Save As replacement path), so its own
// std::bad_alloc (or, defensively, any other exception) is caught here rather than escaping this
// module's noexcept entry point. Crucially, the file WAS durably published at this point: a real
// replacement must never be reported as not-published just because the LOCAL bookkeeping call
// about it failed -- the same truthfulness principle the platform layer applies to a late
// cancellation against an already-visible replacement. See
// SessionSaveResult::publishedSavepointBookkeepingFailed().
enum class SessionSaveSavepointBookkeepingFailure : std::uint8_t {
    ResourceExhausted,
    UnexpectedFailure,
};

struct SessionSaveRequest final {
    std::filesystem::path targetPath;
    platform::ArtifactOverwritePolicy overwritePolicy =
        platform::ArtifactOverwritePolicy::CreateOrReplace;
    std::optional<platform::ArtifactTargetObservation> expectedTarget;
    project::SaveArchiveLimits limits;
    // Normally session.capturePlainSavePathIntent() or an already-obtained
    // session.advancePathIntentForSaveAs() capture; forwarded verbatim to both
    // captureSaveInput() and, on a published outcome, acceptSavepoint().
    SessionPathIntentCapture intent;
    // Same seam as SavePublicationRequest::preAdmitted; forwarded verbatim. Caller-owned; must
    // outlive the call; left moved-from afterward when non-null.
    PublicationAdmission* preAdmitted = nullptr;
    // Same seam as SavePublicationRequest::cancellationFlag; forwarded verbatim.
    const std::atomic_bool* cancellationFlag = nullptr;
};

// See SessionSaveStage above for the three stages this result distinguishes. Exactly one of
// captureOutcome()/publicationFailure()/publication() is populated, matching `stage()`. At
// Savepoint, savepointStatus() and savepointBookkeepingFailure() are mutually exclusive: at most
// one is present, and both are absent for a non-published outcome (unpublished()).
class [[nodiscard]] SessionSaveResult final {
  public:
    SessionSaveResult(SessionSaveResult&&) noexcept = default;
    SessionSaveResult& operator=(SessionSaveResult&&) noexcept = default;
    SessionSaveResult(const SessionSaveResult&) = delete;
    SessionSaveResult& operator=(const SessionSaveResult&) = delete;
    ~SessionSaveResult() = default;

    [[nodiscard]] static SessionSaveResult
    captureFailure(SessionSaveCaptureOutcome outcome) noexcept;
    [[nodiscard]] static SessionSaveResult
    publicationFailure(SavePublicationFailure failure,
                       std::optional<platform::StagedArtifactError> rejectDiagnostic) noexcept;
    // A publication outcome that never publishes (Superseded/CancelledBeforePublication/
    // ExternalModificationConflict/FailedBeforePublication): session untouched, no
    // acceptSavepoint() attempt, so savepointStatus() stays absent.
    [[nodiscard]] static SessionSaveResult
    unpublished(platform::StagedArtifactPublicationResult publication,
                PublicationIntentId intentId) noexcept;
    // Published/PublishedWithDurabilityWarning: acceptSavepoint() was attempted and its exact
    // status (Accepted or a refusal -- surfaced faithfully, never masked) is carried here.
    [[nodiscard]] static SessionSaveResult
    published(platform::StagedArtifactPublicationResult publication, PublicationIntentId intentId,
              ProjectSessionSavepointStatus savepointStatus) noexcept;
    // Published/PublishedWithDurabilityWarning, but the LOCAL acceptSavepoint() call itself never
    // returned a status (see SessionSaveSavepointBookkeepingFailure above). `publication`/
    // `intentId` name the real, already-durable replacement -- never relabeled as unpublished --
    // while operator bool() stays false and savepointStatus() stays absent, since no
    // ProjectSessionSavepointStatus was ever obtained.
    [[nodiscard]] static SessionSaveResult
    publishedSavepointBookkeepingFailed(platform::StagedArtifactPublicationResult publication,
                                        PublicationIntentId intentId,
                                        SessionSaveSavepointBookkeepingFailure failure) noexcept;

    // True only for the single fully-succeeded path: a Published/PublishedWithDurabilityWarning
    // outcome whose acceptSavepoint() call returned Accepted. Every other case -- including a
    // real file publish whose savepoint acceptance was refused -- is false here; inspect stage(),
    // publication(), and savepointStatus() for the exact typed reason.
    [[nodiscard]] explicit operator bool() const noexcept {
        return stage_ == SessionSaveStage::Savepoint && publication_.has_value() &&
               publication_->targetWasPublished() && savepointStatus_.has_value() &&
               *savepointStatus_ == ProjectSessionSavepointStatus::Accepted;
    }

    [[nodiscard]] SessionSaveStage stage() const noexcept { return stage_; }

    // Valid only when stage() == Capture.
    [[nodiscard]] const SessionSaveCaptureOutcome* captureOutcome() const& noexcept {
        return stage_ == SessionSaveStage::Capture ? &captureOutcome_ : nullptr;
    }
    [[nodiscard]] const SessionSaveCaptureOutcome* captureOutcome() const&& = delete;

    // Valid only when stage() == Publication (the executor's own typed pipeline failure,
    // verbatim; publish() was never called).
    [[nodiscard]] const SavePublicationFailure* publicationFailure() const& noexcept {
        return publicationFailure_.has_value() ? &*publicationFailure_ : nullptr;
    }
    [[nodiscard]] const SavePublicationFailure* publicationFailure() const&& = delete;
    [[nodiscard]] std::optional<platform::StagedArtifactError> rejectDiagnostic() const noexcept {
        return rejectDiagnostic_;
    }

    // Valid only when stage() == Savepoint (the executor reached a real publish() outcome).
    [[nodiscard]] const platform::StagedArtifactPublicationResult* publication() const& noexcept {
        return publication_.has_value() ? &*publication_ : nullptr;
    }
    [[nodiscard]] const platform::StagedArtifactPublicationResult* publication() const&& = delete;
    [[nodiscard]] std::optional<PublicationIntentId> intentId() const noexcept { return intentId_; }
    // Present only when publication()->targetWasPublished() triggered an acceptSavepoint() call
    // that ran to completion; absent for a non-published outcome (unpublished()) and for a
    // published outcome whose local acceptSavepoint() call itself failed (see
    // savepointBookkeepingFailure() below).
    [[nodiscard]] std::optional<ProjectSessionSavepointStatus> savepointStatus() const noexcept {
        return savepointStatus_;
    }
    // Present only for publishedSavepointBookkeepingFailed(): the file was durably published, but
    // the local acceptSavepoint() call never returned a status. See
    // SessionSaveSavepointBookkeepingFailure's comment above.
    [[nodiscard]] std::optional<SessionSaveSavepointBookkeepingFailure>
    savepointBookkeepingFailure() const noexcept {
        return savepointBookkeepingFailure_;
    }

  private:
    SessionSaveResult() = default;

    SessionSaveStage stage_ = SessionSaveStage::None;
    SessionSaveCaptureOutcome captureOutcome_;
    std::optional<SavePublicationFailure> publicationFailure_;
    std::optional<platform::StagedArtifactError> rejectDiagnostic_;
    std::optional<platform::StagedArtifactPublicationResult> publication_;
    std::optional<PublicationIntentId> intentId_;
    std::optional<ProjectSessionSavepointStatus> savepointStatus_;
    std::optional<SessionSaveSavepointBookkeepingFailure> savepointBookkeepingFailure_;
};

// ------------------------------------------------------------------------------------------------
// Task A1 (issue #68), frozen design decision 2: "session-free middles" so the synchronous
// saveProjectSession() below and the async save worker (session_async_io.cpp) drive the SAME code.
// A SessionSaveOwningInput is a fully self-contained snapshot -- everything captureSaveInput() and
// SessionSaveRequest's own by-value fields provide, with no remaining reference back to
// ProjectSession or to the caller's request object -- so it can cross a thread boundary and outlive
// the call that built it. `capturedInput` alone already owns the round-trip view (see
// SessionSaveInput's class comment): a copy of the session's std::shared_ptr<const
// project::RoundTripState>, kept alive independently of the session (design decision 1).
// ------------------------------------------------------------------------------------------------
struct SessionSaveOwningInput final {
    SessionSaveInput capturedInput;
    std::filesystem::path targetPath;
    platform::ArtifactOverwritePolicy overwritePolicy =
        platform::ArtifactOverwritePolicy::CreateOrReplace;
    std::optional<platform::ArtifactTargetObservation> expectedTarget;
    project::SaveArchiveLimits limits;
};

// The session-free save middle: manifest/document assembly (from `input.capturedInput`) ->
// executeSavePublication(). Owns no session reference at all. `preAdmitted`/`cancellationFlag` are
// forwarded verbatim to executeSavePublication(), exactly as SessionSaveRequest documents them --
// for the async save worker, `cancellationFlag` is the executor-side atomic the scheduler's
// CancellationToken is bridged onto (see session_async_io.hpp's cancellation-bridge documentation).
// noexcept: mirrors executeSavePublication() itself; nothing this function does beyond that call
// can throw (CanonicalManifestV1::requirements is a non-owning std::span over `input`'s own
// already- owned vector).
[[nodiscard]] SavePublicationResult executeSessionSaveMiddle(
    PublicationCoordinator& coordinator, platform::StagedArtifactCoordinator& artifacts,
    const SessionSaveOwningInput& input, PublicationAdmission* preAdmitted,
    const std::atomic_bool* cancellationFlag, project::ProjectIoOperationMemory operation) noexcept;

// The shared "accept" step: given a real SavePublicationResult reached via executeSessionSaveMiddle
// (synchronously below, or from a completed async worker in session_async_io.cpp's
// AsyncSessionSave::tryComplete()), decides the outcome and, on a published target, calls
// session.acceptSavepoint() -- exactly saveProjectSession()'s own tail, factored out so the
// synchronous entry point and the async completion drive identical logic. noexcept: mirrors
// saveProjectSession()'s own top-level noexcept guarantee (acceptSavepoint() is wrapped
// internally).
[[nodiscard]] SessionSaveResult
acceptSessionSavePublication(ProjectSession& session, SavePublicationResult publicationResult,
                             SessionPathIntentCapture intent, document::Revision capturedRevision,
                             const std::filesystem::path& targetPath) noexcept;

// Pipeline: session.captureSaveInput(request.intent) -> build a SessionSaveOwningInput ->
// executeSessionSaveMiddle() -> acceptSessionSavePublication() -- outcome handling exactly as
// documented on SessionSaveResult's factory functions above.
//
// On Published/PublishedWithDurabilityWarning, acceptSavepoint() is called with `request.intent`,
// the winning PublicationIntentId, the captured revision, and -- per the frozen design -- the
// request's target path for a ReplacementPath intent (nullopt for ExistingPath, matching
// acceptSavepoint()'s own path-authority contract). Save As abandonment on a non-published outcome
// is deliberately left to the caller (session.abandonSaveAsIntent()); this function never calls it.
//
// noexcept top level: neither captureSaveInput() nor building the owning input is itself noexcept
// (both can allocate -- copying document::ColorSettings/retained requirements/a
// std::filesystem::path -- and are wrapped accordingly; see SessionSaveResourceExhausted above).
// executeSessionSaveMiddle() and acceptSessionSavePublication() are already noexcept.
[[nodiscard]] SessionSaveResult
saveProjectSession(ProjectSession& session, PublicationCoordinator& coordinator,
                   platform::StagedArtifactCoordinator& artifacts,
                   const SessionSaveRequest& request,
                   project::ProjectIoOperationMemory operation) noexcept;

} // namespace bloom::host
