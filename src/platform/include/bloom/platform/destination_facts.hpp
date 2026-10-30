#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

// Export destination facts (ADR 0033 section 7, issue #369). Blocking filesystem queries: call
// them only on a blocking-I/O worker, never on the UI thread.
namespace bloom::platform {

struct DestinationFolderFacts final {
    // The folder itself exists.
    bool exists = false;
    // The folder, or when it does not exist yet its nearest existing ancestor, accepts new files
    // (the run creates the missing folders).
    bool writable = false;
    // Bytes available to this user on the destination volume; absent when not reportable.
    std::optional<std::uint64_t> availableBytes;
};

[[nodiscard]] DestinationFolderFacts destinationFolderFacts(const std::filesystem::path& folder);

// The safety margin kept free on a destination volume: 2% of the expected size, at least 64 MiB.
[[nodiscard]] constexpr std::uint64_t
destinationSafetyMargin(const std::uint64_t expected) noexcept {
    const auto proportional = expected / 50U;
    return proportional > (64ULL << 20U) ? proportional : (64ULL << 20U);
}

// Removes stale staged files (".bloom-stage-<pid>-<target>-<nonce>") left in `target`'s folder
// for exactly that target by a process that is no longer alive. Never touches other files or the
// stages of a live process. Returns how many were removed.
std::uint64_t removeStaleStagedArtifacts(const std::filesystem::path& target) noexcept;

} // namespace bloom::platform
