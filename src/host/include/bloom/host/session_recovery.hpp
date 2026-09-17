#pragma once

#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/save_publication.hpp>
#include <bloom/host/session_save.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>

// SAVEFIX-1's recovery half. A save can now FAIL instead of aborting the process, but a crash from
// anywhere else still loses everything a session holds, because nothing on disk describes the work
// in progress. This module writes that description: while a session is dirty, its canonical
// document is published to a recovery file beside the application's own data, through exactly the
// staged/atomic path a real Save uses, and the file is removed once a real Save has made it
// redundant.
//
// It deliberately adds no schema and no second writer. A recovery file IS a Bloom project archive
// (the same manifest/document pair at the same schema version), produced by the same
// executeSessionSaveMiddle() a Save runs, so recovering is opening. What it is NOT is a save: no
// savepoint is accepted, the session's dirty state and path intent are untouched, and every
// failure is typed and non-fatal -- a recovery write that cannot happen must never cost the artist
// anything, least of all the session it was meant to protect.
namespace bloom::host {

// Default cadence for the recovery write while a session is dirty. Exposed as a setting on
// SessionRecoveryPolicy rather than hard-coded at the call site so a host can slow it down for a
// very large project without touching this module.
inline constexpr std::chrono::seconds kDefaultSessionRecoveryInterval{60};

struct SessionRecoveryPolicy final {
    std::chrono::seconds interval = kDefaultSessionRecoveryInterval;

    [[nodiscard]] constexpr bool isValid() const noexcept { return interval.count() > 0; }
};

// Where a writeSessionRecoverySnapshot() call stopped.
//   Written           -- the recovery file now describes the session's current revision.
//   NotDirty          -- the session has nothing unsaved; any stale recovery file was removed.
//   Unsaveable        -- captureSaveInput() refused (for example a preserved-read-only session).
//   CaptureFailed     -- capture threw; reported, never propagated.
//   PublicationFailed -- the staged/atomic publication refused or failed before replacing the file.
enum class SessionRecoveryStatus : std::uint8_t {
    Written,
    NotDirty,
    Unsaveable,
    CaptureFailed,
    PublicationFailed,
};

class [[nodiscard]] SessionRecoveryResult final {
  public:
    SessionRecoveryResult(SessionRecoveryResult&&) noexcept = default;
    SessionRecoveryResult& operator=(SessionRecoveryResult&&) noexcept = default;
    SessionRecoveryResult(const SessionRecoveryResult&) = delete;
    SessionRecoveryResult& operator=(const SessionRecoveryResult&) = delete;
    ~SessionRecoveryResult() = default;

    [[nodiscard]] static SessionRecoveryResult written() noexcept;
    [[nodiscard]] static SessionRecoveryResult notDirty() noexcept;
    [[nodiscard]] static SessionRecoveryResult unsaveable(SessionSaveInputStatus status) noexcept;
    [[nodiscard]] static SessionRecoveryResult captureFailed() noexcept;
    [[nodiscard]] static SessionRecoveryResult
    publicationFailed(SavePublicationFailure failure) noexcept;

    [[nodiscard]] SessionRecoveryStatus status() const noexcept { return status_; }
    [[nodiscard]] bool wrote() const noexcept { return status_ == SessionRecoveryStatus::Written; }
    [[nodiscard]] std::optional<SessionSaveInputStatus> captureStatus() const noexcept {
        return captureStatus_;
    }
    [[nodiscard]] const SavePublicationFailure* publicationFailure() const& noexcept {
        return publicationFailure_.has_value() ? &*publicationFailure_ : nullptr;
    }
    [[nodiscard]] const SavePublicationFailure* publicationFailure() const&& = delete;

  private:
    SessionRecoveryResult() = default;

    std::optional<SavePublicationFailure> publicationFailure_;
    std::optional<SessionSaveInputStatus> captureStatus_;
    SessionRecoveryStatus status_ = SessionRecoveryStatus::NotDirty;
};

// The recovery file this session would write inside `directory`. One file per session identity, so
// two windows editing two projects never fight over the same path. The caller creates `directory`;
// this is pure path arithmetic.
[[nodiscard]] std::filesystem::path sessionRecoveryFilePath(const std::filesystem::path& directory,
                                                            ProjectSessionId sessionId);

// Publishes the session's current document to `recoveryPath` when the session is dirty, through
// executeSessionSaveMiddle() -- the same manifest/document assembly, the same staged write, the
// same atomic replacement a Save performs -- WITHOUT accepting a savepoint, so the session's dirty
// state, path intent and revision are all exactly as they were. A clean session writes nothing and
// removes any file left from before it was saved. noexcept, and every failure is typed: a recovery
// write is never allowed to be the reason a session is lost.
[[nodiscard]] SessionRecoveryResult writeSessionRecoverySnapshot(
    const ProjectSession& session, PublicationCoordinator& coordinator,
    platform::StagedArtifactCoordinator& artifacts, const std::filesystem::path& recoveryPath,
    project::SaveArchiveLimits limits, project::ProjectIoOperationMemory operation) noexcept;

// Removes a recovery file that is no longer needed (a successful Save, or closing a session that
// has nothing unsaved). True when nothing is left at `recoveryPath` afterwards, including when
// there was nothing there to begin with.
[[nodiscard]] bool removeSessionRecoveryFile(const std::filesystem::path& recoveryPath) noexcept;

// What a launch found worth offering back to the artist.
struct SessionRecoveryOffer final {
    std::filesystem::path recoveryPath;
    // The project the recovery file was written for, when the session had one. Absent for a
    // never-saved project, which is exactly the case where recovery matters most.
    std::optional<std::filesystem::path> projectPath;
    std::filesystem::file_time_type recoveredAt{};
};

// Scans `directory` for recovery files and returns the newest one that is worth offering: one that
// exists, is non-empty, and -- when its project still exists on disk -- is NEWER than that project,
// because a recovery file older than the saved project describes work the artist already has. No
// offer means there is nothing to recover, which is the ordinary case for a clean shutdown.
[[nodiscard]] std::optional<SessionRecoveryOffer>
findSessionRecoveryOffer(const std::filesystem::path& directory,
                         const std::optional<std::filesystem::path>& projectPath) noexcept;

} // namespace bloom::host
