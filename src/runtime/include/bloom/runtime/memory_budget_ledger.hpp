#pragma once

#include <bloom/runtime/process_memory_sample.hpp>

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
// Automatic defaults use this total when physical capacity is unavailable, bounded by reported
// physical capacity on smaller machines. Explicit preferences never inherit this floor.
inline constexpr std::size_t kFallbackUsableMemoryBudget =
    kMinimumPreviewFrameCacheByteBudget + kDefaultOperationCacheBytes;

inline constexpr std::size_t kDefaultTotalPhysicalPercent = 50;

// Selected per-pool budgets. Real process constraints can separately reduce reusable admission.
struct MemoryBudgetAllocation final {
    std::size_t usableByteBudget = 0;
    std::size_t operationCacheByteBudget = 0;
    std::size_t previewFrameCacheByteBudget = 0;

    friend bool operator==(const MemoryBudgetAllocation&, const MemoryBudgetAllocation&) = default;
};

[[nodiscard]] std::size_t physicalMemoryBytes() noexcept;

// Missing availability is distinct from a measured zero. Swap counters are zero on a machine
// without swap or when the platform cannot report it; observed activity never rewrites targets.
struct MachineMemorySample final {
    std::optional<std::size_t> availableBytes;
    std::size_t swapTotalBytes = 0;
    std::size_t swapUsedBytes = 0;
};
[[nodiscard]] MachineMemorySample machineMemorySample() noexcept;
[[nodiscard]] std::size_t availableMemoryBytes() noexcept;
[[nodiscard]] std::size_t startupAvailableMemoryBytes() noexcept;

struct MemoryBudgetSample final {
    MachineMemorySample machine;
    std::size_t physicalBytes = 0;
    ProcessMemorySample process;
};
// Sampling is worker-only; applying a completed snapshot remains the owner's thread responsibility.
[[nodiscard]] MemoryBudgetSample
memoryBudgetSample(const CancellationToken& cancellation = {}) noexcept;

enum class MemoryBudgetOwnerKind { ReusableRetention, LiveWork };

struct MemoryBudgetState final {
    // Selected operation/preview total and that total within the observed absolute OS limit.
    // Auxiliary registered pools can share a finite limit; inspect owners for actual pool budgets.
    std::size_t configuredBytes = 0;
    std::size_t effectiveBytes = 0;
    std::size_t retainedBytes = 0;
    std::size_t reusableHostCreditBytes = 0;
    // Hardware capacity only. Live phase admission separately checks fresh usage and reservations.
    std::size_t processingEffectiveBytes = 0;
    ProcessMemorySample process;
    std::optional<std::size_t> processCacheCeilingBytes;
    bool awaitingProcessSample = false;
    bool processSampleFresh = false;
    std::uint64_t sampleSerial = 0;
    MachineMemorySample machine;
    // Compatibility field: named budgets no longer use a percentage retention policy.
    unsigned retentionPercent = 100;
    bool memoryPressure = false;
    bool swapPressure = false;
    bool memoryNotice = false;
    bool swapNotice = false;
};

class MemoryBudgetLedger final {
  public:
    using Clock = std::chrono::steady_clock;
    // Construction never probes the operating system. Explicit injected host capacity is useful
    // for standalone owners/tests; production activates bounded asynchronous process sampling.
    explicit MemoryBudgetLedger(std::size_t physicalMemory = 0,
                                std::size_t availableMemory = 0) noexcept;

    // A registered ceiling is that pool's selected allowance. Only an observed finite OS limit can
    // require reusable pools to divide a shared total. Live work is separately phase-admitted.
    // Registration/removal and callbacks are serialized, so removal waits for any in-progress
    // callback before destruction.
    // Poll on the UI thread when UI-owned pools are registered. Callbacks must not perform I/O,
    // wait for workers, or modify registrations. Cache locks must never enclose ledger calls.
    void registerCache(const void* owner, std::size_t ceiling,
                       std::function<std::size_t()> retained,
                       std::function<void(std::size_t)> applyBudget,
                       MemoryBudgetOwnerKind kind = MemoryBudgetOwnerKind::ReusableRetention);
    void unregisterCache(const void* owner);
    [[nodiscard]] std::size_t setCacheCeiling(const void* owner, std::size_t ceiling);
    [[nodiscard]] std::size_t cacheByteBudget(const void* owner) const;
    void setConfiguredTotal(std::size_t bytes);
    // The RAM headroom preference. Once set, each reusable pool may hold what it retains now plus
    // all measured available memory beyond the headroom: caches fill usable memory and are evicted
    // on demand (admission reclaimers, the next sample). An observed OS limit still applies.
    void setRamHeadroom(std::size_t bytes);
    [[nodiscard]] MemoryBudgetState poll(MachineMemorySample sample,
                                         Clock::time_point now = Clock::now());
    [[nodiscard]] MemoryBudgetState applySample(MemoryBudgetSample sample,
                                                Clock::time_point now = Clock::now());
    // Production activates this before constructing cache owners; it never gates live processing.
    void beginProcessSampling();
    [[nodiscard]] MemoryBudgetState state() const;

    // Reported physical capacity, or the automatic fallback when unavailable. This is not a clamp
    // on selected per-pool preferences and is not a whole-process allocation guarantee.
    [[nodiscard]] std::size_t usableByteBudget() const noexcept {
        const std::lock_guard lock(mutex_);
        return usableByteBudget_;
    }

    // The automatic split is chosen from physical capacity once, independently of free RAM.
    [[nodiscard]] std::size_t defaultTotalByteBudget() const noexcept {
        const std::lock_guard lock(mutex_);
        return defaultTotalByteBudget_;
    }

    // Compatibility accessor. Cache retention has no implicit host-memory reserve.
    [[nodiscard]] std::size_t reserveByteBudget() const noexcept {
        const std::lock_guard lock(mutex_);
        return reserveByteBudget_;
    }

    [[nodiscard]] std::size_t physicalByteBudget() const noexcept {
        const std::lock_guard lock(mutex_);
        return physicalMemory_;
    }

    // Missing/zero overrides select each pool's automatic default. Every nonzero override is exact
    // and independent of the other selection, physical capacity, and registered auxiliary pools.
    [[nodiscard]] MemoryBudgetAllocation
    allocate(std::optional<std::size_t> operationOverride = {},
             std::optional<std::size_t> previewOverride = {}) const noexcept;

    [[nodiscard]] static std::size_t
    usableByteBudgetForPhysicalMemory(std::size_t physicalMemory) noexcept;
    [[nodiscard]] static std::size_t reserveForPhysicalMemory(std::size_t physicalMemory) noexcept;

  private:
    struct Cache final {
        std::size_t ceiling;
        MemoryBudgetOwnerKind kind;
        std::function<std::size_t()> retained;
        std::function<void(std::size_t)> applyBudget;
    };
    [[nodiscard]] std::size_t cacheByteBudgetLocked(const Cache& cache) const;
    // Returns true only while swap is actively growing beyond the entry/sustain threshold. Static
    // occupancy, a first sample, a released sample, a non-advancing clock and missing counters all
    // rebaseline and assert nothing.
    [[nodiscard]] bool observeSwapActivity(const MachineMemorySample& sample, Clock::time_point now,
                                           bool wasSwapPressure) noexcept;
    void updatePhysicalCapacity(std::size_t physical);
    void applyBudgetsLocked();
    void applyProcessCeilingLocked();
    [[nodiscard]] MemoryBudgetState pollHostLocked(MachineMemorySample sample,
                                                   Clock::time_point now);
    std::size_t physicalMemory_;
    // Recursive only for readback from synchronous UI budget-change notifications.
    mutable std::recursive_mutex mutex_;
    std::map<const void*, Cache> caches_;
    MemoryBudgetState state_;
    // Swap is judged by change, never by occupancy: these carry the previous valid sample.
    std::optional<std::size_t> lastSwapUsedBytes_;
    std::optional<Clock::time_point> lastSwapSampleTime_;
    bool hasPolled_ = false;
    bool processSampling_ = false;
    std::optional<std::size_t> ramHeadroom_;
    bool defaultsResolved_ = false;
    bool configuredTotalSet_ = false;
    std::optional<Clock::time_point> lastProcessSample_;
    ProcessMemoryConstraintHistory processConstraintHistory_;
    std::size_t usableByteBudget_ = 0;
    std::size_t defaultTotalByteBudget_ = 0;
    std::size_t reserveByteBudget_ = 0;
};

// One narrowly scoped process budget coordinator, shared by all windows and cache owners.
[[nodiscard]] MemoryBudgetLedger& processMemoryBudgetLedger();
[[nodiscard]] std::size_t defaultOperationCacheByteBudget() noexcept;

} // namespace bloom::runtime
