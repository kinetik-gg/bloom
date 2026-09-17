#pragma once

#include <cstddef>
#include <optional>

namespace bloom::runtime {

inline constexpr std::size_t kDefaultOperationCacheBytes = std::size_t{1} * 1024U * 1024U * 1024U;
inline constexpr std::size_t kMinimumPreviewFrameCacheByteBudget =
    std::size_t{2} * 1024U * 1024U * 1024U;
// The low-memory floor: 3 GiB of cache in total, kept from CACHE-1 so a small machine still has a
// usable RAM preview. Every rule below may reduce a budget only down TO this floor.
inline constexpr std::size_t kFallbackUsableMemoryBudget =
    kMinimumPreviewFrameCacheByteBudget + kDefaultOperationCacheBytes;

// CACHEFIX-1. The reserve is what Bloom leaves to the rest of the machine -- the kernel, the
// compositor, a browser, and the swap that on a workstation is often only a few GiB. CACHE-1
// reserved max(4 GiB, 25%), which on a 60 GiB machine let one process plan to hold 45 GiB and
// froze the desktop.
inline constexpr std::size_t kMinimumHostMemoryReserve = std::size_t{8} * 1024U * 1024U * 1024U;
inline constexpr std::size_t kHostMemoryReservePercent = 40;
// A second, independent ceiling on the DEFAULT total: whatever the reserve arithmetic allows, the
// caches Bloom gives itself without being asked never add up to more than half the machine.
inline constexpr std::size_t kDefaultTotalPhysicalPercent = 50;
// And a ceiling derived from what is actually free at startup, so launching Bloom next to a loaded
// machine does not immediately plan to consume memory that is already spoken for.
inline constexpr std::size_t kDefaultTotalAvailablePercent = 80;

// The ledger is deliberately a budget calculator, not a memory owner. The application composition
// root resolves these numbers once and gives each cache its effective allowance.
struct MemoryBudgetAllocation final {
    std::size_t usableByteBudget = 0;
    std::size_t operationCacheByteBudget = 0;
    std::size_t previewFrameCacheByteBudget = 0;

    friend bool operator==(const MemoryBudgetAllocation&, const MemoryBudgetAllocation&) = default;
};

[[nodiscard]] std::size_t physicalMemoryBytes() noexcept;

// Memory the operating system says it can hand out right now without swapping: MemAvailable from
// /proc/meminfo on Linux, sysconf(_SC_AVPHYS_PAGES) where that file cannot be read, ullAvailPhys on
// Windows. 0 means "the platform did not say", which every caller reads as "do not apply the
// availability cap" rather than as zero free memory.
[[nodiscard]] std::size_t availableMemoryBytes() noexcept;

// The FIRST reading of availableMemoryBytes() this process ever took, latched. Every ledger built
// with default arguments -- in the composition root, in the status bar, in a test -- must agree on
// one number, or two ledgers constructed a second apart would hand out different budgets for the
// same machine. The default budget is a decision taken at startup; the live reading above is what
// the runtime pressure response polls.
[[nodiscard]] std::size_t startupAvailableMemoryBytes() noexcept;

class MemoryBudgetLedger final {
  public:
    // Supplying physical and available memory makes the policy deterministic for tests. The
    // defaults read the host once, at construction, so callers do not observe a moving
    // machine-sized budget -- and, for availability, so the default budget is a decision taken at
    // startup rather than a number that drifts with whatever else the artist opened. Runtime
    // pressure is answered by trimming the caches (see WindowStatusBar), not by re-deriving this.
    explicit MemoryBudgetLedger(
        std::size_t physicalMemory = physicalMemoryBytes(),
        std::size_t availableMemory = startupAvailableMemoryBytes()) noexcept;

    // The ceiling an EXPLICIT override may reach: physical memory minus the host reserve. An artist
    // who asks for more than Bloom would choose on its own still gets what they asked for, up to
    // the point where the machine itself would be starved.
    [[nodiscard]] std::size_t usableByteBudget() const noexcept { return usableByteBudget_; }

    // The ceiling the DEFAULT split may reach: min(usable, 50% of physical, 80% of available),
    // never below the 3 GiB low-memory floor. This is the number that keeps Bloom from planning to
    // own the machine when nobody configured anything.
    [[nodiscard]] std::size_t defaultTotalByteBudget() const noexcept {
        return defaultTotalByteBudget_;
    }

    // What this ledger left to the rest of the machine: max(8 GiB, 40% of physical). Also the
    // threshold the runtime pressure response compares MemAvailable against.
    [[nodiscard]] std::size_t reserveByteBudget() const noexcept { return reserveByteBudget_; }

    // Missing overrides split `defaultTotalByteBudget()` 60/40 operation/preview. A single override
    // keeps its exact value where possible, clamped to `usableByteBudget()`, and reduces the other
    // allocation. If both overrides exceed the ledger, they are proportionally reduced so neither
    // setting is silently discarded.
    [[nodiscard]] MemoryBudgetAllocation
    allocate(std::optional<std::size_t> operationOverride = {},
             std::optional<std::size_t> previewOverride = {}) const noexcept;

    [[nodiscard]] static std::size_t
    usableByteBudgetForPhysicalMemory(std::size_t physicalMemory) noexcept;
    [[nodiscard]] static std::size_t reserveForPhysicalMemory(std::size_t physicalMemory) noexcept;

  private:
    std::size_t usableByteBudget_ = 0;
    std::size_t defaultTotalByteBudget_ = 0;
    std::size_t reserveByteBudget_ = 0;
};

[[nodiscard]] std::size_t defaultOperationCacheByteBudget() noexcept;

} // namespace bloom::runtime
