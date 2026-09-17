#include <bloom/host/session_recovery.hpp>

#include <algorithm>
#include <string>
#include <system_error>
#include <utility>

namespace bloom::host {

namespace {

// Recovery files are ordinary Bloom project archives, so they carry the project extension: a
// recovered file opens through the same Open the artist already knows, with no second reader and
// no schema of its own. The leading "recovery-" keeps them recognisable inside the directory, and
// the session id keeps two concurrently open projects from writing over each other.
constexpr std::string_view kRecoveryFilePrefix = "recovery-";
constexpr std::string_view kRecoveryFileExtension = ".bloom";

[[nodiscard]] bool isRecoveryFileName(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    return name.starts_with(kRecoveryFilePrefix) && path.extension() == kRecoveryFileExtension;
}

} // namespace

SessionRecoveryResult SessionRecoveryResult::written() noexcept {
    SessionRecoveryResult result;
    result.status_ = SessionRecoveryStatus::Written;
    return result;
}

SessionRecoveryResult SessionRecoveryResult::notDirty() noexcept {
    SessionRecoveryResult result;
    result.status_ = SessionRecoveryStatus::NotDirty;
    return result;
}

SessionRecoveryResult
SessionRecoveryResult::unsaveable(const SessionSaveInputStatus status) noexcept {
    SessionRecoveryResult result;
    result.status_ = SessionRecoveryStatus::Unsaveable;
    result.captureStatus_ = status;
    return result;
}

SessionRecoveryResult SessionRecoveryResult::captureFailed() noexcept {
    SessionRecoveryResult result;
    result.status_ = SessionRecoveryStatus::CaptureFailed;
    return result;
}

SessionRecoveryResult
SessionRecoveryResult::publicationFailed(SavePublicationFailure failure) noexcept {
    SessionRecoveryResult result;
    result.status_ = SessionRecoveryStatus::PublicationFailed;
    result.publicationFailure_.emplace(std::move(failure));
    return result;
}

std::filesystem::path sessionRecoveryFilePath(const std::filesystem::path& directory,
                                              const ProjectSessionId sessionId) {
    return directory / (std::string(kRecoveryFilePrefix) + std::to_string(sessionId.value()) +
                        std::string(kRecoveryFileExtension));
}

SessionRecoveryResult writeSessionRecoverySnapshot(
    const ProjectSession& session, PublicationCoordinator& coordinator,
    platform::StagedArtifactCoordinator& artifacts, const std::filesystem::path& recoveryPath,
    const project::SaveArchiveLimits limits, project::ProjectIoOperationMemory operation) noexcept {
    std::optional<SessionSaveOwningInput> owning;
    try {
        const auto state = session.stateSnapshot();
        if (!state.dirty.has_value() || !*state.dirty) {
            // Nothing unsaved: whatever this session left behind earlier describes work the file
            // on disk already holds, so it is retired rather than refreshed.
            static_cast<void>(removeSessionRecoveryFile(recoveryPath));
            return SessionRecoveryResult::notDirty();
        }
        SessionSaveInputResult captured = session.captureRecoveryInput();
        if (!captured) {
            return SessionRecoveryResult::unsaveable(captured.status());
        }
        owning.emplace(SessionSaveOwningInput{
            .capturedInput = std::move(captured).takeValue(),
            .targetPath = recoveryPath,
            .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace,
            .expectedTarget = std::nullopt,
            .limits = limits,
        });
    } catch (...) {
        // Capture allocates; a recovery write that cannot even start is reported and forgotten.
        return SessionRecoveryResult::captureFailed();
    }

    auto publication = executeSessionSaveMiddle(coordinator, artifacts, *owning, nullptr, nullptr,
                                                std::move(operation));
    if (!publication) {
        return SessionRecoveryResult::publicationFailed(*publication.failure());
    }
    const auto* published = publication.publication();
    if (published == nullptr || !published->targetWasPublished()) {
        // Superseded or refused at the guard: no replacement happened, so no recovery file claims
        // to describe this revision. Deliberately NOT a session failure -- the next tick retries.
        return SessionRecoveryResult::publicationFailed(SavePublicationFailure{});
    }
    // No acceptSavepoint(): the session is still exactly as dirty as it was, still pointed at its
    // own project path, and still owes the artist a real Save.
    return SessionRecoveryResult::written();
}

bool removeSessionRecoveryFile(const std::filesystem::path& recoveryPath) noexcept {
    std::error_code code;
    std::filesystem::remove(recoveryPath, code);
    if (code) {
        return false;
    }
    return !std::filesystem::exists(recoveryPath, code);
}

std::optional<SessionRecoveryOffer>
findSessionRecoveryOffer(const std::filesystem::path& directory,
                         const std::optional<std::filesystem::path>& projectPath) noexcept {
    try {
        std::error_code code;
        if (!std::filesystem::is_directory(directory, code) || code) {
            return std::nullopt;
        }
        std::optional<std::filesystem::file_time_type> projectWrittenAt;
        if (projectPath.has_value() && std::filesystem::exists(*projectPath, code) && !code) {
            const auto written = std::filesystem::last_write_time(*projectPath, code);
            if (!code) {
                projectWrittenAt = written;
            }
        }

        std::optional<SessionRecoveryOffer> newest;
        for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
            if (code) {
                return std::nullopt;
            }
            if (!entry.is_regular_file(code) || code || !isRecoveryFileName(entry.path())) {
                continue;
            }
            const auto size = entry.file_size(code);
            if (code || size == 0) {
                continue;
            }
            const auto writtenAt = entry.last_write_time(code);
            if (code) {
                continue;
            }
            // A recovery file no newer than the saved project describes work the artist already
            // has on disk; offering it would invite them to overwrite the newer file with it.
            if (projectWrittenAt.has_value() && writtenAt <= *projectWrittenAt) {
                continue;
            }
            if (!newest.has_value() || writtenAt > newest->recoveredAt) {
                newest = SessionRecoveryOffer{entry.path(), projectPath, writtenAt};
            }
        }
        return newest;
    } catch (...) {
        // A launch-time scan is best effort by construction: a directory that cannot be read means
        // there is nothing to offer, never a failure to start.
        return std::nullopt;
    }
}

} // namespace bloom::host
