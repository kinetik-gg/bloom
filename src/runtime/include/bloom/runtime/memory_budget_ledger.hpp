#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>

namespace bloom::runtime {

inline constexpr std::size_t kDefaultOperationCacheBytes = std::size_t{1} * 1024U * 1024U * 1024U;
inline constexpr std::size_t kMinimumPreviewFrameCacheByteBudget =
    std::size_t{2} * 1024U * 1024U * 1024U;
// The low-memory floor: 3 GiB of cache in total, kept from CACHE-1 so a small machine still has a
// usable RAM preview. This floors the effective cap; pressure can reduce admission below it.
inline constexpr std::size_t kFallbackUsableMemoryBudget =
    kMinimumPreviewFrameCacheByteBudget + kDefaultOperationCacheBytes;

// The host reserve also determines when the eviction ladder engages.
inline constexpr std::size_t kMinimumHostMemoryReserve = std::size_t{8} * 1024U * 1024U * 1024U;
inline constexpr std::size_t kHostMemoryReservePercent = 40;
inline constexpr std::size_t kDefaultTotalPhysicalPercent = 50;
inline constexpr std::size_t kDefaultTotalAvailablePercent = 80;

// Configured ceilings. Live admission budgets are supplied by the ledger callbacks below.
struct MemoryBudgetAllocation final {
    std::size_t usableByteBudget = 0;
    std::size_t operationCacheByteBudget = 0;
    std::size_t previewFrameCacheByteBudget = 0;

    friend bool operator==(const MemoryBudgetAllocation&, const MemoryBudgetAllocation&) = default;
};

[[nodiscard]] std::size_t physicalMemoryBytes() noexcept;

// Missing availability is distinct from a measured zero. Swap counters are zero on a machine
// without swap or when the platform cannot report it; availability pressure remains supported.
struct MachineMemorySample final {
    std::optional<std::size_t> availableBytes;
    std::size_t swapTotalBytes = 0;
    std::size_t swapUsedBytes = 0;
};
[[nodiscard]] MachineMemorySample machineMemorySample() noexcept;
[[nodiscard]] std::size_t availableMemoryBytes() noexcept;
[[nodiscard]] std::size_t startupAvailableMemoryBytes() noexcept;

struct MemoryBudgetState final {
    std::size_t configuredBytes = 0;
    std::size_t effectiveBytes = 0;
    std::size_t retainedBytes = 0;
    MachineMemorySample machine;
    // Admission stays reduced between polls: 100 -> 25 -> 10, then 25 -> 50 -> 100 on recovery.
    unsigned retentionPercent = 100;
    bool memoryPressure = false;
    bool swapPressure = false;
    bool memoryNotice = false;
    bool swapNotice = false;
};

class MemoryBudgetLedger final {
  public:
    using Clock = std::chrono::steady_clock;
    // Zero availability leaves the configured defaults independent of startup contention. The
    // process ledger passes the startup sample explicitly to establish its initial effective cap.
    explicit MemoryBudgetLedger(std::size_t physicalMemory = physicalMemoryBytes(),
                                std::size_t availableMemory = 0) noexcept;

    // Every registered pool gets the same proportional policy. Ceilings are weights as well as
    // upper bounds; their sum is normalized to the configured total. Registration/removal and
    // callbacks are serialized, so removal waits for any in-progress callback before destruction.
    // Poll on the UI thread when UI-owned pools are registered. Callbacks must not perform I/O,
    // wait for workers, or modify registrations. Cache locks must never enclose ledger calls.
    void registerCache(const void* owner, std::size_t ceiling,
                       std::function<std::size_t()> retained,
                       std::function<void(std::size_t)> applyBudget);
    void unregisterCache(const void* owner);
    [[nodiscard]] std::size_t setCacheCeiling(const void* owner, std::size_t ceiling);
    [[nodiscard]] std::size_t cacheByteBudget(const void* owner) const;
    void setConfiguredTotal(std::size_t bytes);
    [[nodiscard]] MemoryBudgetState poll(MachineMemorySample sample,
                                         Clock::time_point now = Clock::now());
    [[nodiscard]] MemoryBudgetState state() const;

    // Upper bound for configured override weights. The live cap and callback budgets can be
    // smaller; an override is never a guaranteed minimum.
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
    struct Cache final {
        std::size_t ceiling;
        std::function<std::size_t()> retained;
        std::function<void(std::size_t)> applyBudget;
    };
    [[nodiscard]] std::size_t cacheByteBudgetLocked(std::size_t ceiling) const;
    [[nodiscard]] std::size_t capFor(const MachineMemorySample& sample,
                                     std::size_t retained) const noexcept;
    const std::size_t physicalMemory_;
    // Recursive only for readback from synchronous UI budget-change notifications.
    mutable std::recursive_mutex mutex_;
    std::map<const void*, Cache> caches_;
    MemoryBudgetState state_;
    std::optional<Clock::time_point> lastCapChange_;
    std::optional<Clock::time_point> recoveryStep_;
    unsigned pressurePolls_ = 0;
    bool hasPolled_ = false;
    std::size_t usableByteBudget_ = 0;
    std::size_t defaultTotalByteBudget_ = 0;
    std::size_t reserveByteBudget_ = 0;
};

// One narrowly scoped process budget coordinator, shared by all windows and cache owners.
[[nodiscard]] MemoryBudgetLedger& processMemoryBudgetLedger();
[[nodiscard]] std::size_t defaultOperationCacheByteBudget() noexcept;

} // namespace bloom::runtime
