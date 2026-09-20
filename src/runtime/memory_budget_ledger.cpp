#include <bloom/runtime/memory_budget_ledger.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// The page-file declarations require the Windows SDK types first.
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

namespace bloom::runtime {
namespace {

[[nodiscard]] std::size_t fraction(const std::size_t value, const std::size_t numerator,
                                   const std::size_t denominator) noexcept {
    const auto whole = value / denominator;
    const auto remainder = value % denominator;
    return whole * numerator + remainder * numerator / denominator;
}

[[nodiscard]] bool exceeds(const std::size_t left, const std::size_t right,
                           const std::size_t limit) noexcept {
    return left > limit || right > limit - left;
}

[[nodiscard]] std::size_t saturatedAdd(const std::size_t left, const std::size_t right) noexcept {
    return left + std::min(right, std::numeric_limits<std::size_t>::max() - left);
}

// Swap pressure is activity, not occupancy. A swap file can sit almost full for the life of the
// boot -- pages written long ago, untouched since -- while RAM is plentiful, so a static reading is
// never a reason to trim. Only sustained growth relative to the previous valid sample counts; the
// entry bar ignores small background churn, and once active a quarter-sized sustaining bar keeps a
// real episode latched without letting a borderline value flap every poll.
inline constexpr std::size_t kSwapGrowthMinimumBytes = std::size_t{16} * 1024U * 1024U;
inline constexpr std::size_t kSwapGrowthBytesPerSecond = std::size_t{4} * 1024U * 1024U;
inline constexpr std::size_t kSwapSustainDivisor = 4;

[[nodiscard]] std::size_t swapGrowthThreshold(const std::chrono::seconds elapsed,
                                              const bool sustaining) noexcept {
    const auto seconds =
        elapsed.count() > 0 ? static_cast<std::size_t>(elapsed.count()) : std::size_t{0};
    const auto rate = seconds > std::numeric_limits<std::size_t>::max() / kSwapGrowthBytesPerSecond
                          ? std::numeric_limits<std::size_t>::max()
                          : kSwapGrowthBytesPerSecond * seconds;
    const auto entry = std::max(kSwapGrowthMinimumBytes, rate);
    return sustaining ? std::max<std::size_t>(entry / kSwapSustainDivisor, 1U) : entry;
}

#if defined(__linux__)
[[nodiscard]] MachineMemorySample sampleProcMeminfo() {
    std::ifstream meminfo("/proc/meminfo");
    MachineMemorySample sample;
    std::optional<std::size_t> swapFree;
    std::string line;
    while (std::getline(meminfo, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const auto label = std::string_view(line).substr(0, colon);
        if (label != "MemAvailable" && label != "SwapTotal" && label != "SwapFree")
            continue;
        auto rest = std::string_view(line).substr(colon + 1);
        while (!rest.empty() && rest.front() == ' ')
            rest.remove_prefix(1);
        std::uint64_t kibibytes = 0;
        const auto parsed = std::from_chars(rest.data(), rest.data() + rest.size(), kibibytes);
        if (parsed.ec != std::errc{})
            continue;
        const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
                               kibibytes, std::numeric_limits<std::size_t>::max() / 1024U)) *
                           1024U;
        if (label == "MemAvailable")
            sample.availableBytes = bytes;
        else if (label == "SwapTotal")
            sample.swapTotalBytes = bytes;
        else
            swapFree = bytes;
    }
    if (swapFree)
        sample.swapUsedBytes = sample.swapTotalBytes - std::min(*swapFree, sample.swapTotalBytes);
    return sample;
}
#endif

} // namespace

std::size_t physicalMemoryBytes() noexcept {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0)
        return 0;
    return static_cast<std::size_t>(status.ullTotalPhys);
#elif defined(__APPLE__)
    std::uint64_t bytes = 0;
    auto size = sizeof(bytes);
    return sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0
               ? static_cast<std::size_t>(bytes)
               : 0;
#else
    const auto pages = sysconf(_SC_PHYS_PAGES);
    const auto pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages <= 0 || pageSize <= 0)
        return 0;
    const auto pagesAsSize = static_cast<std::size_t>(pages);
    const auto pageSizeAsSize = static_cast<std::size_t>(pageSize);
    if (pagesAsSize > std::numeric_limits<std::size_t>::max() / pageSizeAsSize)
        return std::numeric_limits<std::size_t>::max();
    return pagesAsSize * pageSizeAsSize;
#endif
}

MachineMemorySample machineMemorySample() noexcept {
    MachineMemorySample sample;
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0)
        sample.availableBytes = static_cast<std::size_t>(status.ullAvailPhys);
    // Actual page-file usage, not commit charge (ullTotalPageFile includes physical RAM).
    const auto callback = [](LPVOID context, PENUM_PAGE_FILE_INFORMATION info, LPCWSTR) -> BOOL {
        auto& result = *static_cast<MachineMemorySample*>(context);
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        result.swapTotalBytes =
            saturatedAdd(result.swapTotalBytes, info->TotalSize * system.dwPageSize);
        result.swapUsedBytes =
            saturatedAdd(result.swapUsedBytes, info->TotalInUse * system.dwPageSize);
        return TRUE;
    };
    static_cast<void>(K32EnumPageFilesW(callback, &sample));
#elif defined(__APPLE__)
    const auto host = mach_host_self();
    vm_statistics64_data_t statistics{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t pageSize = 0;
    if (host_page_size(host, &pageSize) == KERN_SUCCESS &&
        host_statistics64(
            host, HOST_VM_INFO64,
            reinterpret_cast<host_info64_t>(&statistics), // NOLINT(*-reinterpret-cast)
            &count) == KERN_SUCCESS) {
        // The Linux sampler reads MemAvailable, which includes reclaimable page cache. macOS keeps
        // most file cache in active/inactive pages, so free + inactive alone understates what the
        // system can hand back and falsely declares pressure on a machine that is merely caching.
        // Count free, inactive, speculative and purgeable pages, plus file-backed pages, which the
        // VM reclaims under pressure.
        auto pages = static_cast<std::size_t>(statistics.free_count);
        pages = saturatedAdd(pages, static_cast<std::size_t>(statistics.inactive_count));
        pages = saturatedAdd(pages, static_cast<std::size_t>(statistics.speculative_count));
        pages = saturatedAdd(pages, static_cast<std::size_t>(statistics.purgeable_count));
        pages = saturatedAdd(pages, static_cast<std::size_t>(statistics.external_page_count));
        sample.availableBytes = pages * pageSize;
    }
    mach_port_deallocate(mach_task_self(), host);
    xsw_usage swap{};
    auto size = sizeof(swap);
    if (sysctlbyname("vm.swapusage", &swap, &size, nullptr, 0) == 0) {
        sample.swapTotalBytes = static_cast<std::size_t>(swap.xsu_total);
        sample.swapUsedBytes = static_cast<std::size_t>(swap.xsu_used);
    }
#else
#if defined(__linux__)
    try {
        sample = sampleProcMeminfo();
    } catch (...) {
        // Sampling is best effort even while the process is short of allocation headroom.
        sample = {};
    }
#endif
    if (!sample.availableBytes) {
        const auto pages = sysconf(_SC_AVPHYS_PAGES);
        const auto pageSize = sysconf(_SC_PAGE_SIZE);
        if (pages >= 0 && pageSize > 0)
            sample.availableBytes =
                static_cast<std::size_t>(pages) * static_cast<std::size_t>(pageSize);
    }
#endif
    return sample;
}

std::size_t availableMemoryBytes() noexcept {
    return machineMemorySample().availableBytes.value_or(0);
}

std::size_t startupAvailableMemoryBytes() noexcept {
    static const std::size_t latched = availableMemoryBytes();
    return latched;
}

std::size_t
MemoryBudgetLedger::reserveForPhysicalMemory(const std::size_t physicalMemory) noexcept {
    return std::max(kMinimumHostMemoryReserve,
                    fraction(physicalMemory, kHostMemoryReservePercent, 100));
}

std::size_t
MemoryBudgetLedger::usableByteBudgetForPhysicalMemory(const std::size_t physicalMemory) noexcept {
    if (physicalMemory == 0)
        return kFallbackUsableMemoryBudget;

    const auto reserve = reserveForPhysicalMemory(physicalMemory);
    const auto afterReserve = physicalMemory > reserve ? physicalMemory - reserve : 0;
    // Keep the historic low-memory floor useful when the reserve would otherwise leave no room for
    // either cache. Never invent more budget than the machine reports in this fallback branch.
    const auto lowMemoryFallback = std::min(physicalMemory, kFallbackUsableMemoryBudget);
    return std::max(afterReserve, lowMemoryFallback);
}

MemoryBudgetLedger::MemoryBudgetLedger(const std::size_t physicalMemory,
                                       const std::size_t availableMemory) noexcept
    : physicalMemory_(physicalMemory),
      usableByteBudget_(usableByteBudgetForPhysicalMemory(physicalMemory)),
      reserveByteBudget_(physicalMemory == 0 ? kMinimumHostMemoryReserve
                                             : reserveForPhysicalMemory(physicalMemory)) {
    // The default total is the most conservative of three independent ceilings, then raised back to
    // the low-memory floor so a small or busy machine still gets a working cache rather than none.
    auto total = usableByteBudget_;
    if (physicalMemory != 0)
        total = std::min(total, fraction(physicalMemory, kDefaultTotalPhysicalPercent, 100));
    if (availableMemory != 0)
        total = std::min(total, fraction(availableMemory, kDefaultTotalAvailablePercent, 100));
    const auto floor = physicalMemory == 0 ? kFallbackUsableMemoryBudget
                                           : std::min(physicalMemory, kFallbackUsableMemoryBudget);
    defaultTotalByteBudget_ = std::min(usableByteBudget_, std::max(total, floor));
    state_.configuredBytes = std::max(
        floor, std::min(usableByteBudget_,
                        physicalMemory == 0 ? usableByteBudget_ : fraction(physicalMemory, 1, 2)));
    state_.effectiveBytes = defaultTotalByteBudget_;
    if (availableMemory != 0)
        state_.machine.availableBytes = availableMemory;
}

MemoryBudgetAllocation
MemoryBudgetLedger::allocate(const std::optional<std::size_t> operationOverride,
                             const std::optional<std::size_t> previewOverride) const noexcept {
    const auto defaultOperation = fraction(defaultTotalByteBudget_, 3, 5);
    const auto defaultPreview =
        std::max(defaultTotalByteBudget_ - defaultOperation, kMinimumPreviewFrameCacheByteBudget);

    std::size_t operation = defaultOperation;
    std::size_t preview = defaultPreview;
    if (!operationOverride.has_value() && !previewOverride.has_value()) {
        // The preview floor can ask for more than the default total left for it; the total wins, so
        // the operation cache gives the difference back.
        if (exceeds(operation, preview, defaultTotalByteBudget_))
            operation = defaultTotalByteBudget_ - std::min(preview, defaultTotalByteBudget_);
    } else if (operationOverride.has_value() && previewOverride.has_value()) {
        operation = std::min(*operationOverride, usableByteBudget_);
        preview = std::min(*previewOverride, usableByteBudget_);
        if (exceeds(operation, preview, usableByteBudget_)) {
            const auto total =
                static_cast<long double>(operation) + static_cast<long double>(preview);
            operation =
                static_cast<std::size_t>(static_cast<long double>(operation) *
                                         static_cast<long double>(usableByteBudget_) / total);
            preview = usableByteBudget_ - operation;
        }
    } else if (operationOverride.has_value()) {
        operation = std::min(*operationOverride, usableByteBudget_);
        preview = std::min(defaultPreview, usableByteBudget_ - operation);
    } else {
        preview = std::min(*previewOverride, usableByteBudget_);
        operation = std::min(defaultOperation, usableByteBudget_ - preview);
    }

    return {.usableByteBudget = usableByteBudget_,
            .operationCacheByteBudget = operation,
            .previewFrameCacheByteBudget = preview};
}

void MemoryBudgetLedger::registerCache(const void* owner, const std::size_t ceiling,
                                       std::function<std::size_t()> retained,
                                       std::function<void(std::size_t)> applyBudget) {
    const std::lock_guard lock(mutex_);
    const auto entry =
        caches_.insert_or_assign(owner, Cache{ceiling, std::move(retained), std::move(applyBudget)})
            .first;
    // A pool created during pressure must not regain a full ceiling until the next timer tick.
    // Its owner has initialized its storage before registration and is not published yet.
    if (hasPolled_) {
        try {
            entry->second.applyBudget(cacheByteBudgetLocked(ceiling));
        } catch (...) {
            caches_.erase(entry);
            throw;
        }
    }
}

void MemoryBudgetLedger::unregisterCache(const void* owner) {
    const std::lock_guard lock(mutex_);
    caches_.erase(owner);
}

std::size_t MemoryBudgetLedger::setCacheCeiling(const void* owner, const std::size_t ceiling) {
    const std::lock_guard lock(mutex_);
    const auto found = caches_.find(owner);
    if (found == caches_.end())
        return ceiling;
    found->second.ceiling = ceiling;
    const auto bytes = hasPolled_ ? cacheByteBudgetLocked(ceiling) : ceiling;
    // Apply while still serialized with polls; a caller must not publish an old allowance after
    // a concurrent pressure callback has already reduced it.
    found->second.applyBudget(bytes);
    return bytes;
}

std::size_t MemoryBudgetLedger::cacheByteBudgetLocked(const std::size_t ceiling) const {
    long double total = 0;
    for (const auto& [owner, cache] : caches_)
        total += static_cast<long double>(cache.ceiling);
    total = std::max(total, static_cast<long double>(state_.configuredBytes));
    if (total == 0)
        return 0;
    const auto scaled = static_cast<std::size_t>(
        static_cast<long double>(ceiling) *
        std::min(total, static_cast<long double>(state_.effectiveBytes)) / total);
    return fraction(std::min(ceiling, scaled), state_.retentionPercent, 100);
}

std::size_t MemoryBudgetLedger::cacheByteBudget(const void* owner) const {
    const std::lock_guard lock(mutex_);
    const auto found = caches_.find(owner);
    return found == caches_.end() ? 0 : cacheByteBudgetLocked(found->second.ceiling);
}

void MemoryBudgetLedger::setConfiguredTotal(const std::size_t bytes) {
    const std::lock_guard lock(mutex_);
    state_.configuredBytes = bytes;
    state_.effectiveBytes = capFor(state_.machine, state_.retainedBytes);
    lastCapChange_.reset();
}

std::size_t MemoryBudgetLedger::capFor(const MachineMemorySample& sample,
                                       const std::size_t retained) const noexcept {
    auto cap = state_.configuredBytes;
    if (physicalMemory_ != 0)
        cap = std::min(cap, fraction(physicalMemory_, 1, 2));
    if (sample.availableBytes)
        cap = std::min(cap, fraction(saturatedAdd(*sample.availableBytes, retained), 4, 5));
    // The policy floor never forces a client above its explicitly smaller ceiling.
    return std::max(cap, physicalMemory_ == 0
                             ? kFallbackUsableMemoryBudget
                             : std::min(physicalMemory_, kFallbackUsableMemoryBudget));
}

bool MemoryBudgetLedger::observeSwapActivity(const MachineMemorySample& sample,
                                             const Clock::time_point now,
                                             const bool wasSwapPressure) noexcept {
    if (sample.swapTotalBytes == 0) {
        // No swap configured, or the platform cannot report it. Drop the baseline so a later valid
        // sample is measured from scratch instead of from a stale reading.
        lastSwapUsedBytes_.reset();
        lastSwapSampleTime_.reset();
        return false;
    }
    if (!lastSwapUsedBytes_ || !lastSwapSampleTime_ || now <= *lastSwapSampleTime_ ||
        sample.swapUsedBytes < *lastSwapUsedBytes_) {
        // The first valid sample, a clock that did not advance or moved backwards, or swap that was
        // partly released all rebaseline. Releasing from a peak means a later rise is measured from
        // the new low, not against the pre-release peak.
        lastSwapUsedBytes_ = sample.swapUsedBytes;
        lastSwapSampleTime_ = now;
        return false;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(now - *lastSwapSampleTime_);
    const auto growth = sample.swapUsedBytes - *lastSwapUsedBytes_;
    lastSwapUsedBytes_ = sample.swapUsedBytes;
    lastSwapSampleTime_ = now;
    return growth >= swapGrowthThreshold(elapsed, wasSwapPressure);
}

MemoryBudgetState MemoryBudgetLedger::poll(MachineMemorySample sample,
                                           const Clock::time_point now) {
    const std::lock_guard lock(mutex_);
    hasPolled_ = true;
    std::size_t retained = 0;
    for (const auto& [owner, cache] : caches_)
        retained = saturatedAdd(retained, cache.retained());
    state_.retainedBytes = retained;
    const bool wasPressure = state_.memoryPressure || state_.swapPressure;
    const bool wasSwap = state_.swapPressure;
    // An unavailable sample cannot assert recovery or discard an existing pressure episode.
    if (sample.availableBytes)
        state_.memoryPressure = *sample.availableBytes < reserveByteBudget_;
    // Swap only asserts while it is actively growing. Missing counters are not evidence of
    // recovery, so an existing episode is retained until a valid sample clears it through the
    // baseline reset.
    const bool swapGrowing = observeSwapActivity(sample, now, wasSwap);
    if (sample.swapTotalBytes != 0)
        state_.swapPressure = swapGrowing;
    const bool pressure = state_.memoryPressure || state_.swapPressure;
    state_.memoryNotice = pressure && !wasPressure;
    state_.swapNotice = state_.swapPressure && !wasSwap;
    state_.machine = sample;
    bool restore = false;
    if (pressure) {
        pressurePolls_ = std::min(pressurePolls_ + 1, 2U);
        state_.retentionPercent =
            std::min(state_.retentionPercent, pressurePolls_ >= 2 ? 10U : 25U);
        recoveryStep_.reset();
    } else if (sample.availableBytes) {
        pressurePolls_ = 0;
        if (!recoveryStep_)
            recoveryStep_ = now;
        if (now - *recoveryStep_ >= std::chrono::seconds(10)) {
            restore = true;
            recoveryStep_ = now;
            if (state_.retentionPercent < 25)
                state_.retentionPercent = 25;
            else if (state_.retentionPercent < 50)
                state_.retentionPercent = 50;
            else
                state_.retentionPercent = 100;
        }
    }
    auto candidate = capFor(sample, retained);
    if (candidate > state_.effectiveBytes) {
        if (!restore)
            candidate = state_.effectiveBytes;
        else
            candidate =
                std::min(candidate, saturatedAdd(state_.effectiveBytes, state_.effectiveBytes / 4));
    }
    const auto difference = candidate > state_.effectiveBytes ? candidate - state_.effectiveBytes
                                                              : state_.effectiveBytes - candidate;
    if (difference > state_.effectiveBytes / 10 &&
        (!lastCapChange_ || now - *lastCapChange_ >= std::chrono::seconds(5))) {
        state_.effectiveBytes = candidate;
        lastCapChange_ = now;
    }
    for (const auto& [owner, cache] : caches_)
        cache.applyBudget(cacheByteBudgetLocked(cache.ceiling));
    return state_;
}

MemoryBudgetState MemoryBudgetLedger::state() const {
    const std::lock_guard lock(mutex_);
    return state_;
}

MemoryBudgetLedger& processMemoryBudgetLedger() {
    static MemoryBudgetLedger ledger(physicalMemoryBytes(), startupAvailableMemoryBytes());
    return ledger;
}

std::size_t defaultOperationCacheByteBudget() noexcept {
    return MemoryBudgetLedger{}.allocate().operationCacheByteBudget;
}

} // namespace bloom::runtime
