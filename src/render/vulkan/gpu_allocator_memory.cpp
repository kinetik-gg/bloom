#include "gpu_device_private.hpp"

#include <bloom/render/gpu_allocator_memory.hpp>

namespace bloom::render {
GpuAllocatorMemorySnapshot GpuDevice::allocatorMemorySnapshot() const noexcept {
    GpuAllocatorMemorySnapshot result;
    if (!isOwnerThread() || !impl_->control || impl_->control->allocator == VK_NULL_HANDLE)
        return result;
    const auto& control = *impl_->control;
    VmaTotalStatistics statistics{};
    vmaCalculateStatistics(control.allocator, &statistics);
    result.available = true;
    result.allocationBytes = statistics.total.statistics.allocationBytes;
    result.allocationCount = statistics.total.statistics.allocationCount;
    result.blockBytes = statistics.total.statistics.blockBytes;
    result.blockCount = statistics.total.statistics.blockCount;
    // Without the actually enabled extension VMA supplies estimates. Keep those Unknown instead
    // of presenting nominal physical heap sizes or allocator bytes as live driver evidence.
    if (!control.memoryBudgetEnabled)
        return result;
    const auto* api = control.instance.getDispatcher();
    if (!api->vkGetPhysicalDeviceMemoryProperties2)
        return result;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budgets{};
    budgets.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    properties.pNext = &budgets;
    // Query the driver now instead of relabelling VMA's cached, allocation-adjusted estimates as
    // fresh driver evidence. The service calls this diagnostic at a bounded rate on its owner.
    api->vkGetPhysicalDeviceMemoryProperties2(
        static_cast<VkPhysicalDevice>(*control.physicalDevice), &properties);
    result.driverBudgetStatus = GpuDriverMemoryBudgetStatus::Available;
    for (std::uint32_t heap = 0;
         heap < properties.memoryProperties.memoryHeapCount && heap < VK_MAX_MEMORY_HEAPS &&
         result.deviceLocalHeapCount < result.deviceLocalHeaps.size();
         ++heap) {
        if ((properties.memoryProperties.memoryHeaps[heap].flags &
             VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            result.deviceLocalHeaps[result.deviceLocalHeapCount++] = {heap, budgets.heapUsage[heap],
                                                                      budgets.heapBudget[heap]};
        }
    }
    return result;
}
} // namespace bloom::render
