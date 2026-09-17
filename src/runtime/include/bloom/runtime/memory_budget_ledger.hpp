#pragma once

#include <cstddef>
#include <optional>

namespace bloom::runtime {

inline constexpr std::size_t kDefaultOperationCacheBytes = std::size_t{1} * 1024U * 1024U * 1024U;
inline constexpr std::size_t kMinimumPreviewFrameCacheByteBudget =
    std::size_t{2} * 1024U * 1024U * 1024U;
inline constexpr std::size_t kFallbackUsableMemoryBudget =
    kMinimumPreviewFrameCacheByteBudget + kDefaultOperationCacheBytes;

// The ledger is deliberately a budget calculator, not a memory owner. The application composition
// root resolves these numbers once and gives each cache its effective allowance.
struct MemoryBudgetAllocation final {
    std::size_t usableByteBudget = 0;
    std::size_t operationCacheByteBudget = 0;
    std::size_t previewFrameCacheByteBudget = 0;

    friend bool operator==(const MemoryBudgetAllocation&, const MemoryBudgetAllocation&) = default;
};

[[nodiscard]] std::size_t physicalMemoryBytes() noexcept;

class MemoryBudgetLedger final {
  public:
    // Supplying physical memory makes the policy deterministic for tests. The default reads the
    // host once, at construction, so callers do not observe a moving machine-sized budget.
    explicit MemoryBudgetLedger(std::size_t physicalMemory = physicalMemoryBytes()) noexcept;

    [[nodiscard]] std::size_t usableByteBudget() const noexcept { return usableByteBudget_; }

    // Missing overrides use the 60/40 operation/preview split. A single override keeps its exact
    // value where possible and reduces the other allocation. If both overrides exceed the ledger,
    // they are proportionally reduced so neither setting is silently discarded.
    [[nodiscard]] MemoryBudgetAllocation
    allocate(std::optional<std::size_t> operationOverride = {},
             std::optional<std::size_t> previewOverride = {}) const noexcept;

    [[nodiscard]] static std::size_t
    usableByteBudgetForPhysicalMemory(std::size_t physicalMemory) noexcept;

  private:
    std::size_t usableByteBudget_ = 0;
};

[[nodiscard]] std::size_t defaultOperationCacheByteBudget() noexcept;

} // namespace bloom::runtime
