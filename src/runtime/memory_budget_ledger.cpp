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

std::size_t MemoryBudgetLedger::reserveForPhysicalMemory(const std::size_t) noexcept { return 0; }

std::size_t
MemoryBudgetLedger::usableByteBudgetForPhysicalMemory(const std::size_t physicalMemory) noexcept {
    return physicalMemory == 0 ? kFallbackUsableMemoryBudget : physicalMemory;
}

MemoryBudgetLedger::MemoryBudgetLedger(const std::size_t physicalMemory,
                                       const std::size_t availableMemory) noexcept
    : physicalMemory_(physicalMemory), defaultsResolved_(physicalMemory != 0),
      usableByteBudget_(usableByteBudgetForPhysicalMemory(physicalMemory)) {
    // Automatic budgets are selected from hardware capacity, independent of how much other work
    // happens to be resident at startup. Only actual preferences change a resolved selection.
    auto total = usableByteBudget_;
    if (physicalMemory != 0)
        total = std::min(total, fraction(physicalMemory, kDefaultTotalPhysicalPercent, 100));
    const auto floor = physicalMemory == 0 ? kFallbackUsableMemoryBudget
                                           : std::min(physicalMemory, kFallbackUsableMemoryBudget);
    defaultTotalByteBudget_ = std::min(usableByteBudget_, std::max(total, floor));
    state_.configuredBytes = defaultTotalByteBudget_;
    state_.effectiveBytes = defaultTotalByteBudget_;
    state_.processingEffectiveBytes = usableByteBudget_;
    if (availableMemory != 0)
        state_.machine.availableBytes = availableMemory;
}

MemoryBudgetSample memoryBudgetSample(const CancellationToken& cancellation) noexcept {
    MemoryBudgetSample result;
    if (cancellation.isCancellationRequested())
        return result;
    result.physicalBytes = physicalMemoryBytes();
    result.machine = machineMemorySample();
    result.process = sampleProcessMemory(cancellation);
    return result;
}

MemoryBudgetAllocation
MemoryBudgetLedger::allocate(const std::optional<std::size_t> operationOverride,
                             const std::optional<std::size_t> previewOverride) const noexcept {
    const std::lock_guard lock(mutex_);
    const auto defaultOperation = fraction(defaultTotalByteBudget_, 3, 5);
    const auto defaultPreview =
        std::max(defaultTotalByteBudget_ - defaultOperation, kMinimumPreviewFrameCacheByteBudget);

    // Preferences name independent pools. A nonzero value is the selected budget, even when the
    // two selections exceed the automatic total or physical capacity. A real process constraint
    // can separately reduce reusable admission; it never rewrites this selection.
    const auto operation =
        operationOverride.value_or(0) != 0
            ? *operationOverride
            : std::min(defaultOperation,
                       defaultTotalByteBudget_ - std::min(defaultPreview, defaultTotalByteBudget_));
    const auto preview = previewOverride.value_or(0) != 0
                             ? *previewOverride
                             : std::min(defaultPreview, defaultTotalByteBudget_);

    return {.usableByteBudget = usableByteBudget_,
            .operationCacheByteBudget = operation,
            .previewFrameCacheByteBudget = preview};
}

void MemoryBudgetLedger::registerCache(const void* owner, const std::size_t ceiling,
                                       std::function<std::size_t()> retained,
                                       std::function<void(std::size_t)> applyBudget,
                                       const MemoryBudgetOwnerKind kind) {
    const std::lock_guard lock(mutex_);
    const auto entry = caches_
                           .insert_or_assign(owner, Cache{ceiling, kind, std::move(retained),
                                                          std::move(applyBudget)})
                           .first;
    // The new owner initialized its storage before registration and is not published yet.
    // A finite shared constraint must also update existing participants immediately.
    if (hasPolled_) {
        try {
            if (processSampling_)
                applyProcessCeilingLocked();
            if (state_.processCacheCeilingBytes)
                applyBudgetsLocked();
            else
                entry->second.applyBudget(cacheByteBudgetLocked(entry->second));
        } catch (...) {
            caches_.erase(entry);
            throw;
        }
    }
}

void MemoryBudgetLedger::unregisterCache(const void* owner) {
    const std::lock_guard lock(mutex_);
    caches_.erase(owner);
    if (hasPolled_ && state_.processCacheCeilingBytes) {
        applyProcessCeilingLocked();
        applyBudgetsLocked();
    }
}

std::size_t MemoryBudgetLedger::setCacheCeiling(const void* owner, const std::size_t ceiling) {
    const std::lock_guard lock(mutex_);
    const auto found = caches_.find(owner);
    if (found == caches_.end())
        return ceiling;
    found->second.ceiling = ceiling;
    if (processSampling_)
        applyProcessCeilingLocked();
    const auto bytes = hasPolled_ ? cacheByteBudgetLocked(found->second) : ceiling;
    // Apply while still serialized with polls; a caller must not publish an old allowance after
    // a concurrent pressure callback has already reduced it.
    if (state_.processCacheCeilingBytes)
        applyBudgetsLocked();
    else
        found->second.applyBudget(bytes);
    return bytes;
}

std::size_t MemoryBudgetLedger::cacheByteBudgetLocked(const Cache& participant) const {
    if (participant.kind == MemoryBudgetOwnerKind::LiveWork)
        return participant.ceiling;
    if (state_.awaitingProcessSample)
        return 0;
    if (ramHeadroom_ && state_.machine.availableBytes) {
        // Usable-capacity retention: what this pool holds plus the free memory beyond headroom.
        const auto available = *state_.machine.availableBytes;
        const auto free = available > *ramHeadroom_ ? available - *ramHeadroom_ : 0;
        auto bytes = saturatedAdd(participant.retained ? participant.retained() : 0, free);
        if (state_.processCacheCeilingBytes)
            bytes = std::min(bytes, *state_.processCacheCeilingBytes);
        return bytes;
    }
    if (!state_.processCacheCeilingBytes)
        return participant.ceiling;
    const auto absoluteLimit = *state_.processCacheCeilingBytes;
    long double total = 0;
    std::size_t boundedTotal = 0;
    bool exceedsLimit = false;
    for (const auto& [owner, cache] : caches_) {
        if (cache.kind != MemoryBudgetOwnerKind::ReusableRetention)
            continue;
        total += static_cast<long double>(cache.ceiling);
        if (cache.ceiling > absoluteLimit - boundedTotal)
            exceedsLimit = true;
        boundedTotal += std::min(cache.ceiling, absoluteLimit - boundedTotal);
    }
    if (!exceedsLimit)
        return participant.ceiling;
    const auto limit = static_cast<long double>(absoluteLimit);
    // This split exists only for an observed OS limit. Registered targets remain exact when no
    // finite constraint exists; auxiliary pools never rewrite the selected preferences.
    auto remaining = absoluteLimit;
    for (const auto& [owner, cache] : caches_) {
        if (cache.kind != MemoryBudgetOwnerKind::ReusableRetention)
            continue;
        const auto scaled = static_cast<long double>(cache.ceiling) * limit / total;
        // Clamp before conversion, including platforms whose long double cannot represent every
        // size_t. Subtract each assigned share so rounding can never exceed the actual OS limit.
        const auto bytes = scaled >= static_cast<long double>(remaining)
                               ? std::min(cache.ceiling, remaining)
                               : std::min(cache.ceiling, static_cast<std::size_t>(scaled));
        if (&cache == &participant)
            return bytes;
        remaining -= bytes;
    }
    return 0;
}

std::size_t MemoryBudgetLedger::cacheByteBudget(const void* owner) const {
    const std::lock_guard lock(mutex_);
    const auto found = caches_.find(owner);
    return found == caches_.end() ? 0 : cacheByteBudgetLocked(found->second);
}

void MemoryBudgetLedger::setConfiguredTotal(const std::size_t bytes) {
    const std::lock_guard lock(mutex_);
    configuredTotalSet_ = true;
    state_.configuredBytes = bytes;
    if (!processSampling_)
        state_.effectiveBytes = bytes;
    else
        applyProcessCeilingLocked();
    applyBudgetsLocked();
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

MemoryBudgetState MemoryBudgetLedger::pollHostLocked(MachineMemorySample sample,
                                                     const Clock::time_point now) {
    hasPolled_ = true;
    std::size_t retained = 0;
    for (const auto& [owner, cache] : caches_)
        retained = saturatedAdd(retained, cache.retained());
    state_.retainedBytes = retained;
    state_.memoryNotice = false;
    const bool wasSwap = state_.swapPressure;
    // Swap activity remains observable, but does not silently change selected cache budgets.
    const bool swapGrowing = observeSwapActivity(sample, now, wasSwap);
    if (sample.swapTotalBytes != 0)
        state_.swapPressure = swapGrowing;
    state_.swapNotice = state_.swapPressure && !wasSwap;
    state_.machine = sample;
    state_.retentionPercent = 100;
    if (!processSampling_) {
        state_.effectiveBytes = state_.configuredBytes;
        state_.memoryPressure = false;
        state_.memoryNotice = false;
    }
    return state_;
}

void MemoryBudgetLedger::setRamHeadroom(const std::size_t bytes) {
    const std::lock_guard lock(mutex_);
    if (ramHeadroom_ == bytes)
        return;
    ramHeadroom_ = bytes;
    applyBudgetsLocked();
}

void MemoryBudgetLedger::applyBudgetsLocked() {
    for (const auto& [owner, cache] : caches_)
        cache.applyBudget(cacheByteBudgetLocked(cache));
}

void MemoryBudgetLedger::beginProcessSampling() {
    const std::lock_guard lock(mutex_);
    if (processSampling_)
        return;
    processSampling_ = true;
    hasPolled_ = true;
    state_.awaitingProcessSample = true;
    state_.effectiveBytes = 0;
    applyBudgetsLocked();
}

void MemoryBudgetLedger::updatePhysicalCapacity(const std::size_t physical) {
    if (physical == 0)
        return;
    physicalMemory_ = physical;
    usableByteBudget_ = usableByteBudgetForPhysicalMemory(physical);
    state_.processingEffectiveBytes = usableByteBudget_;
    if (!defaultsResolved_) {
        const auto total = fraction(physical, kDefaultTotalPhysicalPercent, 100);
        defaultTotalByteBudget_ = std::max(total, std::min(physical, kFallbackUsableMemoryBudget));
        defaultsResolved_ = true;
        if (!configuredTotalSet_)
            state_.configuredBytes = defaultTotalByteBudget_;
    }
}

void MemoryBudgetLedger::applyProcessCeilingLocked() {
    const bool wasPressure = state_.memoryPressure;
    state_.effectiveBytes = state_.configuredBytes;
    if (state_.processCacheCeilingBytes)
        state_.effectiveBytes = std::min(state_.effectiveBytes, *state_.processCacheCeilingBytes);
    if (state_.awaitingProcessSample)
        state_.effectiveBytes = 0;
    state_.memoryPressure =
        !state_.awaitingProcessSample && state_.processCacheCeilingBytes &&
        (state_.configuredBytes > *state_.processCacheCeilingBytes ||
         (!ramHeadroom_ && std::any_of(caches_.begin(), caches_.end(), [this](const auto& entry) {
             const auto& cache = entry.second;
             return cache.kind == MemoryBudgetOwnerKind::ReusableRetention &&
                    cacheByteBudgetLocked(cache) < cache.ceiling;
         })));
    state_.memoryNotice = state_.memoryPressure && !wasPressure;
}

MemoryBudgetState MemoryBudgetLedger::poll(MachineMemorySample sample,
                                           const Clock::time_point now) {
    const std::lock_guard lock(mutex_);
    static_cast<void>(pollHostLocked(sample, now));
    applyBudgetsLocked();
    return state_;
}

MemoryBudgetState MemoryBudgetLedger::applySample(MemoryBudgetSample sample,
                                                  const Clock::time_point now) {
    const std::lock_guard lock(mutex_);
    updatePhysicalCapacity(sample.physicalBytes);
    if (!processSampling_) {
        static_cast<void>(pollHostLocked(sample.machine, now));
        applyBudgetsLocked();
        return state_;
    }
    const auto stamp = sample.process.sampledAt;
    // Only monotonic deliveries can authorize recovery; a superseded sample can still tighten.
    const auto ageSeconds = std::chrono::duration<long double>(now.time_since_epoch()).count() -
                            std::chrono::duration<long double>(stamp.time_since_epoch()).count();
    const bool fresh =
        ageSeconds >= 0 && ageSeconds <= 15 && (!lastProcessSample_ || stamp > *lastProcessSample_);
    state_.processSampleFresh = fresh;
    // Retained targets use the actual finite domain limit, not remaining headroom. A headroom
    // target would shrink as caches fill; retained gauges are not unique host ownership and must
    // never be added back as reclaimable credit. Live phase admission checks fresh usage
    // separately.
    std::optional<std::size_t> measured;
    for (const auto& domain : sample.process.domains) {
        if (domain.kind == ProcessMemoryLimitKind::Finite)
            measured = measured ? std::min(*measured, domain.limitBytes) : domain.limitBytes;
    }
    const bool complete = fresh && (isCompleteProcessMemorySample(sample.process) ||
                                    sample.process.status == ProcessMemoryStatus::Unsupported);
    processConstraintHistory_.recordFinite(
        sample.process, complete && sample.process.status == ProcessMemoryStatus::Finite);
    // Remove only constraints explicitly observed as unlimited in the same verified view.
    if (complete && processConstraintHistory_.verifiesRemoval(sample.process))
        state_.processCacheCeilingBytes.reset();
    if (measured) {
        if (!state_.processCacheCeilingBytes || *measured < *state_.processCacheCeilingBytes ||
            (complete && sample.process.status == ProcessMemoryStatus::Finite))
            state_.processCacheCeilingBytes = measured;
    }
    if (complete)
        state_.awaitingProcessSample = false;
    if (fresh)
        lastProcessSample_ = stamp;
    state_.process = std::move(sample.process);
    if (state_.sampleSerial != std::numeric_limits<std::uint64_t>::max())
        ++state_.sampleSerial;
    static_cast<void>(pollHostLocked(sample.machine, now));
    applyProcessCeilingLocked();
    applyBudgetsLocked();
    return state_;
}

MemoryBudgetState MemoryBudgetLedger::state() const {
    const std::lock_guard lock(mutex_);
    return state_;
}

MemoryBudgetLedger& processMemoryBudgetLedger() {
    static MemoryBudgetLedger ledger;
    return ledger;
}

std::size_t defaultOperationCacheByteBudget() noexcept {
    return processMemoryBudgetLedger().allocate().operationCacheByteBudget;
}

} // namespace bloom::runtime
