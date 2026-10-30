#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

// Small per-user secrets on disk (ADR 0034: the MCP server's bearer token). Qt-free.
//
// A secret file is readable and writable by its owner only: mode 0600 in a 0700 directory on Linux
// and macOS, and a protected DACL granting only the current user's SID on Windows. Writes replace
// the file atomically (write to a sibling owner-only temporary, flush, rename), so a reader never
// sees a partial secret. These functions perform filesystem I/O: call them on a blocking-I/O
// worker, never on the UI thread. They never log or echo the secret's value.
namespace bloom::platform {

enum class OwnerOnlySecretStatus : std::uint8_t {
    Ok = 0,
    // No file yet. The caller generates a secret and writes it.
    Missing = 1,
    // The file exists but other users can read or write it (POSIX group/other bits, or a Windows
    // DACL that grants anyone else). The value is not returned; the caller replaces it.
    ExposedToOtherUsers = 2,
    // Larger than the caller's limit, empty, or not printable ASCII.
    Malformed = 3,
    // The directory or file cannot be created, read or written.
    Failed = 4,
};

struct OwnerOnlySecretRead final {
    OwnerOnlySecretStatus status = OwnerOnlySecretStatus::Failed;
    // Set only for Ok.
    std::string value;
    // Plain language; never contains the value.
    std::string message;
};

struct OwnerOnlySecretWrite final {
    OwnerOnlySecretStatus status = OwnerOnlySecretStatus::Failed;
    std::string message;
};

// Fills `bytes` from the operating system's CSPRNG (getrandom on Linux, getentropy on macOS,
// BCryptGenRandom with the system-preferred RNG on Windows). Returns false only if the system
// source is unavailable; the buffer is then zeroed. Non-blocking after system start-up.
[[nodiscard]] bool fillSecureRandom(std::span<std::byte> bytes) noexcept;

// Reads a secret of at most `maximumBytes` printable ASCII bytes. Blocking I/O.
[[nodiscard]] OwnerOnlySecretRead readOwnerOnlySecret(const std::filesystem::path& file,
                                                      std::size_t maximumBytes);

// Creates the parent directory owner-only when missing and atomically replaces `file` with
// `value`. Blocking I/O.
[[nodiscard]] OwnerOnlySecretWrite writeOwnerOnlySecret(const std::filesystem::path& file,
                                                        std::string_view value);

} // namespace bloom::platform
