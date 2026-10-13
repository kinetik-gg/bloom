#pragma once

#include <array>
#include <cstdint>

namespace bloom::render {
enum class GpuDriverMemoryBudgetStatus : std::uint8_t { Unknown, Available };
struct GpuDeviceLocalHeapMemory final {
    std::uint32_t heapIndex = 0;
    std::uint64_t usageBytes = 0;
    std::uint64_t budgetBytes = 0;
};
// Owner-thread snapshot, copied to consumers by the owning service. Allocation bytes are live VMA
// payload; block bytes include allocator-held free space. Neither is process RSS. Driver heap
// usage/budget is separate, only available with the enabled live-budget extension, and may include
// allocations outside VMA. Never add these overlapping values or label residual usage Bloom-owned.
struct GpuAllocatorMemorySnapshot final {
    bool available = false;
    std::uint64_t allocationBytes = 0;
    std::uint64_t allocationCount = 0;
    std::uint64_t blockBytes = 0;
    std::uint64_t blockCount = 0;
    GpuDriverMemoryBudgetStatus driverBudgetStatus = GpuDriverMemoryBudgetStatus::Unknown;
    // Bounded backend-neutral storage; the current backend exposes at most 16 physical heaps.
    std::array<GpuDeviceLocalHeapMemory, 16> deviceLocalHeaps{};
    std::uint32_t deviceLocalHeapCount = 0;
};
} // namespace bloom::render
