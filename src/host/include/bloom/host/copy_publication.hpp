#pragma once

#include <bloom/host/publication_coordinator.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/staged_copy.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <variant>

// The application-host composition of docs/architecture/project-format.md's "Staging And Atomic
// Publication" nine-step sequence with docs/architecture/project-session.md's "Per-Target Ordering
// And Publication" for Save Copy -- the byte-copy sibling of save_publication.hpp's identical
// composition for Save. This module owns steps 1-2 (admission and preflight) and 6-8 (publication
// ordering against the application-wide PublicationCoordinator, then the platform publication
// lease), composing project::stageCopyArchive() (steps 3-5) and
// platform::StagedArtifactLease::publish() (the rest of 6-8) in between -- the SAME
// PublicationCoordinator and PublicationIntentId sequence Save and frame export share (project-
// session.md: "This one sequence covers project saves, byte-preserving Save Copy, and frame
// exports"). It is the synchronous, caller-threaded executor primitive only; session/executor
// threading is a later slice (see bloom/host/copy_async_io.hpp).
//
// Save Copy has no manifest/document to build -- the caller already holds the exact bytes to
// publish (a preserved-read-only project's retained original archive) -- so, unlike
// SavePublicationRequest, this request carries `sourceBytes` directly instead of manifest/document
// pointers, and composes project::stageCopyArchive() instead of project::stageSaveArchive() at the
// StagedCopy step. Every other stage of the pipeline is identical in shape to save_publication.hpp;
// see the implementor's report for why this module is parallel construction rather than a shared
// refactor with executeSavePublication().
namespace bloom::host {

enum class CopyPublicationStage : std::uint8_t {
    None,
    Admission,
    Preflight,
    Registration,
    Staging,
    StagedCopy,
    Guard,
    Cancelled,
};

// Mirrors SavePublicationResourceExhausted/SavePublicationUnexpectedFailure exactly (see
// save_publication.hpp's identical comment on why a resource-exhausted or unexpected-exception
// failure is reported at whichever stage was current rather than getting its own per-stage
// enumerator).
struct CopyPublicationResourceExhausted final {};
struct CopyPublicationUnexpectedFailure final {};

// Mirrors SavePublicationFailurePayload exactly, substituting project::StagedCopyFailure for
// project::StagedSaveFailure at the StagedCopy step. See save_publication.hpp's identical comment
// on why the PublicationGuardStatus enumerators other than Entered/Superseded are still handled
// (typed, no publish() call) despite being unreachable from this single-executor composition.
using CopyPublicationFailurePayload =
    std::variant<std::monostate, PublicationAdmissionStatus, platform::StagedArtifactError,
                 PublicationRegistrationStatus, project::StagedCopyFailure, PublicationGuardStatus,
                 CopyPublicationResourceExhausted, CopyPublicationUnexpectedFailure>;

class CopyPublicationFailure final {
  public:
    CopyPublicationFailure() = default;

    template <typename Payload>
    CopyPublicationFailure(const CopyPublicationStage stage, Payload payload)
        : stage_(stage), payload_(std::move(payload)) {}

    [[nodiscard]] CopyPublicationStage stage() const noexcept { return stage_; }
    [[nodiscard]] const CopyPublicationFailurePayload& payload() const noexcept { return payload_; }

    template <typename Payload> [[nodiscard]] const Payload* payloadAs() const noexcept {
        return std::get_if<Payload>(&payload_);
    }

  private:
    CopyPublicationStage stage_ = CopyPublicationStage::None;
    CopyPublicationFailurePayload payload_;
};

// Mirrors SavePublicationResult exactly (see its own declaration comment for the full contract):
// exactly one of `failure()`/`publication()` is non-null, `publication()` set only for the two
// guard outcomes that actually call platform::StagedArtifactLease::publish().
class [[nodiscard]] CopyPublicationResult final {
  public:
    CopyPublicationResult(CopyPublicationResult&&) noexcept = default;
    CopyPublicationResult& operator=(CopyPublicationResult&&) noexcept = default;
    CopyPublicationResult(const CopyPublicationResult&) = delete;
    CopyPublicationResult& operator=(const CopyPublicationResult&) = delete;
    ~CopyPublicationResult() = default;

    [[nodiscard]] static CopyPublicationResult
    published(platform::StagedArtifactPublicationResult publication,
              PublicationIntentId intentId) noexcept;

    [[nodiscard]] static CopyPublicationResult
    failure(CopyPublicationFailure failure,
            std::optional<platform::StagedArtifactError> rejectDiagnostic = std::nullopt) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept { return !failure_.has_value(); }

    [[nodiscard]] const platform::StagedArtifactPublicationResult* publication() const& noexcept {
        return publication_.has_value() ? &*publication_ : nullptr;
    }
    [[nodiscard]] const platform::StagedArtifactPublicationResult* publication() const&& = delete;
    [[nodiscard]] std::optional<PublicationIntentId> intentId() const noexcept { return intentId_; }
    [[nodiscard]] const CopyPublicationFailure* failure() const& noexcept {
        return failure_.has_value() ? &*failure_ : nullptr;
    }
    [[nodiscard]] const CopyPublicationFailure* failure() const&& = delete;
    [[nodiscard]] std::optional<platform::StagedArtifactError> rejectDiagnostic() const noexcept {
        return rejectDiagnostic_;
    }

  private:
    CopyPublicationResult() = default;

    std::optional<platform::StagedArtifactPublicationResult> publication_;
    std::optional<PublicationIntentId> intentId_;
    std::optional<CopyPublicationFailure> failure_;
    std::optional<platform::StagedArtifactError> rejectDiagnostic_;
};

struct CopyPublicationRequest final {
    std::filesystem::path targetPath;
    platform::ArtifactOverwritePolicy overwritePolicy =
        platform::ArtifactOverwritePolicy::CreateOrReplace;
    std::optional<platform::ArtifactTargetObservation> expectedTarget;
    // Non-owning, caller-kept-alive -- mirrors SavePublicationRequest::manifest/document. The
    // retained original archive bytes to publish verbatim.
    std::span<const std::byte> sourceBytes;
    // Only `.container` is consulted (project::stageCopyArchive() takes a bare
    // project::ZipContainerLimits); the full project::SaveArchiveLimits shape is reused here so
    // callers that already carry one typed limits value (e.g. bloom::ui::ProjectHost, which uses
    // the same value for Open) do not need a second, copy-only limits type.
    project::SaveArchiveLimits limits;
    // Same seam as SavePublicationRequest::preAdmitted, including its own ownership contract.
    PublicationAdmission* preAdmitted = nullptr;
    // Same seam as SavePublicationRequest::cancellationFlag.
    const std::atomic_bool* cancellationFlag = nullptr;
};

// See the namespace-level comment above for the composition this performs. noexcept top level,
// with the identical RAII/exception-safety contract as executeSavePublication() (see its own
// declaration comment).
[[nodiscard]] CopyPublicationResult executeCopyPublication(
    PublicationCoordinator& coordinator, platform::StagedArtifactCoordinator& artifacts,
    const CopyPublicationRequest& request, project::ProjectIoOperationMemory operation) noexcept;

} // namespace bloom::host
