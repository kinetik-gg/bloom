#pragma once

#include <bloom/host/publication_coordinator.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/staged_save.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <variant>

// The application-host composition of docs/architecture/project-format.md's "Staging And Atomic
// Publication" nine-step sequence with docs/architecture/project-session.md's "Per-Target
// Ordering And Publication": this module owns steps 1-2 (admission and preflight) and 6-8
// (publication ordering against the application-wide PublicationCoordinator, then the platform
// publication lease), composing project::stageSaveArchive() (steps 3-5) and
// platform::StagedArtifactLease::publish() (the rest of 6-8) in between. It is the synchronous,
// caller-threaded executor primitive only: session/executor threading, Save As path-authority
// state, and session result acceptance are a later slice (see docs/architecture/project-session.md
// "Save Result Acceptance").
namespace bloom::host {

// Every stage this executor's own pipeline can fail at, distinct from the outcome of a publish()
// call that was actually reached (see SavePublicationResult::publication()). "Cancelled" is one
// stage shared by every cancellation checkpoint between admission and the publication guard: the
// checkpoint that observed the flag is not separately distinguished because none of them run
// publish() and the caller-observable contract (no replacement occurred) is identical at all of
// them.
enum class SavePublicationStage : std::uint8_t {
    None,
    Admission,
    Preflight,
    Registration,
    Staging,
    StagedSave,
    Guard,
    Cancelled,
};

// SavePublicationStage::ResourceExhausted/UnexpectedFailure are not stage-scoped enumerators of
// their own; a resource-exhausted or unexpected-exception failure is reported at whichever
// SavePublicationStage was current when it happened, the same established pattern as
// bloom/project/staged_save.hpp's StagedSaveStage tracking. These two payload types name that a
// failure of this kind occurred without duplicating one payload type per stage.
struct SavePublicationResourceExhausted final {};
struct SavePublicationUnexpectedFailure final {};

// Guard statuses other than Entered/Superseded (InvalidClaim/TargetBusy/AlreadyEntered) are
// internal-protocol failures for this single-executor composition: this function's own claim is
// always freshly registered and entered at most once, so InvalidClaim/AlreadyEntered cannot occur,
// and no other guard is ever held concurrently against the same claim's target from within one
// call, so TargetBusy cannot occur either. They are still handled (typed failure, no publish()
// call) because a concurrent multi-executor flow -- out of scope here -- can reach them; see the
// implementor's report for the full unreachability argument.
using SavePublicationFailurePayload =
    std::variant<std::monostate, PublicationAdmissionStatus, platform::StagedArtifactError,
                 PublicationRegistrationStatus, project::StagedSaveFailure, PublicationGuardStatus,
                 SavePublicationResourceExhausted, SavePublicationUnexpectedFailure>;

class SavePublicationFailure final {
  public:
    SavePublicationFailure() = default;

    template <typename Payload>
    SavePublicationFailure(const SavePublicationStage stage, Payload payload)
        : stage_(stage), payload_(std::move(payload)) {}

    [[nodiscard]] SavePublicationStage stage() const noexcept { return stage_; }
    [[nodiscard]] const SavePublicationFailurePayload& payload() const noexcept { return payload_; }

    template <typename Payload> [[nodiscard]] const Payload* payloadAs() const noexcept {
        return std::get_if<Payload>(&payload_);
    }

  private:
    SavePublicationStage stage_ = SavePublicationStage::None;
    SavePublicationFailurePayload payload_;
};

// The synchronous save-publication executor's result. Exactly one of `failure()`/`publication()`
// is non-null: `publication()` is set only for the two guard outcomes (Entered, Superseded) that
// actually call platform::StagedArtifactLease::publish(); every earlier pipeline stage reports a
// typed SavePublicationFailure instead and never calls publish().
class [[nodiscard]] SavePublicationResult final {
  public:
    SavePublicationResult(SavePublicationResult&&) noexcept = default;
    SavePublicationResult& operator=(SavePublicationResult&&) noexcept = default;
    SavePublicationResult(const SavePublicationResult&) = delete;
    SavePublicationResult& operator=(const SavePublicationResult&) = delete;
    ~SavePublicationResult() = default;

    // The pipeline reached and called lease.publish(): `publication` carries the platform's exact
    // six-outcome contract (see project-format.md's outcome table) and `intentId` is this
    // executor's own claim's PublicationIntentId -- the winning intent on Entered/Published, the
    // superseded loser on Superseded -- for the caller's session bookkeeping (the future session
    // layer keys its own in-flight Save operation by the intent its admission was assigned).
    [[nodiscard]] static SavePublicationResult
    published(platform::StagedArtifactPublicationResult publication,
              PublicationIntentId intentId) noexcept;

    // A typed pipeline failure before publish() was ever called. `rejectDiagnostic`, when set,
    // is StagedSaveResult::rejectDiagnostic() forwarded verbatim (SavePublicationStage::StagedSave
    // only): a secondary diagnostic from a lease.rejectVerification() call that itself failed,
    // never the primary reason staging failed.
    [[nodiscard]] static SavePublicationResult
    failure(SavePublicationFailure failure,
            std::optional<platform::StagedArtifactError> rejectDiagnostic = std::nullopt) noexcept;

    // True when the pipeline reached publish() without an internal protocol failure -- NOT
    // whether the target was actually replaced (Superseded/ExternalModificationConflict/
    // FailedBeforePublication also return true here). Check publication()->targetWasPublished()
    // for that.
    [[nodiscard]] explicit operator bool() const noexcept { return !failure_.has_value(); }

    [[nodiscard]] const platform::StagedArtifactPublicationResult* publication() const& noexcept {
        return publication_.has_value() ? &*publication_ : nullptr;
    }
    [[nodiscard]] const platform::StagedArtifactPublicationResult* publication() const&& = delete;
    [[nodiscard]] std::optional<PublicationIntentId> intentId() const noexcept { return intentId_; }
    [[nodiscard]] const SavePublicationFailure* failure() const& noexcept {
        return failure_.has_value() ? &*failure_ : nullptr;
    }
    [[nodiscard]] const SavePublicationFailure* failure() const&& = delete;
    [[nodiscard]] std::optional<platform::StagedArtifactError> rejectDiagnostic() const noexcept {
        return rejectDiagnostic_;
    }

  private:
    SavePublicationResult() = default;

    std::optional<platform::StagedArtifactPublicationResult> publication_;
    std::optional<PublicationIntentId> intentId_;
    std::optional<SavePublicationFailure> failure_;
    std::optional<platform::StagedArtifactError> rejectDiagnostic_;
};

struct SavePublicationRequest final {
    std::filesystem::path targetPath;
    platform::ArtifactOverwritePolicy overwritePolicy =
        platform::ArtifactOverwritePolicy::CreateOrReplace;
    std::optional<platform::ArtifactTargetObservation> expectedTarget;
    const project::CanonicalManifestV1* manifest = nullptr; // non-owning, caller-kept-alive
    const project::CanonicalDocumentV1* document = nullptr; // non-owning, caller-kept-alive
    project::SaveArchiveLimits limits;

    // When set, executeSavePublication() takes ownership of *preAdmitted (moves from it) instead
    // of calling coordinator.admit() itself, and never touches admission internally otherwise.
    // The contract assigns a Save's PublicationIntentId at admission time, before path
    // resolution/worker dispatch -- the future session layer admits at Save-request time on its
    // own thread and hands the admission to this synchronous executor later, so this seam is not
    // solely a test convenience. Caller-owned; must outlive the call; left moved-from afterward.
    PublicationAdmission* preAdmitted = nullptr;

    // Optional caller-checked cancellation, observed between pipeline stages up to (never after)
    // the publication guard attempt; a caller-owned atomic checked at each boundary. Left null,
    // cancellation is never observed here -- full session-level cancellation semantics are a
    // later slice.
    const std::atomic_bool* cancellationFlag = nullptr;
};

// See the namespace-level comment above for the composition this performs. noexcept top level;
// an internal std::bad_alloc or other exception is reported as a typed
// SavePublicationResourceExhausted/UnexpectedFailure failure at whichever stage was current (the
// established bloom/project/staged_save.hpp pattern). RAII (PublicationAdmission/
// PublicationTargetClaim/PublicationGuard/StagedArtifactTarget/StagedArtifactLease) unwinds
// correctly on every path; a run that never reaches publish() leaves both the coordinator and the
// platform artifact coordinator with no unresolved admissions, claims, guards, or active targets
// once this call returns and every handle it produced has been dropped.
[[nodiscard]] SavePublicationResult executeSavePublication(
    PublicationCoordinator& coordinator, platform::StagedArtifactCoordinator& artifacts,
    const SavePublicationRequest& request, project::ProjectIoOperationMemory operation) noexcept;

} // namespace bloom::host
