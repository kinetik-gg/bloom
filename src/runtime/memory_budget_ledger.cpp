#include <bloom/runtime/memory_budget_ledger.hpp>

#include <algorithm>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace bloom::runtime {
namespace {

constexpr std::size_t kMinimumReserve = std::size_t{4} * 1024U * 1024U * 1024U;

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

std::size_t
MemoryBudgetLedger::usableByteBudgetForPhysicalMemory(const std::size_t physicalMemory) noexcept {
    if (physicalMemory == 0)
        return kFallbackUsableMemoryBudget;

    const auto reserve = std::max(kMinimumReserve, physicalMemory / 4);
    const auto afterReserve = physicalMemory > reserve ? physicalMemory - reserve : 0;
    // Keep the historic low-memory floor useful when the reserve would otherwise leave no room for
    // either cache. Never invent more budget than the machine reports in this fallback branch.
    const auto lowMemoryFallback = std::min(physicalMemory, kFallbackUsableMemoryBudget);
    return std::max(afterReserve, lowMemoryFallback);
}

MemoryBudgetLedger::MemoryBudgetLedger(const std::size_t physicalMemory) noexcept
    : usableByteBudget_(usableByteBudgetForPhysicalMemory(physicalMemory)) {}

MemoryBudgetAllocation
MemoryBudgetLedger::allocate(const std::optional<std::size_t> operationOverride,
                             const std::optional<std::size_t> previewOverride) const noexcept {
    const auto defaultOperation = fraction(usableByteBudget_, 3, 5);
    const auto defaultPreview =
        std::max(usableByteBudget_ - defaultOperation, kMinimumPreviewFrameCacheByteBudget);

    std::size_t operation = defaultOperation;
    std::size_t preview = defaultPreview;
    if (!operationOverride.has_value() && !previewOverride.has_value()) {
        if (exceeds(operation, preview, usableByteBudget_))
            operation = usableByteBudget_ - std::min(preview, usableByteBudget_);
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
