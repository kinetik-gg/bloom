#pragma once

#include <bloom/core/sha256.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/output/flat_exr_export_write.hpp>
#include <bloom/output/output_analysis_attempt.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/output/output_export_stage.hpp>
#include <bloom/output/output_limits.hpp>
#include <bloom/output/png_export_write.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <variant>

// docs/architecture/frame-output.md "Approval", "Immutable Export Request", "Non-Blocking
// Execution", and "Atomic Publication": the host-owned composition binding
// bloom::output::OutputAnalysisAttemptV1 (design decision 2) to the application-wide
// PublicationCoordinator + platform StagedArtifactCoordinator, mirroring
// bloom/host/copy_publication.hpp's admission -> preflight -> registerTarget -> stage -> guard ->
// publish(disposition) composition (design decision 1) -- the closest established shape: like Save
// Copy, an export publishes from already-retained products (the attempt's frame/report), not a
// freshly-evaluated document.
//
// Two temporal halves, per design decision 3 ("assign PublicationIntentId (host, via the
// coordinator, at APPROVAL time per the atomic-publication section) and freeze the immutable
// request"):
//   - approveFrameExportV1() runs ENTIRELY on the calling (authoring) thread: no evaluation,
//     hashing, color work, or filesystem access (frame-output.md "Non-Blocking Execution":
//     "Approval itself performs no evaluation, hashing, color work, or filesystem access"). It
//     checks byte-equality against the retained attempt's own digest, then performs the SAME
//     coordinator.admit()+registerTarget() pair copy_publication.hpp's executeCopyPublication()
//     performs internally -- just earlier in time, before a job is ever submitted -- producing a
//     move-only FrameExportRequestV1 that owns the resulting PublicationTargetClaim.
//   - executeExportPublication() is the approved job's own blocking-I/O publication stage (design
//     decision 4's node 3). It runs INSIDE a real bloom::runtime::TaskContext (TaskExecutor::
//     BlockingIo) rather than as a free synchronous function taking a raw cancellation flag like
//     executeSavePublication()/executeCopyPublication() do: bloom::output's F1 writer/verifier and
//     ProcessFrameSemanticIdentityV1Preparer all require a genuine runtime::CancellationToken,
//     which only a real TaskContext can mint (its constructor is private, friended only to
//     TaskContext) -- see the implementor's report for the full rationale. Design decision 4's
//     node 1 ("CPU output preflight") and node 2 ("EXR exposes retained rows directly, no
//     PreparingOutput work") are folded into the START of this ONE task's synchronous stage
//     sequence, exactly like executeSavePublication() folds its own Admission/Preflight/
//     Registration/Staging stages into one synchronous call on one executor -- "mirror the
//     established save/copy publication composition rather than inventing coordinator variants".
namespace bloom::host {

struct FrameExportLimitsV1 final {
    std::chrono::steady_clock::duration totalDeadline = std::chrono::hours(24);
    std::chrono::steady_clock::duration noProgressInterval = std::chrono::seconds(120);
    // The PNG preset's retained-prepared-bytes ceiling (frame-output.md's "Version 1 export
    // limits": "retained prepared PNG bytes | 256 MiB"). Ignored by the EXR preset, which retains
    // no prepared display/output buffer at all. A request may LOWER this ("a request may lower but
    // not raise them"); a higher value is clamped back down to the closed limit.
    std::uint64_t preparedPngByteLimit = output::kOutputExportPreparedPngBytesMaximumV1;
};

enum class FrameExportApprovalStatusV1 : std::uint8_t {
    Approved,
    NotApprovable,
    DigestMismatch,
    AdmissionFailed,
    RegistrationFailed,
};

class FrameExportRequestV1;

class [[nodiscard]] FrameExportApprovalResultV1 final {
  public:
    FrameExportApprovalResultV1(FrameExportApprovalResultV1&&) noexcept = default;
    FrameExportApprovalResultV1& operator=(FrameExportApprovalResultV1&&) noexcept = default;
    FrameExportApprovalResultV1(const FrameExportApprovalResultV1&) = delete;
    FrameExportApprovalResultV1& operator=(const FrameExportApprovalResultV1&) = delete;
    ~FrameExportApprovalResultV1();

    [[nodiscard]] FrameExportApprovalStatusV1 status() const noexcept { return status_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return status_ == FrameExportApprovalStatusV1::Approved;
    }
    // Valid, and non-null, only when *this is true. Moves the request out (single-use, mirroring
    // every other takeX()&& idiom in this module family).
    [[nodiscard]] std::unique_ptr<FrameExportRequestV1> takeRequest() && noexcept;
    [[nodiscard]] std::optional<PublicationAdmissionStatus> admissionFailure() const noexcept {
        return admissionFailure_;
    }
    [[nodiscard]] std::optional<PublicationRegistrationStatus>
    registrationFailure() const noexcept {
        return registrationFailure_;
    }

  private:
    friend FrameExportApprovalResultV1
    approveFrameExportV1(PublicationCoordinator&,
                         std::shared_ptr<const output::OutputAnalysisAttemptV1>, core::Sha256Digest,
                         FrameExportLimitsV1, PublicationAdmission*);

    explicit FrameExportApprovalResultV1(FrameExportApprovalStatusV1 status) noexcept;
    explicit FrameExportApprovalResultV1(std::unique_ptr<FrameExportRequestV1> request) noexcept;

    FrameExportApprovalStatusV1 status_;
    std::unique_ptr<FrameExportRequestV1> request_;
    std::optional<PublicationAdmissionStatus> admissionFailure_;
    std::optional<PublicationRegistrationStatus> registrationFailure_;
};

// Compares `approverDigest` byte-for-byte against `attempt->digest()` (frame-output.md: "Approval
// requires both that digest and all eleven derived permission bits" -- a caller cannot reduce this
// to `acceptLoss = true`). On an exact match, assigns the PublicationIntentId (coordinator.admit(),
// or takes ownership of `*preAdmitted` when non-null -- the identical seam
// SavePublicationRequest::preAdmitted documents) and registers it for the attempt's own retained
// target key, then freezes the immutable FrameExportRequestV1: no mutator, no re-evaluation path,
// export uses the retained frame/identity/report directly.
[[nodiscard]] FrameExportApprovalResultV1
approveFrameExportV1(PublicationCoordinator& coordinator,
                     std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt,
                     core::Sha256Digest approverDigest, FrameExportLimitsV1 limits = {},
                     PublicationAdmission* preAdmitted = nullptr);

// The immutable, move-only frozen request design decision 3 names: retains the completed attempt
// (and, through it, its resource reservation) and the PublicationTargetClaim approval registered.
// No accessor exposes a mutator; the only way to consume it is executeExportPublication(), which
// takes it by value.
class FrameExportRequestV1 final {
  public:
    FrameExportRequestV1(const FrameExportRequestV1&) = delete;
    FrameExportRequestV1& operator=(const FrameExportRequestV1&) = delete;
    FrameExportRequestV1(FrameExportRequestV1&&) noexcept = default;
    FrameExportRequestV1& operator=(FrameExportRequestV1&&) noexcept = default;
    ~FrameExportRequestV1() = default;

    [[nodiscard]] const std::shared_ptr<const output::OutputAnalysisAttemptV1>&
    attempt() const& noexcept {
        return attempt_;
    }
    [[nodiscard]] const std::shared_ptr<const output::OutputAnalysisAttemptV1>&
    attempt() const&& = delete;
    [[nodiscard]] PublicationIntentId intentId() const noexcept { return claim_.intentId(); }
    [[nodiscard]] const FrameExportLimitsV1& limits() const noexcept { return limits_; }

  private:
    friend FrameExportApprovalResultV1
    approveFrameExportV1(PublicationCoordinator&,
                         std::shared_ptr<const output::OutputAnalysisAttemptV1>, core::Sha256Digest,
                         FrameExportLimitsV1, PublicationAdmission*);
    friend class FrameExportRequestAccessV1;

    FrameExportRequestV1(std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt,
                         PublicationTargetClaim claim, FrameExportLimitsV1 limits) noexcept;

    std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt_;
    PublicationTargetClaim claim_;
    FrameExportLimitsV1 limits_;
};

// Test/executor-only accessor: executeExportPublication() needs to consume `claim_` by value
// (tryEnterPublication() below); kept as a dedicated friend accessor type (rather than a public
// method) so nothing outside this module's own executor can reach the claim.
class FrameExportRequestAccessV1 final {
  public:
    [[nodiscard]] static PublicationTargetClaim& claim(FrameExportRequestV1& request) noexcept {
        return request.claim_;
    }
};

enum class FrameExportPublicationStageV1 : std::uint8_t {
    Preflight,
    Staging,
    // PNG only: the retained qualified processor applied to the retained process frame in bounded
    // chunks. EXR skips it entirely (frame-output.md: "EXR skips ColorPreparing").
    ColorPreparing,
    Writing,
    Verifying,
    ArtifactCopy,
    Guard,
    Cancelled,
};

struct FrameExportDeadlineExceededV1 final {};
struct FrameExportNoProgressExceededV1 final {};
struct FrameExportResourceExhaustedV1 final {};
// The PNG prepared straight-RGBA8 stream this export would retain exceeds the effective
// retained-prepared-PNG-bytes limit. Distinct from FrameExportResourceExhaustedV1 (which reports a
// refused ledger reservation) because this one names a specific closed per-preset limit.
struct FrameExportPreparedBytesExceededV1 final {};
struct FrameExportUnexpectedFailureV1 final {};

using FrameExportPublicationFailurePayloadV1 =
    std::variant<std::monostate, platform::StagedArtifactError,
                 output::FlatExrExportWriteErrorCodeV1, output::PngExportWriteErrorCodeV1,
                 PublicationGuardStatus, FrameExportDeadlineExceededV1,
                 FrameExportNoProgressExceededV1, FrameExportResourceExhaustedV1,
                 FrameExportPreparedBytesExceededV1, FrameExportUnexpectedFailureV1>;

class FrameExportPublicationFailureV1 final {
  public:
    FrameExportPublicationFailureV1() = default;
    template <typename Payload>
    FrameExportPublicationFailureV1(const FrameExportPublicationStageV1 stage, Payload payload)
        : stage_(stage), payload_(std::move(payload)) {}

    [[nodiscard]] FrameExportPublicationStageV1 stage() const noexcept { return stage_; }
    [[nodiscard]] const FrameExportPublicationFailurePayloadV1& payload() const noexcept {
        return payload_;
    }
    template <typename Payload> [[nodiscard]] const Payload* payloadAs() const noexcept {
        return std::get_if<Payload>(&payload_);
    }

  private:
    FrameExportPublicationStageV1 stage_ = FrameExportPublicationStageV1::Preflight;
    FrameExportPublicationFailurePayloadV1 payload_;
};

// Mirrors SavePublicationResult/CopyPublicationResult exactly: `publication()` is set only when
// the pipeline actually reached `lease.publish()`. On a Published/PublishedWithDurabilityWarning
// outcome, `semanticDigest()`/`artifactDigest()`/`artifactByteCount()` are also set (design
// decision 4's "artifact SHA-256 + flush"; "artifact digest surfaced" test requirement).
class [[nodiscard]] FrameExportPublicationResultV1 final {
  public:
    FrameExportPublicationResultV1(FrameExportPublicationResultV1&&) noexcept = default;
    FrameExportPublicationResultV1& operator=(FrameExportPublicationResultV1&&) noexcept = default;
    FrameExportPublicationResultV1(const FrameExportPublicationResultV1&) = delete;
    FrameExportPublicationResultV1& operator=(const FrameExportPublicationResultV1&) = delete;
    ~FrameExportPublicationResultV1() = default;

    [[nodiscard]] static FrameExportPublicationResultV1
    published(platform::StagedArtifactPublicationResult publication, PublicationIntentId intentId,
              std::optional<core::Sha256Digest> semanticDigest,
              std::optional<core::Sha256Digest> artifactDigest,
              std::uint64_t artifactByteCount) noexcept;
    [[nodiscard]] static FrameExportPublicationResultV1
    failure(FrameExportPublicationFailureV1 failure,
            std::optional<platform::StagedArtifactError> rejectDiagnostic = std::nullopt) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept { return !failure_.has_value(); }
    [[nodiscard]] const platform::StagedArtifactPublicationResult* publication() const& noexcept {
        return publication_.has_value() ? &*publication_ : nullptr;
    }
    [[nodiscard]] const platform::StagedArtifactPublicationResult* publication() const&& = delete;
    [[nodiscard]] std::optional<PublicationIntentId> intentId() const noexcept { return intentId_; }
    [[nodiscard]] std::optional<core::Sha256Digest> semanticDigest() const noexcept {
        return semanticDigest_;
    }
    [[nodiscard]] std::optional<core::Sha256Digest> artifactDigest() const noexcept {
        return artifactDigest_;
    }
    [[nodiscard]] std::uint64_t artifactByteCount() const noexcept { return artifactByteCount_; }
    [[nodiscard]] const FrameExportPublicationFailureV1* failure() const& noexcept {
        return failure_.has_value() ? &*failure_ : nullptr;
    }
    [[nodiscard]] const FrameExportPublicationFailureV1* failure() const&& = delete;
    [[nodiscard]] std::optional<platform::StagedArtifactError> rejectDiagnostic() const noexcept {
        return rejectDiagnostic_;
    }

  private:
    FrameExportPublicationResultV1() = default;

    std::optional<platform::StagedArtifactPublicationResult> publication_;
    std::optional<PublicationIntentId> intentId_;
    std::optional<core::Sha256Digest> semanticDigest_;
    std::optional<core::Sha256Digest> artifactDigest_;
    std::uint64_t artifactByteCount_ = 0;
    std::optional<FrameExportPublicationFailureV1> failure_;
    std::optional<platform::StagedArtifactError> rejectDiagnostic_;
};

// Runs the approved export job's blocking-I/O publication stage. See the namespace-level comment
// above for why this takes a real runtime::TaskContext& (F1/identity-preparer cancellation) rather
// than a raw atomic flag, and for how design decision 4's preflight/PreparingOutput nodes fold into
// this one stage sequence. `scratchDirectory` is a caller-owned directory (never the real
// destination's parent, never tracked by StagedArtifactCoordinator) this call uses for its
// bridging Writing/Verifying scratch file (bloom/output/flat_exr_export_write.hpp); the scratch
// file is always removed before this call returns, on every path. `clock` is design decision 5's
// injectable monotonic clock (defaults to the task system's own std::chrono::steady_clock source).
// Approved-job resource admission (design decision 5) transactionally expands the SAME
// output::ExportResourceReservationV1 the attempt already owns (reached through
// `request.attempt()->resources()`); no separate ExportResourceLedgerV1 reference is needed here.
[[nodiscard]] FrameExportPublicationResultV1 executeExportPublication(
    runtime::TaskContext& context, platform::StagedArtifactCoordinator& artifacts,
    FrameExportRequestV1 request, const std::filesystem::path& scratchDirectory,
    output::OutputExportClockV1 clock = output::systemOutputExportClockV1(),
    output::OutputExportProgressCallbackV1 progress = {}) noexcept;

} // namespace bloom::host
