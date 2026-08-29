#pragma once

#include <bloom/host/project_session.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <variant>

// The synchronous session-level Open orchestration: composes ProjectSession::admitOpenIntent()
// (docs/architecture/project-session.md, "Open Intent") with project::openProjectArchive()
// (open_archive.hpp -- the shared reopen/decode/reconstruct/validate chain) and, on a successful
// or preserved-read-only classification, ProjectSession::installDecodedReplacement()/
// installPreservedReadOnlyReplacement() (project_session.hpp, "Session Publication"). This module
// owns none of those three seams' own semantics; it only wires them together on the caller's
// thread. File reading stays OUT of this module (bytes-in span, matching every prior slice -- the
// async/file package owns streaming and fingerprints later); an asynchronous/Jobs-driven Open is a
// later slice (see AGENTS.md's boundary and this task's non-goals).
namespace bloom::host {

// Where an openSessionArchive() call stopped. Admission: session.admitOpenIntent() itself refused
// (see SessionOpenResult::admissionStatus()). Opening: project::openProjectArchive() reported a
// typed SaveArchiveFailure (see SessionOpenResult::openingFailure()) -- the session is untouched,
// since no install*() call is ever made on this path. Installation: openProjectArchive() reached
// either Opened or PreservedReadOnlyRequired, and this module attempted (or, for a pathless
// PreservedReadOnlyRequired result, deliberately declined to attempt) an install*() call (see
// SessionOpenResult::installOutcome()).
enum class SessionOpenStage : std::uint8_t {
    Admission,
    Opening,
    Installation,
};

// Wraps a thrown exception from ProjectSession::installDecodedReplacement()/
// installPreservedReadOnlyReplacement() -- neither is noexcept (each may allocate; see their own
// declaration comments on ProjectSession) -- so this module's noexcept entry point never lets an
// exception escape. Mirrors session_save.hpp's SessionSaveResourceExhausted/
// SessionSaveUnexpectedFailure precedent exactly. Per installDecodedReplacement()'s/
// installPreservedReadOnlyReplacement()'s own strong exception guarantee, a caught exception here
// means the session was left completely untouched.
struct SessionOpenInstallResourceExhausted final {};
struct SessionOpenInstallUnexpectedFailure final {};

using SessionOpenInstallOutcome =
    std::variant<SessionInstallStatus, SessionOpenInstallResourceExhausted,
                 SessionOpenInstallUnexpectedFailure>;

// See SessionOpenStage above for the three stages this result distinguishes. Exactly one of
// admissionStatus()/openingFailure()/installOutcome() is populated, matching stage().
class [[nodiscard]] SessionOpenResult final {
  public:
    SessionOpenResult(SessionOpenResult&&) noexcept = default;
    SessionOpenResult& operator=(SessionOpenResult&&) noexcept = default;
    SessionOpenResult(const SessionOpenResult&) = delete;
    SessionOpenResult& operator=(const SessionOpenResult&) = delete;
    ~SessionOpenResult() = default;

    [[nodiscard]] static SessionOpenResult
    admissionFailure(OpenIntentAdmissionStatus status) noexcept;
    [[nodiscard]] static SessionOpenResult
    openingFailure(project::SaveArchiveFailure failure) noexcept;
    // `attemptedContentKind` names which content kind this pipeline attempted to install
    // (DecodedDocument for an Opened archive, PreservedReadOnly for a PreservedReadOnlyRequired
    // one) regardless of whether `outcome` is a real success. `preservedReadOnly` is present only
    // for the PreservedReadOnlyRequired path (whether or not installation itself succeeded), per
    // the frozen design: "the result carries the preservation side/reason diagnostics verbatim for
    // the caller (who retains the archive bytes for Save Copy)".
    [[nodiscard]] static SessionOpenResult
    installation(SessionOpenInstallOutcome outcome, ProjectSessionContentKind attemptedContentKind,
                 std::optional<project::OpenArchivePreservedReadOnly> preservedReadOnly) noexcept;

    // True only for the single fully-succeeded path: Installation stage with a real
    // SessionInstallStatus::Installed outcome. Every other case -- including a wrapped install
    // exception -- is false; inspect stage() and installOutcome() for the exact typed reason.
    [[nodiscard]] explicit operator bool() const noexcept {
        if (stage_ != SessionOpenStage::Installation) {
            return false;
        }
        const auto* status = std::get_if<SessionInstallStatus>(&installOutcome_);
        return status != nullptr && *status == SessionInstallStatus::Installed;
    }

    [[nodiscard]] SessionOpenStage stage() const noexcept { return stage_; }

    // Valid only when stage() == Admission.
    [[nodiscard]] std::optional<OpenIntentAdmissionStatus> admissionStatus() const noexcept {
        return admissionStatus_;
    }

    // Valid only when stage() == Opening (project::openProjectArchive()'s own Failed outcome,
    // verbatim; no install*() call was ever made).
    [[nodiscard]] const project::SaveArchiveFailure* openingFailure() const& noexcept {
        return openingFailure_.has_value() ? &*openingFailure_ : nullptr;
    }
    [[nodiscard]] const project::SaveArchiveFailure* openingFailure() const&& = delete;

    // Valid only when stage() == Installation: either the exact SessionInstallStatus an
    // install*() call returned, or a wrapped exception from that (non-noexcept) call. For the
    // pathless-PreservedReadOnlyRequired refusal (no ProjectDisplayPath supplied to
    // openSessionArchive()), this holds SessionInstallStatus::InvalidContent synthesized directly
    // -- installPreservedReadOnlyReplacement() is never called, since it requires a real
    // ProjectDisplayPath by contract (matching createPreservedReadOnly()'s own requirement).
    [[nodiscard]] const SessionOpenInstallOutcome* installOutcome() const& noexcept {
        return stage_ == SessionOpenStage::Installation ? &installOutcome_ : nullptr;
    }
    [[nodiscard]] const SessionOpenInstallOutcome* installOutcome() const&& = delete;

    // Valid only when stage() == Installation: which content kind this pipeline attempted to
    // install, regardless of whether installation itself succeeded.
    [[nodiscard]] std::optional<ProjectSessionContentKind> attemptedContentKind() const noexcept {
        return attemptedContentKind_;
    }

    // Present whenever project::openProjectArchive() classified the archive
    // PreservedReadOnlyRequired, whether or not installation succeeded -- see installation()'s
    // comment above.
    [[nodiscard]] const project::OpenArchivePreservedReadOnly* preservedReadOnly() const& noexcept {
        return preservedReadOnly_.has_value() ? &*preservedReadOnly_ : nullptr;
    }
    [[nodiscard]] const project::OpenArchivePreservedReadOnly* preservedReadOnly() const&& = delete;

  private:
    SessionOpenResult() = default;

    SessionOpenStage stage_ = SessionOpenStage::Admission;
    std::optional<OpenIntentAdmissionStatus> admissionStatus_;
    std::optional<project::SaveArchiveFailure> openingFailure_;
    SessionOpenInstallOutcome installOutcome_ = SessionInstallStatus::InvalidSession;
    std::optional<ProjectSessionContentKind> attemptedContentKind_;
    std::optional<project::OpenArchivePreservedReadOnly> preservedReadOnly_;
};

// Pipeline: session.admitOpenIntent() -> project::openProjectArchive(archive, limits, operation) ->
//   - Opened: build a DecodedReplacementContent from the OpenedArchive (editability is always
//     Editable in this slice -- provider availability/degraded-editable detection needs a
//     capability registry that does not exist yet) -> session.installDecodedReplacement().
//   - PreservedReadOnlyRequired: requires `displayPath` (a pathless preserved-read-only install is
//     refused as typed InvalidContent -- createPreservedReadOnly() requires a path too) ->
//     session.installPreservedReadOnlyReplacement().
//   - Failed: a typed wrap of the SaveArchiveFailure; session untouched.
//
// noexcept top level: neither install*() call is itself noexcept (each may allocate), so both are
// wrapped exactly as session_save.hpp's saveProjectSession() wraps captureSaveInput()/
// acceptSavepoint() -- see SessionOpenInstallResourceExhausted/SessionOpenInstallUnexpectedFailure
// above. project::openProjectArchive() and session.admitOpenIntent() are already noexcept.
[[nodiscard]] SessionOpenResult
openSessionArchive(ProjectSession& session, std::span<const std::byte> archive,
                   std::optional<ProjectDisplayPath> displayPath,
                   const project::SaveArchiveLimits& limits,
                   project::ProjectIoOperationMemory operation) noexcept;

} // namespace bloom::host
