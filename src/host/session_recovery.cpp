#include <bloom/host/session_recovery.hpp>

#include <bloom/core/sha256.hpp>
#include <bloom/project/open_archive.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace bloom::host {

namespace {

// Recovery files are ordinary Bloom project archives, so they carry the project extension: a
// recovered file opens through the same Open the artist already knows, with no second reader and
// no schema of its own. The leading "recovery-" keeps them recognisable inside the directory, and
// an owner UUID plus session id keeps separate process lifetimes and concurrently open projects
// from writing over each other. The broad scan also recognizes earlier recovery-<id>.bloom files.
constexpr std::string_view kRecoveryFilePrefix = "recovery-";
constexpr std::string_view kRecoveryFileExtension = ".bloom";
constexpr std::uintmax_t kMaxOwnershipRecordBytes = 256 * 1024;

[[nodiscard]] bool isRecoveryFileName(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    return name.starts_with(kRecoveryFilePrefix) && path.extension() == kRecoveryFileExtension;
}

[[nodiscard]] std::filesystem::path ownershipPath(std::filesystem::path recoveryPath) {
    recoveryPath += ".owner";
    return recoveryPath;
}

// The archive buffer is charged to the same operation that later decodes it. The caller retains
// the reservation until decode completes; this runs only on a BlockingIo worker.
struct RecoveryArchiveBytes final {
    project::ProjectIoMemoryReservation reservation;
    std::vector<std::byte> bytes;
};

[[nodiscard]] std::optional<RecoveryArchiveBytes>
readRecoveryArchive(const std::filesystem::path& path,
                    const project::ProjectIoOperationMemory& operation) {
    std::error_code code;
    const auto size = std::filesystem::file_size(path, code);
    if (code || size == 0 || size > project::SaveArchiveLimits{}.container.maxArchiveBytes ||
        size > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    auto reserved = operation.reserve(size);
    if (!reserved) {
        return std::nullopt;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()))) {
        return std::nullopt;
    }
    return RecoveryArchiveBytes{std::move(reserved).takeReservation(), std::move(bytes)};
}

// Metadata publication needs only a digest, never a second whole-archive allocation.
[[nodiscard]] std::optional<core::Sha256Digest>
hashRecoveryArchive(const std::filesystem::path& path) {
    std::error_code code;
    const auto size = std::filesystem::file_size(path, code);
    if (code || size == 0 || size > project::SaveArchiveLimits{}.container.maxArchiveBytes) {
        return std::nullopt;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    core::Sha256Hasher hasher;
    std::array<std::byte, 64 * 1024> buffer{};
    std::uint64_t remaining = size;
    while (remaining > 0) {
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        if (!stream.read(reinterpret_cast<char*>(buffer.data()),
                         static_cast<std::streamsize>(count)) ||
            !hasher.update(std::span<const std::byte>(buffer.data(), count))) {
            return std::nullopt;
        }
        remaining -= count;
    }
    return hasher.finalize();
}

[[nodiscard]] bool writeOwnership(const std::filesystem::path& recoveryPath,
                                  const std::optional<ProjectDisplayPath>& projectPath) noexcept {
    try {
        const auto digest = hashRecoveryArchive(recoveryPath);
        if (!digest.has_value()) {
            return false;
        }
        std::error_code code;
        const auto writtenAt = std::filesystem::last_write_time(recoveryPath, code);
        if (code) {
            return false;
        }
        std::string ownedPath;
        if (projectPath.has_value()) {
            const auto absolutePath = std::filesystem::absolute(projectPath->value(), code);
            if (code) {
                return false;
            }
            const auto utf8 = absolutePath.lexically_normal().u8string();
            ownedPath.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        }
        const auto hex = digest->toLowercaseHex();
        const auto metadataPath = ownershipPath(recoveryPath);
        auto temporaryPath = metadataPath;
        temporaryPath += ".tmp";
        {
            std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
            output << "BLOOM-RECOVERY-OWNER-1\n";
            output.write(hex.data(), hex.size());
            output << '\n' << writtenAt.time_since_epoch().count() << '\n';
            output.write(ownedPath.data(), static_cast<std::streamsize>(ownedPath.size()));
            output.flush();
            output.close();
            if (!output) {
                std::filesystem::remove(temporaryPath, code);
                return false;
            }
        }
        // POSIX replaces in place. Windows requires removing an existing destination first; either
        // interruption leaves an absent/old record, which scan checks against digest and write
        // time.
        std::filesystem::rename(temporaryPath, metadataPath, code);
        if (code) {
            code.clear();
            std::filesystem::remove(metadataPath, code);
            code.clear();
            std::filesystem::rename(temporaryPath, metadataPath, code);
        }
        if (code) {
            code.clear();
            std::filesystem::remove(temporaryPath, code);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

struct RecoveryOwnership final {
    std::optional<std::filesystem::path> projectPath;
};

[[nodiscard]] std::optional<RecoveryOwnership>
readOwnership(const std::filesystem::path& recoveryPath, const core::Sha256Digest& digest,
              const std::filesystem::file_time_type writtenAt) {
    std::error_code code;
    const auto metadataPath = ownershipPath(recoveryPath);
    const auto size = std::filesystem::file_size(metadataPath, code);
    if (code || size > kMaxOwnershipRecordBytes) {
        return std::nullopt;
    }
    std::string record(static_cast<std::size_t>(size), '\0');
    std::ifstream stream(metadataPath, std::ios::binary);
    if (!stream.read(record.data(), static_cast<std::streamsize>(record.size()))) {
        return std::nullopt;
    }
    constexpr std::string_view header = "BLOOM-RECOVERY-OWNER-1\n";
    if (!record.starts_with(header)) {
        return std::nullopt;
    }
    const auto digestEnd = record.find('\n', header.size());
    const auto stampEnd =
        digestEnd == std::string::npos ? digestEnd : record.find('\n', digestEnd + 1);
    if (digestEnd == std::string::npos || stampEnd == std::string::npos ||
        record.substr(header.size(), digestEnd - header.size()) !=
            std::string(digest.toLowercaseHex().data(), core::kSha256HexCharacters)) {
        return std::nullopt;
    }
    decltype(writtenAt.time_since_epoch().count()) stamp{};
    const auto stampText = std::string_view(record).substr(digestEnd + 1, stampEnd - digestEnd - 1);
    const auto parsed =
        std::from_chars(stampText.data(), stampText.data() + stampText.size(), stamp);
    if (parsed.ec != std::errc{} || parsed.ptr != stampText.data() + stampText.size() ||
        stamp != writtenAt.time_since_epoch().count()) {
        return std::nullopt;
    }
    const auto pathBytes = std::string_view(record).substr(stampEnd + 1);
    if (pathBytes.empty()) {
        return RecoveryOwnership{};
    }
    if (pathBytes.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    return RecoveryOwnership{
        std::filesystem::path{std::u8string(pathBytes.begin(), pathBytes.end())}};
}

[[nodiscard]] bool isOpenableArchive(const std::vector<std::byte>& bytes,
                                     project::ProjectIoOperationMemory operation) {
    const auto opened = project::openProjectArchive(bytes, {}, std::move(operation));
    return opened.outcome() != project::OpenArchiveOutcome::Failed;
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
                                              const std::string_view ownerToken,
                                              const ProjectSessionId sessionId) {
    const auto hexDigit = [](const char digit) {
        return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
    };
    if (ownerToken.size() != 32 || !std::all_of(ownerToken.begin(), ownerToken.end(), hexDigit) ||
        !sessionId.isValid()) {
        throw std::invalid_argument("invalid recovery owner or session identity");
    }
    return directory / (std::string(kRecoveryFilePrefix) + std::string(ownerToken) + "-" +
                        std::to_string(sessionId.value()) + std::string(kRecoveryFileExtension));
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
    if (!writeOwnership(recoveryPath, owning->capturedInput.displayPath())) {
        return SessionRecoveryResult::publicationFailed(SavePublicationFailure{});
    }
    return SessionRecoveryResult::written();
}

SessionRecoveryResult executeSessionRecoverySnapshot(
    PublicationCoordinator& coordinator, platform::StagedArtifactCoordinator& artifacts,
    const SessionSaveOwningInput& input, const std::atomic_bool* cancellationFlag,
    project::ProjectIoOperationMemory operation) noexcept {
    std::error_code code;
    std::filesystem::create_directories(input.targetPath.parent_path(), code);
    if (code) {
        return SessionRecoveryResult::publicationFailed(SavePublicationFailure{});
    }
    auto publication = executeSessionSaveMiddle(coordinator, artifacts, input, nullptr,
                                                cancellationFlag, std::move(operation));
    if (!publication) {
        return SessionRecoveryResult::publicationFailed(*publication.failure());
    }
    const auto* published = publication.publication();
    if (published == nullptr || !published->targetWasPublished()) {
        return SessionRecoveryResult::publicationFailed(SavePublicationFailure{});
    }
    if (!writeOwnership(input.targetPath, input.capturedInput.displayPath())) {
        return SessionRecoveryResult::publicationFailed(SavePublicationFailure{});
    }
    return SessionRecoveryResult::written();
}

bool removeSessionRecoveryFile(const std::filesystem::path& recoveryPath) noexcept {
    try {
        std::error_code code;
        std::filesystem::remove(recoveryPath, code);
        if (code) {
            return false;
        }
        if (std::filesystem::exists(recoveryPath, code) || code) {
            return false;
        }
        std::filesystem::remove(ownershipPath(recoveryPath), code);
        if (code) {
            return false;
        }
        auto temporaryPath = ownershipPath(recoveryPath);
        temporaryPath += ".tmp";
        std::filesystem::remove(temporaryPath, code);
        return !code;
    } catch (...) {
        return false;
    }
}

std::optional<SessionRecoveryOffer>
findSessionRecoveryOffer(const std::filesystem::path& directory,
                         const std::optional<std::filesystem::path>& /* projectPath */) noexcept {
    try {
        std::error_code code;
        if (!std::filesystem::is_directory(directory, code) || code) {
            return std::nullopt;
        }
        std::vector<SessionRecoveryOffer> candidates;
        for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
            if (code) {
                return std::nullopt;
            }
            if (!entry.is_regular_file(code) || code || !isRecoveryFileName(entry.path())) {
                continue;
            }
            const auto size = entry.file_size(code);
            if (code || size == 0 ||
                size > project::SaveArchiveLimits{}.container.maxArchiveBytes) {
                continue;
            }
            const auto writtenAt = entry.last_write_time(code);
            if (code) {
                continue;
            }
            candidates.push_back(SessionRecoveryOffer{entry.path(), std::nullopt, writtenAt});
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
            if (left.recoveredAt != right.recoveredAt) {
                return left.recoveredAt > right.recoveredAt;
            }
            return left.recoveryPath.generic_string() < right.recoveryPath.generic_string();
        });
        for (auto& candidate : candidates) {
            auto memory = project::ProjectIoMemoryCoordinator::create();
            if (!memory.has_value()) {
                continue;
            }
            auto operation =
                memory->createOperation(project::kDefaultProjectIoOperationResidentBytes,
                                        project::SaveArchiveLimits{}.container.maxArchiveBytes);
            if (!operation.has_value()) {
                continue;
            }
            const auto bytes = readRecoveryArchive(candidate.recoveryPath, *operation);
            if (!bytes.has_value()) {
                continue;
            }
            code.clear();
            const auto currentWrittenAt =
                std::filesystem::last_write_time(candidate.recoveryPath, code);
            const auto currentSize = std::filesystem::file_size(candidate.recoveryPath, code);
            if (code || currentWrittenAt != candidate.recoveredAt ||
                currentSize != bytes->bytes.size()) {
                continue;
            }
            const auto digest = core::Sha256Hasher::hash(bytes->bytes);
            if (!digest.has_value()) {
                continue;
            }
            const auto owner =
                readOwnership(candidate.recoveryPath, *digest, candidate.recoveredAt);
            if (owner.has_value()) {
                candidate.projectPath = owner->projectPath;
            }
            // Unknown (legacy/missing/corrupt metadata) ownership never borrows the caller's
            // project path. The artist can still open the preserved recovery archive explicitly.
            if (candidate.projectPath.has_value()) {
                code.clear();
                const auto projectWrittenAt =
                    std::filesystem::last_write_time(*candidate.projectPath, code);
                if (!code && candidate.recoveredAt <= projectWrittenAt) {
                    continue;
                }
            }
            if (isOpenableArchive(bytes->bytes, std::move(*operation))) {
                return candidate;
            }
        }
        return std::nullopt;
    } catch (...) {
        // A launch-time scan is best effort by construction: a directory that cannot be read means
        // there is nothing to offer, never a failure to start.
        return std::nullopt;
    }
}

} // namespace bloom::host
