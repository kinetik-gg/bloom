#include <bloom/platform/destination_facts.hpp>

#include "staged_artifact_platform.hpp"

#include <charconv>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace bloom::platform {
namespace {

bool writableFolder(const std::filesystem::path& folder) {
#if defined(_WIN32)
    const auto attributes = GetFileAttributesW(folder.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_READONLY) == 0;
#else
    return ::access(folder.c_str(), W_OK | X_OK) == 0;
#endif
}

bool processAlive(const std::int64_t pid) {
    if (pid <= 0)
        return false;
#if defined(_WIN32)
    const auto handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr)
        return false;
    const bool alive = WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
    CloseHandle(handle);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

} // namespace

DestinationFolderFacts destinationFolderFacts(const std::filesystem::path& folder) {
    DestinationFolderFacts facts;
    std::error_code error;
    facts.exists = std::filesystem::is_directory(folder, error) && !error;
    auto existing = folder;
    while (!existing.empty() && !(std::filesystem::is_directory(existing, error) && !error)) {
        if (existing == existing.parent_path())
            break;
        existing = existing.parent_path();
    }
    if (existing.empty())
        return facts;
    facts.writable = writableFolder(existing);
    const auto space = std::filesystem::space(existing, error);
    if (!error && space.available != static_cast<std::uintmax_t>(-1))
        facts.availableBytes = static_cast<std::uint64_t>(space.available);
    return facts;
}

std::uint64_t removeStaleStagedArtifacts(const std::filesystem::path& target) noexcept {
    try {
        const auto tag = detail::stagedArtifactTargetTag(target.filename().string());
        std::error_code error;
        std::uint64_t removed = 0;
        for (const auto& entry : std::filesystem::directory_iterator(target.parent_path(), error)) {
            const auto name = entry.path().filename().string();
            if (!name.starts_with(detail::kStagedArtifactPrefix))
                continue;
            // ".bloom-stage-<pid>-<target tag>-<nonce>".
            const auto rest = std::string_view(name).substr(detail::kStagedArtifactPrefix.size());
            std::int64_t pid = 0;
            const auto [end, parsed] = std::from_chars(rest.data(), rest.data() + rest.size(), pid);
            const auto after =
                std::string_view(end, static_cast<std::size_t>(rest.data() + rest.size() - end));
            if (parsed != std::errc{} || end == rest.data() || !after.starts_with("-") ||
                !after.substr(1).starts_with(tag + "-") || processAlive(pid))
                continue;
            if (entry.is_regular_file(error) && std::filesystem::remove(entry.path(), error))
                ++removed;
        }
        return removed;
    } catch (...) {
        return 0;
    }
}

} // namespace bloom::platform
