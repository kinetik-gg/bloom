#pragma once
#include <bloom/core/sha256.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>

namespace bloom::platform {
// Cheap observation of one regular file: length, modification and status-change times, and the
// volume/file id. It never proves content by itself. A full SHA-256 pass records the observation
// that bracketed it; while a later observation is identical, the recorded digest stands for the
// bytes. Any difference (touch, rewrite, rename-over, another volume) forces a fresh hash.
struct FileIdentity final {
    std::uint64_t size = 0;
    std::int64_t modifiedNanoseconds = 0;
    std::int64_t changedNanoseconds = 0;
    std::uint64_t volume = 0;
    std::uint64_t fileIdHigh = 0, fileIdLow = 0;
    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};
// Identity of a regular file through its pathname (following links), or nullopt when the path is
// missing, unreadable or not a regular file. Linux/macOS use stat(2); Windows uses the file
// handle's id, basic and standard information.
[[nodiscard]] std::optional<FileIdentity> observeFileIdentity(const std::filesystem::path& path);

// Process-wide work counters for source verification. Every whole-file read of a media source for
// hashing or snapshotting is counted, so callers and tests can prove that playback, edits and
// per-frame export perform no full-file reads once a source is verified. Counters saturate.
struct SourceReadWork final {
    std::uint64_t fullReads = 0, fullReadBytes = 0;
    std::uint64_t identityChecks = 0, identityHits = 0;
};
[[nodiscard]] SourceReadWork sourceReadWork() noexcept;
void recordFullSourceRead(std::uint64_t bytes) noexcept;

// Bounded process-wide memo of (absolute path, identity) -> verified SHA-256. Entries are recorded
// only by a full hash whose bracketing observations matched. Thread-safe.
class VerifiedSourceDigests final {
  public:
    static constexpr std::size_t kEntryLimit = 4096;
    [[nodiscard]] std::optional<core::Sha256Digest> find(const std::filesystem::path& path,
                                                         const FileIdentity& identity) const;
    void record(const std::filesystem::path& path, const FileIdentity& identity,
                const core::Sha256Digest& digest);
    void forget(const std::filesystem::path& path);
    void clear();
    [[nodiscard]] std::size_t size() const;
};
[[nodiscard]] VerifiedSourceDigests& verifiedSourceDigests() noexcept;

// One cheap stat plus memo lookup. `digest` is set only when the current identity matches a
// recorded full hash.
struct SourceObservation final {
    std::optional<FileIdentity> identity;
    std::optional<core::Sha256Digest> digest;
};
[[nodiscard]] SourceObservation observeSource(const std::filesystem::path& path);

enum class SourceHashError : std::uint8_t { None, Missing, Oversized, Io, Changed, Cancelled };
struct SourceHash final {
    SourceHashError error = SourceHashError::None;
    core::Sha256Digest digest{};
    FileIdentity identity{};
};
// Streams one full SHA-256 pass (counted as a full read) and records it when the file identity is
// unchanged across the pass. A file modified during hashing reports Changed and records nothing.
[[nodiscard]] SourceHash
hashSourceFile(const std::filesystem::path& path, const std::function<bool()>& cancel = {},
               std::uint64_t sizeLimit = std::numeric_limits<std::uint64_t>::max());
// Memo first; a full hash only when the identity is unknown or changed.
[[nodiscard]] SourceHash
digestSourceFile(const std::filesystem::path& path, const std::function<bool()>& cancel = {},
                 std::uint64_t sizeLimit = std::numeric_limits<std::uint64_t>::max());

enum class SourceCheck : std::uint8_t { Matches, Changed, Missing, Cancelled };
[[nodiscard]] SourceCheck
verifySourceDigest(const std::filesystem::path& path, const core::Sha256Digest& expected,
                   const std::function<bool()>& cancel = {},
                   std::uint64_t sizeLimit = std::numeric_limits<std::uint64_t>::max());
} // namespace bloom::platform
