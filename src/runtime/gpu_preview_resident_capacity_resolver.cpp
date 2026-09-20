// Owner-thread bridge between the render device allocation budget and the resident admission
// policy. Kept in its own translation unit so the service startup and resident-route files stay
// focused. Nothing here allocates, blocks, or touches a device off its owner thread.

#include "gpu_preview_display_service_private.hpp"

#include <bloom/runtime/gpu_preview_resident_capacity.hpp>

#include <algorithm>
#include <cstdint>

namespace bloom::runtime::detail {

void resolveResidentCapacity(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core == nullptr || core->gpuStageFunction == nullptr || core->device == nullptr) {
        return;
    }
    if (!core->device->isOwnerThread()) {
        return;
    }

    GpuResidentCapacity capacity;
    try {
        capacity =
            gpuResidentCapacityFromAllocationBudget(core->device->availableAllocationBudget());
    } catch (...) {
        capacity = GpuResidentCapacity{};
    }

    GpuResidentConfiguredBudgets configured;
    configured.leaseBytes = core->options.residentLeaseBudgets.maxBytes;
    configured.sceneCacheBytes = core->options.residentSceneCacheBudgets.maxRetainedBytes;
    configured.requestBytes = core->options.previewByteAllowance;
    const GpuResidentCapacityPlan plan = gpuResidentCapacityPlanFor(configured, capacity);

    // Effective sub-budgets are never above the configured values. They are what the owner actually
    // creates the lease registry, scene cache, and per-request admission with.
    core->effectiveResidentLeaseBudgets.maxBytes = plan.leaseBytes;
    const std::uint64_t configuredEntries =
        static_cast<std::uint64_t>(core->options.residentLeaseBudgets.maxEntries);
    core->effectiveResidentLeaseBudgets.maxEntries =
        static_cast<std::size_t>(std::min(configuredEntries, plan.leaseEntries));
    core->effectiveResidentSceneCacheBudgets.maxRetainedBytes = plan.sceneCacheBytes;
    core->effectivePreviewByteAllowance.store(plan.requestBytes, std::memory_order_relaxed);

    {
        std::lock_guard lock(core->stateMutex);
        core->publishedResidentCapacityPlan = plan;
    }
    core->notify();
}

} // namespace bloom::runtime::detail
