#include <algorithm>
#include <array>
#include <atomic>
#include <bloom/platform/source_identity.hpp>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <string>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace bloom::platform {
namespace {
#if !defined(_WIN32)
std::int64_t nanoseconds(const timespec& value) {
    constexpr std::int64_t billion = 1000000000;
    const auto seconds = static_cast<std::int64_t>(value.tv_sec);
    if (seconds > std::numeric_limits<std::int64_t>::max() / billion ||
        seconds < std::numeric_limits<std::int64_t>::min() / billion)
        return seconds < 0 ? std::numeric_limits<std::int64_t>::min()
                           : std::numeric_limits<std::int64_t>::max();
    return seconds * billion + static_cast<std::int64_t>(value.tv_nsec);
}
#endif
void saturatingAdd(std::atomic<std::uint64_t>& counter, const std::uint64_t amount) noexcept {
    auto current = counter.load(std::memory_order_relaxed);
    std::uint64_t next = 0;
    do {
        next = amount > std::numeric_limits<std::uint64_t>::max() - current
                   ? std::numeric_limits<std::uint64_t>::max()
                   : current + amount;
    } while (!counter.compare_exchange_weak(current, next, std::memory_order_relaxed));
}
struct Counters final {
    std::atomic<std::uint64_t> fullReads{0}, fullReadBytes{0}, identityChecks{0}, identityHits{0};
};
Counters& counters() noexcept {
    static Counters value;
    return value;
}
std::string memoKey(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error)
        absolute = path;
    const auto text = absolute.lexically_normal().generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
struct MemoEntry final {
    FileIdentity identity;
    core::Sha256Digest digest;
    std::uint64_t used = 0;
};
struct MemoState final {
    mutable std::mutex mutex;
    std::map<std::string, MemoEntry, std::less<>> entries;
    mutable std::uint64_t clock = 0;
};
MemoState& memoState() noexcept {
    static MemoState state;
    return state;
}
} // namespace

std::optional<FileIdentity> observeFileIdentity(const std::filesystem::path& path) {
    if (path.empty())
        return {};
#if defined(_WIN32)
    const HANDLE handle = ::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return {};
    struct Closer final {
        HANDLE value;
        ~Closer() { ::CloseHandle(value); }
    } closer{handle};
    FILE_STANDARD_INFO standard{};
    FILE_BASIC_INFO basic{};
    if (!::GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) ||
        standard.Directory || standard.EndOfFile.QuadPart < 0 ||
        !::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)))
        return {};
    FileIdentity identity;
    identity.size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
    identity.modifiedNanoseconds = basic.LastWriteTime.QuadPart;
    identity.changedNanoseconds = basic.ChangeTime.QuadPart;
    FILE_ID_INFO id{};
    if (::GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id))) {
        identity.volume = static_cast<std::uint64_t>(id.VolumeSerialNumber);
        std::array<std::uint8_t, 16> bytes{};
        std::copy(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), bytes.begin());
        for (std::size_t i = 0; i < 8; ++i) {
            identity.fileIdHigh = (identity.fileIdHigh << 8U) | bytes[i];
            identity.fileIdLow = (identity.fileIdLow << 8U) | bytes[i + 8];
        }
    } else {
        BY_HANDLE_FILE_INFORMATION legacy{};
        if (!::GetFileInformationByHandle(handle, &legacy))
            return {};
        identity.volume = legacy.dwVolumeSerialNumber;
        identity.fileIdLow =
            (static_cast<std::uint64_t>(legacy.nFileIndexHigh) << 32U) | legacy.nFileIndexLow;
    }
    return identity;
#else
    struct stat status{};
    if (::stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0)
        return {};
    FileIdentity identity;
    identity.size = static_cast<std::uint64_t>(status.st_size);
#if defined(__APPLE__)
    identity.modifiedNanoseconds = nanoseconds(status.st_mtimespec);
    identity.changedNanoseconds = nanoseconds(status.st_ctimespec);
#else
    identity.modifiedNanoseconds = nanoseconds(status.st_mtim);
    identity.changedNanoseconds = nanoseconds(status.st_ctim);
#endif
    identity.volume = static_cast<std::uint64_t>(status.st_dev);
    identity.fileIdLow = static_cast<std::uint64_t>(status.st_ino);
    return identity;
#endif
}

SourceReadWork sourceReadWork() noexcept {
    auto& value = counters();
    return {value.fullReads.load(std::memory_order_relaxed),
            value.fullReadBytes.load(std::memory_order_relaxed),
            value.identityChecks.load(std::memory_order_relaxed),
            value.identityHits.load(std::memory_order_relaxed)};
}
void recordFullSourceRead(const std::uint64_t bytes) noexcept {
    saturatingAdd(counters().fullReads, 1);
    saturatingAdd(counters().fullReadBytes, bytes);
}

std::optional<core::Sha256Digest> VerifiedSourceDigests::find(const std::filesystem::path& path,
                                                              const FileIdentity& identity) const {
    auto& state = memoState();
    const auto key = memoKey(path);
    const std::lock_guard lock(state.mutex);
    const auto found = state.entries.find(key);
    if (found == state.entries.end() || found->second.identity != identity)
        return {};
    found->second.used = ++state.clock;
    return found->second.digest;
}
void VerifiedSourceDigests::record(const std::filesystem::path& path, const FileIdentity& identity,
                                   const core::Sha256Digest& digest) {
    auto& state = memoState();
    auto key = memoKey(path);
    const std::lock_guard lock(state.mutex);
    state.entries.insert_or_assign(std::move(key), MemoEntry{identity, digest, ++state.clock});
    if (state.entries.size() > kEntryLimit) {
        const auto oldest = std::ranges::min_element(
            state.entries, {}, [](const auto& entry) { return entry.second.used; });
        state.entries.erase(oldest);
    }
}
void VerifiedSourceDigests::forget(const std::filesystem::path& path) {
    auto& state = memoState();
    const auto key = memoKey(path);
    const std::lock_guard lock(state.mutex);
    state.entries.erase(key);
}
void VerifiedSourceDigests::clear() {
    auto& state = memoState();
    const std::lock_guard lock(state.mutex);
    state.entries.clear();
}
std::size_t VerifiedSourceDigests::size() const {
    auto& state = memoState();
    const std::lock_guard lock(state.mutex);
    return state.entries.size();
}
VerifiedSourceDigests& verifiedSourceDigests() noexcept {
    static VerifiedSourceDigests value;
    return value;
}

SourceObservation observeSource(const std::filesystem::path& path) {
    SourceObservation observation;
    observation.identity = observeFileIdentity(path);
    saturatingAdd(counters().identityChecks, 1);
    if (observation.identity) {
        observation.digest = verifiedSourceDigests().find(path, *observation.identity);
        if (observation.digest)
            saturatingAdd(counters().identityHits, 1);
    }
    return observation;
}

SourceHash hashSourceFile(const std::filesystem::path& path, const std::function<bool()>& cancel,
                          const std::uint64_t sizeLimit) {
    SourceHash result;
    if (cancel && cancel()) {
        result.error = SourceHashError::Cancelled;
        return result;
    }
    const auto before = observeFileIdentity(path);
    if (!before) {
        result.error = SourceHashError::Missing;
        return result;
    }
    if (before->size > sizeLimit) {
        result.error = SourceHashError::Oversized;
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.error = SourceHashError::Missing;
        return result;
    }
    core::Sha256Hasher hasher;
    std::array<char, 65536> chunk{};
    std::uint64_t total = 0;
    while (input) {
        if (cancel && cancel()) {
            result.error = SourceHashError::Cancelled;
            return result;
        }
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto count = input.gcount();
        if (count <= 0)
            break;
        total += static_cast<std::uint64_t>(count);
        if (total > sizeLimit) {
            result.error = SourceHashError::Oversized;
            return result;
        }
        if (!hasher.update(
                std::as_bytes(std::span(chunk.data(), static_cast<std::size_t>(count))))) {
            result.error = SourceHashError::Oversized;
            return result;
        }
    }
    recordFullSourceRead(total);
    if (input.bad() || !input.eof()) {
        result.error = SourceHashError::Io;
        return result;
    }
    const auto after = observeFileIdentity(path);
    if (!after || *after != *before || total != before->size) {
        result.error = SourceHashError::Changed;
        return result;
    }
    result.digest = hasher.finalize();
    result.identity = *after;
    verifiedSourceDigests().record(path, result.identity, result.digest);
    return result;
}

SourceHash digestSourceFile(const std::filesystem::path& path, const std::function<bool()>& cancel,
                            const std::uint64_t sizeLimit) {
    if (const auto observed = observeSource(path); observed.digest && observed.identity) {
        if (observed.identity->size > sizeLimit)
            return {.error = SourceHashError::Oversized};
        return {.error = SourceHashError::None,
                .digest = *observed.digest,
                .identity = *observed.identity};
    }
    return hashSourceFile(path, cancel, sizeLimit);
}

SourceCheck verifySourceDigest(const std::filesystem::path& path,
                               const core::Sha256Digest& expected,
                               const std::function<bool()>& cancel, const std::uint64_t sizeLimit) {
    const auto hashed = digestSourceFile(path, cancel, sizeLimit);
    switch (hashed.error) {
    case SourceHashError::None:
        return hashed.digest == expected ? SourceCheck::Matches : SourceCheck::Changed;
    case SourceHashError::Cancelled:
        return SourceCheck::Cancelled;
    case SourceHashError::Missing:
    case SourceHashError::Io:
        return SourceCheck::Missing;
    case SourceHashError::Changed:
    case SourceHashError::Oversized:
        break;
    }
    return SourceCheck::Changed;
}
} // namespace bloom::platform
