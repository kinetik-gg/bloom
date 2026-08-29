#pragma once

#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/staged_save.hpp>
#include <bloom/project/zip_container.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

// Staged execution of the Save Copy chain over a platform::StagedArtifactLease -- the byte-copy
// sibling of staged_save.hpp's save/reopen chain (docs/architecture/project-session.md: "Save
// Copy stages, validates, and atomically publishes an asynchronous byte-for-byte copy and never
// claims to rewrite or migrate the document"). Unlike stageSaveArchive(), there is no build step:
// the caller already holds the exact bytes to publish (an already-opened preserved-read-only
// project's bounded original archive), so this module writes them to the stage verbatim, reads
// them back, and verifies the round trip two ways: (a) the read-back bytes are byte-identical to
// the source (the contract's literal claim), and (b) the read-back bytes still preflight as a
// conforming Constrained ZIP Profile container -- a cheap, allocation-free structural sanity check
// that catches disk-level corruption of the staged copy distinctly from a byte inequality, exactly
// as this module's own implementor's report explains. As with stageSaveArchive(), this function
// NEVER calls publish() -- publication ordering belongs to the application-layer executor
// (bloom::host::executeCopyPublication).
namespace bloom::project {

enum class StagedCopyStage : std::uint8_t {
    None,
    // lease.write(sourceBytes) failed. Payload: StagedSavePlatformFailure (reused verbatim from
    // staged_save.hpp -- see that header's own StagedSavePlatformFailure/StagedSaveLeaseCall for
    // why this shape is generic enough to import unchanged rather than duplicate).
    StageWrite,
    // lease.finishWriting() failed. Payload: StagedSavePlatformFailure.
    StageFinish,
    // lease.stageBytes() disagreed with sourceBytes.size(), or sourceBytes.size() exceeded
    // limits.maxArchiveBytes, before any read-back allocation was attempted. Payload:
    // StagedCopySizeDisagreement.
    StagedSizeDisagreement,
    // The PMR-charged read-back buffer (sized to sourceBytes) could not be allocated within
    // `operation`'s budget. Payload: StagedCopyResourceExhausted.
    ReadBackAllocation,
    // lease.readForVerification() failed, or returned a short/zero/over-sized read inconsistent
    // with the requested bound. Payload: StagedSavePlatformFailure.
    StageRead,
    // The round-trip verification over the staged/read-back bytes failed: either (a) the read-back
    // bytes are not byte-identical to `sourceBytes` (payload StagedCopyByteMismatch), or (b) they
    // no longer preflight as a conforming Constrained ZIP Profile container (payload
    // StagedCopyContainerSanityFailure). lease.rejectVerification() was already called
    // (best-effort; see StagedCopyResult::rejectDiagnostic()).
    Verification,
    // lease.acceptVerification() failed. Payload: StagedSavePlatformFailure.
    Accept,
};

struct StagedCopySizeDisagreement final {
    std::uint64_t sourceBytes = 0;
    std::uint64_t stageBytes = 0;
};

// The first byte offset (within `sourceBytes`/the read-back buffer) at which the two disagree.
// Reachability note: with a real platform::StagedArtifactCoordinator, lease.write(sourceBytes)
// followed by lease.readForVerification() reading back exactly what was written cannot itself
// disagree without genuine disk-level corruption between finishWriting() and the read-back loop --
// there is no public seam (and the task package forbids adding a test-only one) to desynchronize
// the two in-process. See the implementor's report for the full unreachability argument; this type
// exists for the real (if practically unobserved) disk-corruption case the design calls for.
struct StagedCopyByteMismatch final {
    std::uint64_t byteOffset = 0;
};

// `error` is bloom::project::ZipContainerError (zip_container.hpp's public enum, already reachable
// from this header transitively) rather than the private allocation-free scanner's own
// zip_container_preflight.hpp-local enum, so this stays a public-only contract; staged_copy.cpp
// translates the scanner's result before returning it here.
struct StagedCopyContainerSanityFailure final {
    ZipContainerError error = ZipContainerError::None;
    std::size_t byteOffset = 0;
};

struct StagedCopyResourceExhausted final {};
struct StagedCopyUnexpectedFailure final {};

// Verification-stage payload is exactly one of StagedCopyByteMismatch (check (a)) or
// StagedCopyContainerSanityFailure (check (b)) -- flat alternatives of the same variant every other
// stage's payload lives in, matching bloom/project/save_archive.hpp's SaveArchiveFailurePayload
// precedent of one flat variant across every stage rather than nesting a per-stage sub-variant.
using StagedCopyFailurePayload =
    std::variant<std::monostate, StagedSavePlatformFailure, StagedCopySizeDisagreement,
                 StagedCopyByteMismatch, StagedCopyContainerSanityFailure,
                 StagedCopyResourceExhausted, StagedCopyUnexpectedFailure>;

class StagedCopyFailure final {
  public:
    StagedCopyFailure() = default;

    template <typename Payload>
    StagedCopyFailure(const StagedCopyStage stage, Payload payload)
        : stage_(stage), payload_(std::move(payload)) {}

    [[nodiscard]] StagedCopyStage stage() const noexcept { return stage_; }
    [[nodiscard]] const StagedCopyFailurePayload& payload() const noexcept { return payload_; }

    template <typename Payload> [[nodiscard]] const Payload* payloadAs() const noexcept {
        return std::get_if<Payload>(&payload_);
    }

  private:
    StagedCopyStage stage_ = StagedCopyStage::None;
    StagedCopyFailurePayload payload_;
};

class [[nodiscard]] StagedCopyResult final {
  public:
    StagedCopyResult(StagedCopyResult&&) noexcept = default;
    StagedCopyResult& operator=(StagedCopyResult&&) noexcept = default;
    StagedCopyResult(const StagedCopyResult&) = delete;
    StagedCopyResult& operator=(const StagedCopyResult&) = delete;
    ~StagedCopyResult() = default;

    [[nodiscard]] static StagedCopyResult success() noexcept;
    // `rejectDiagnostic`, when set, names a StagedArtifactError from a lease.rejectVerification()
    // call that itself failed while reacting to this failure (StagedCopyStage::Verification only).
    // Always secondary to `failure`, mirroring StagedSaveResult::rejectDiagnostic() exactly.
    [[nodiscard]] static StagedCopyResult
    failure(StagedCopyFailure failure,
            std::optional<platform::StagedArtifactError> rejectDiagnostic = std::nullopt);

    [[nodiscard]] explicit operator bool() const noexcept { return !failure_.has_value(); }
    [[nodiscard]] const StagedCopyFailure* failure() const& noexcept {
        return failure_.has_value() ? &*failure_ : nullptr;
    }
    [[nodiscard]] const StagedCopyFailure* failure() const&& = delete;
    [[nodiscard]] std::optional<platform::StagedArtifactError> rejectDiagnostic() const noexcept {
        return rejectDiagnostic_;
    }

  private:
    StagedCopyResult() = default;

    std::optional<StagedCopyFailure> failure_;
    std::optional<platform::StagedArtifactError> rejectDiagnostic_;
};

// Drives the write/read-back/verify chain over an already-staged platform::StagedArtifactLease
// (the caller has already run the coordinator's preflight() and stage()): writes `sourceBytes`
// verbatim, reads the staged bytes back, and verifies the round trip as documented above. On
// success the lease has accepted verification and is exactly one lease.publish() away from durable
// publication; this function NEVER calls publish() itself. `sourceBytes` must remain valid for the
// duration of the call; no reference to it survives the call. Top-level noexcept: an internal
// std::bad_alloc or budget rejection is reported as a typed StagedCopyResourceExhausted failure at
// whichever stage was current; any other unexpected exception is reported as
// StagedCopyUnexpectedFailure.
[[nodiscard]] StagedCopyResult stageCopyArchive(platform::StagedArtifactLease& lease,
                                                std::span<const std::byte> sourceBytes,
                                                const ZipContainerLimits& limits,
                                                ProjectIoOperationMemory operation) noexcept;

} // namespace bloom::project
