#include <bloom/runtime/memory_budget_ledger.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
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

#if !defined(_WIN32)
// MemAvailable is the kernel's own estimate of what can be allocated without swapping -- the
// reclaimable page cache included -- which is exactly the question a cache budget asks. MemFree is
// not: a machine with a warm page cache reports almost no free memory while most of it is
// reclaimable.
[[nodiscard]] std::size_t memAvailableFromProcMeminfo() noexcept {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo)
        return 0;
    std::string line;
    while (std::getline(meminfo, line)) {
        constexpr std::string_view label = "MemAvailable:";
        auto rest = std::string_view(line);
        if (!rest.starts_with(label))
            continue;
        rest.remove_prefix(label.size());
        while (!rest.empty() && rest.front() == ' ')
            rest.remove_prefix(1);
        std::uint64_t kibibytes = 0;
        const auto parsed = std::from_chars(rest.data(), rest.data() + rest.size(), kibibytes);
        if (parsed.ec != std::errc{} || kibibytes == 0)
            return 0;
        if (kibibytes > std::numeric_limits<std::size_t>::max() / 1024U)
            return std::numeric_limits<std::size_t>::max();
        return static_cast<std::size_t>(kibibytes) * 1024U;
    }
    return 0;
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

std::size_t availableMemoryBytes() noexcept {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0)
        return 0;
    return static_cast<std::size_t>(status.ullAvailPhys);
#else
    if (const auto available = memAvailableFromProcMeminfo(); available != 0)
        return available;
    // Every platform without /proc still answers this, and it is the closest standard equivalent:
    // free pages, without the reclaimable page cache MemAvailable would have counted. Reading low
    // here only makes the default budget more conservative, never less.
    const auto pages = sysconf(_SC_AVPHYS_PAGES);
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
    : usableByteBudget_(usableByteBudgetForPhysicalMemory(physicalMemory)),
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
            const auto total = static_cast<long double>(operation) + preview;
            operation = static_cast<std::size_t>(static_cast<long double>(operation) *
                                                 usableByteBudget_ / total);
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

std::size_t defaultOperationCacheByteBudget() noexcept {
    return MemoryBudgetLedger{}.allocate().operationCacheByteBudget;
}

} // namespace bloom::runtime
