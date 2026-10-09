#pragma once

#include <bloom/runtime/cancellation.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::runtime {

inline constexpr std::size_t kMaximumProcessMemoryDomains = 32;
inline constexpr std::size_t kMaximumProcessMemoryScalarBytes = 4096;

enum class ProcessMemorySource {
    Unavailable,
    LinuxCgroupV2,
    LinuxCgroupV1Unsupported,
    WindowsHostOnly,
    MacOsHostOnly,
    OtherHostOnly,
};

// NoFiniteVisibleLimit means every visible domain was successfully read, not that hidden
// ancestors or unmeasured process/driver allocations cannot constrain the application.
enum class ProcessMemoryStatus {
    Pending,
    Finite,
    NoFiniteVisibleLimit,
    Partial,
    Unavailable,
    Unsupported,
};

enum class ProcessMemoryLimitKind { Unknown, Finite, Unlimited };

struct ProcessMemoryDomain final {
    std::string path;
    ProcessMemoryLimitKind kind = ProcessMemoryLimitKind::Unknown;
    std::size_t limitBytes = 0;
    std::optional<std::size_t> currentBytes;
};

// A bounded neutral worker snapshot. Every usage belongs to its own limit domain; callers must
// never combine a leaf usage with an ancestor limit or sum independent domain headrooms.
struct ProcessMemorySample final {
    ProcessMemorySource source = ProcessMemorySource::Unavailable;
    ProcessMemoryStatus status = ProcessMemoryStatus::Unavailable;
    std::chrono::steady_clock::time_point sampledAt{};
    std::string membershipPath;
    std::vector<ProcessMemoryDomain> domains;
};

enum class ProcessMemoryReadStatus { Value, Missing, Unavailable, TooLarge };
struct ProcessMemoryFileRead final {
    ProcessMemoryReadStatus status = ProcessMemoryReadStatus::Unavailable;
    std::string text;
};
using ProcessMemoryFileReader = std::function<ProcessMemoryFileRead(const std::filesystem::path&)>;

// Pure topology/parser seam. Text and scalar reads are bounded even for injected fixtures.
// A v1 hierarchy is explicitly Unsupported in this slice; it never impersonates an unlimited v2
// sample. Sampling/read failures retain Partial/Unavailable provenance.
[[nodiscard]] ProcessMemorySample parseLinuxProcessMemorySample(
    std::string_view cgroupText, std::string_view mountInfoText,
    const ProcessMemoryFileReader& readScalar,
    std::chrono::steady_clock::time_point sampledAt = std::chrono::steady_clock::now()) noexcept;

// OS/filesystem sampling belongs on a worker, never a UI thread. Windows/macOS return explicit
// host-only process-limit-unavailable provenance; their existing host sampler remains separate.
[[nodiscard]] ProcessMemorySample sampleProcessMemory(CancellationToken cancellation = {}) noexcept;

[[nodiscard]] std::size_t reserveForProcessMemoryLimit(std::size_t limit) noexcept;

// Saturating paired-domain formula. reusableHostBytes is exclusively owned, nonduplicated host
// retention only; unproved/shared/device/scratch allocations receive zero credit.
[[nodiscard]] std::size_t processCacheAllowanceForDomain(std::size_t limit, std::size_t current,
                                                         std::size_t reusableHostBytes,
                                                         std::size_t reserve) noexcept;
[[nodiscard]] std::optional<std::size_t>
minimumProcessCacheAllowance(const ProcessMemorySample& sample,
                             std::size_t reusableHostBytes) noexcept;

} // namespace bloom::runtime
