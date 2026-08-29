#pragma once

#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>

#include <cstdint>
#include <optional>
#include <variant>

// Staged execution of the save/reopen chain over a platform::StagedArtifactLease -- the "steps
// 3-5 and 9" half of docs/architecture/project-format.md's "Staging And Atomic Publication" that
// this package owns (steps 1-2, the preflight, and steps 6-8, publication ordering, belong to the
// application-layer caller; see that document's "Staging And Atomic Publication" section for the
// complete nine-step sequence this is embedded in).
namespace bloom::project {

enum class StagedSaveStage : std::uint8_t {
    None,
    // buildSaveArchive() failed while encoding manifest.json/document.json or writing the
    // in-memory archive. Payload: SaveArchiveFailure (its own .stage() names the exact encode
    // step).
    Build,
    // lease.write(archiveBytes) failed. Payload: StagedSavePlatformFailure.
    StageWrite,
    // lease.finishWriting() failed. Payload: StagedSavePlatformFailure.
    StageFinish,
    // lease.stageBytes() disagreed with the built archive's size, or exceeded
    // limits.container.maxArchiveBytes, before any read-back allocation was attempted. Payload:
    // StagedSaveSizeDisagreement.
    StagedSizeDisagreement,
    // The PMR-charged read-back buffer (sized to the built archive) could not be allocated within
    // `operation`'s budget. Payload: SaveArchiveResourceExhausted.
    ReadBackAllocation,
    // lease.readForVerification() failed, or returned a short/zero/over-sized read inconsistent
    // with the requested bound. Payload: StagedSavePlatformFailure.
    StageRead,
    // verifySaveArchive() over the staged/read-back bytes failed. lease.rejectVerification() was
    // already called (best-effort; see StagedSaveResult::rejectDiagnostic()). Payload:
    // SaveArchiveFailure (its own .stage() names the exact verification step).
    Verification,
    // lease.acceptVerification() failed. Payload: StagedSavePlatformFailure.
    Accept,
};

// Names the exact StagedArtifactLease call a StagedSaveStage::StageWrite/StageFinish/StageRead/
// Accept platform-stage failure occurred at.
enum class StagedSaveLeaseCall : std::uint8_t {
    None,
    Write,
    FinishWriting,
    ReadForVerification,
    AcceptVerification,
};

struct StagedSavePlatformFailure final {
    platform::StagedArtifactError error = platform::StagedArtifactError::None;
    StagedSaveLeaseCall call = StagedSaveLeaseCall::None;
};

struct StagedSaveSizeDisagreement final {
    std::uint64_t builtArchiveBytes = 0;
    std::uint64_t stageBytes = 0;
};

using StagedSaveFailurePayload =
    std::variant<std::monostate, SaveArchiveFailure, StagedSavePlatformFailure,
                 StagedSaveSizeDisagreement, SaveArchiveResourceExhausted,
                 SaveArchiveUnexpectedFailure>;

class StagedSaveFailure final {
  public:
    StagedSaveFailure() = default;

    template <typename Payload>
    StagedSaveFailure(const StagedSaveStage stage, Payload payload)
        : stage_(stage), payload_(std::move(payload)) {}

    [[nodiscard]] StagedSaveStage stage() const noexcept { return stage_; }
    [[nodiscard]] const StagedSaveFailurePayload& payload() const noexcept { return payload_; }

    template <typename Payload> [[nodiscard]] const Payload* payloadAs() const noexcept {
        return std::get_if<Payload>(&payload_);
    }

  private:
    StagedSaveStage stage_ = StagedSaveStage::None;
    StagedSaveFailurePayload payload_;
};

class [[nodiscard]] StagedSaveResult final {
  public:
    StagedSaveResult(StagedSaveResult&&) noexcept = default;
    StagedSaveResult& operator=(StagedSaveResult&&) noexcept = default;
    StagedSaveResult(const StagedSaveResult&) = delete;
    StagedSaveResult& operator=(const StagedSaveResult&) = delete;
    ~StagedSaveResult() = default;

    [[nodiscard]] static StagedSaveResult success() noexcept;
    // `rejectDiagnostic`, when set, names a StagedArtifactError from a lease.rejectVerification()
    // call that itself failed while reacting to this failure (StagedSaveStage::Verification
    // only). It is always secondary to `failure` and never replaces it: rejectVerification()'s
    // own failure is best-effort diagnostic information, not the reason staging failed.
    [[nodiscard]] static StagedSaveResult
    failure(StagedSaveFailure failure,
            std::optional<platform::StagedArtifactError> rejectDiagnostic = std::nullopt);

    [[nodiscard]] explicit operator bool() const noexcept { return !failure_.has_value(); }
    [[nodiscard]] const StagedSaveFailure* failure() const& noexcept {
        return failure_.has_value() ? &*failure_ : nullptr;
    }
    [[nodiscard]] const StagedSaveFailure* failure() const&& = delete;
    [[nodiscard]] std::optional<platform::StagedArtifactError> rejectDiagnostic() const noexcept {
        return rejectDiagnostic_;
    }

  private:
    StagedSaveResult() = default;

    std::optional<StagedSaveFailure> failure_;
    std::optional<platform::StagedArtifactError> rejectDiagnostic_;
};

// Drives the composed build/write/verify chain over an already-staged platform::StagedArtifactLease
// (the caller has already run the coordinator's preflight() and stage(), i.e.
// project-format.md's steps 1-2): builds the archive in memory (no in-memory verification), writes
// it to the stage, reads the staged bytes back, and runs the exact same verifySaveArchive() chain
// Save always ran -- but over those staged/read-back bytes rather than the in-memory ones, because
// the verification that matters is over what was actually durably staged. On success the lease has
// accepted verification and is exactly one lease.publish() away from durable publication; this
// function NEVER calls publish() itself -- publication ordering (steps 6-8, the winning
// PublicationIntentId, disposition) is the application-layer publication coordinator's job, not
// this module's.
//
// On any failure, the lease is left in a state its destructor can safely clean up (RAII owns
// stage cleanup; this function does not attempt to clean the stage itself, except that a
// verification-content failure calls lease.rejectVerification() as documented above). Top-level
// noexcept: an internal std::bad_alloc or budget rejection is reported as a typed
// SaveArchiveResourceExhausted/ResourceExhausted-equivalent failure at whichever stage was current.
[[nodiscard]] StagedSaveResult stageSaveArchive(platform::StagedArtifactLease& lease,
                                                const CanonicalManifestV1& manifest,
                                                const CanonicalDocumentV1& document,
                                                const SaveArchiveLimits& limits,
                                                ProjectIoOperationMemory operation) noexcept;

} // namespace bloom::project
