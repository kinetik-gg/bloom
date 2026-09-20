#include <bloom/runtime/gpu_preview_resident_capacity.hpp>

#include <algorithm>
#include <limits>

namespace bloom::runtime {

namespace gpu_resident_capacity_detail {

std::uint64_t scaledShareClamped(const std::uint64_t value, const std::uint64_t numerator,
                                 const std::uint64_t denominator) noexcept {
    if (denominator == 0U || value == 0U || numerator == 0U) {
        return 0U;
    }
    // The result can never legitimately exceed min(value, numerator); clamp to that bound so a
    // rounded-up floating estimate can never break a caller's no-overcommit invariant.
    const std::uint64_t bound = std::min(value, numerator);
    const long double estimate =
        (static_cast<long double>(value) * static_cast<long double>(numerator)) /
        static_cast<long double>(denominator);
    if (!(estimate > 0.0L)) {
        return 0U;
    }
    if (estimate >= static_cast<long double>(bound)) {
        return bound;
    }
    return std::min(static_cast<std::uint64_t>(estimate), bound);
}

} // namespace gpu_resident_capacity_detail

namespace {

std::uint64_t saturatingAdd(const std::uint64_t left, const std::uint64_t right) noexcept {
    const std::uint64_t sum = left + right;
    return sum < left ? std::numeric_limits<std::uint64_t>::max() : sum;
}

// Overflow-safe value * numerator / denominator for numerator <= denominator. The exact 128-bit
// path floors on the compilers that provide it; the portable fallback is clamped to
// [0, min(value, numerator)], so the sequential pool accounting can never underflow or overcommit
// on any toolchain. The result is an admission bound, never larger than min(value, numerator).
std::uint64_t scaledShare(const std::uint64_t value, const std::uint64_t numerator,
                          const std::uint64_t denominator) noexcept {
    if (denominator == 0U || value == 0U || numerator == 0U) {
        return 0U;
    }
    const std::uint64_t bound = std::min(value, numerator);
    if (numerator >= denominator) {
        return bound;
    }
#if defined(__SIZEOF_INT128__)
    const auto exact = static_cast<std::uint64_t>(
        (static_cast<unsigned __int128>(value) * numerator) / denominator);
    return std::min(exact, bound);
#else
    return gpu_resident_capacity_detail::scaledShareClamped(value, numerator, denominator);
#endif
}

void finalizeCacheAndEntries(GpuResidentCapacityPlan& plan) noexcept {
    plan.cacheBytes = plan.leaseBytes - plan.leaseBytes / kResidentCacheHeadroomDenominator;
    if (plan.cacheBytes == 0U) {
        return;
    }
    const std::uint64_t rawEntries = plan.cacheBytes / kNominalResidentFrameBytes;
    plan.cacheEntries =
        std::clamp(rawEntries, kResidentCacheEntryFloor, kResidentCacheEntryCeiling);
    const std::uint64_t entryHeadroom =
        std::max<std::uint64_t>(plan.cacheEntries / 8U, kResidentCacheEntryFloor);
    plan.leaseEntries =
        std::min<std::uint64_t>(plan.cacheEntries + entryHeadroom, kResidentLeaseEntryCeiling);
}

} // namespace

GpuResidentCapacity
gpuResidentCapacityFromAllocationBudget(const render::GpuAllocationBudget& allocation) noexcept {
    GpuResidentCapacity capacity;
    capacity.deviceLocalBytes = allocation.device_local_bytes;
    switch (allocation.source) {
    case render::GpuAllocationBudgetSource::MemoryBudget:
        capacity.source = GpuResidentCapacitySource::MemoryBudget;
        capacity.availableBytes = allocation.available_bytes;
        break;
    case render::GpuAllocationBudgetSource::DeviceLocalEstimate:
        capacity.source = GpuResidentCapacitySource::DeviceLocalEstimate;
        capacity.availableBytes = allocation.available_bytes;
        break;
    case render::GpuAllocationBudgetSource::Unavailable:
        capacity.source = GpuResidentCapacitySource::Unknown;
        capacity.availableBytes = 0;
        break;
    }
    return capacity;
}

GpuResidentCapacityPlan
gpuResidentCapacityPlanConfigured(const std::uint64_t configuredCeilingBytes) noexcept {
    GpuResidentCapacityPlan plan;
    plan.poolBytes = configuredCeilingBytes;
    plan.leaseBytes =
        (configuredCeilingBytes / kResidentLeaseDenominator) * kResidentLeaseNumerator;
    plan.sceneCacheBytes = configuredCeilingBytes / kResidentSceneDenominator;
    plan.requestBytes = configuredCeilingBytes - plan.leaseBytes - plan.sceneCacheBytes;
    finalizeCacheAndEntries(plan);
    return plan;
}
std::uint64_t gpuResidentRequestBudget(const std::uint64_t hostByteAllowance,
                                       const std::uint64_t gpuRequestCeiling) noexcept {
    if (hostByteAllowance == 0U || gpuRequestCeiling == 0U) {
        return 0U;
    }
    return std::min(hostByteAllowance, gpuRequestCeiling);
}

GpuResidentCapacityPlan gpuResidentCapacityPlanFor(const GpuResidentConfiguredBudgets& configured,
                                                   const GpuResidentCapacity& capacity) noexcept {
    GpuResidentCapacityPlan plan;
    // This is the owner-resolved path: even an Unknown device produces a valid conservative
    // fallback plan that a consumer may apply, so mark it resolved unconditionally.
    plan.resolved = true;
    plan.capacity = capacity;
    const std::uint64_t total = saturatingAdd(
        saturatingAdd(configured.leaseBytes, configured.sceneCacheBytes), configured.requestBytes);
    if (total == 0U) {
        return plan;
    }
    const std::uint64_t deviceLimit = capacity.isKnown()
                                          ? capacity.availableBytes / kResidentCapacityClaimDivisor
                                          : kUnknownResidentPoolBytes;
    if (total <= deviceLimit) {
        // The configured route already fits the resolved device pool: preserve it exactly.
        plan.poolBytes = total;
        plan.leaseBytes = configured.leaseBytes;
        plan.sceneCacheBytes = configured.sceneCacheBytes;
        plan.requestBytes = configured.requestBytes;
    } else {
        // Capacity pressure: share the pool by configured proportion, draining remaining pool and
        // weight after each ledger. The sum can never exceed the pool -- even when the configured
        // total saturated, each share is bounded by what is left. This relies only on the share
        // never exceeding min(weight, remaining pool), which holds by construction on every
        // toolchain; it does not depend on floating-point rounding direction.
        plan.poolBytes = deviceLimit;
        std::uint64_t remainingPool = deviceLimit;
        std::uint64_t remainingTotal = total;
        const auto take = [&remainingPool, &remainingTotal](const std::uint64_t weight) noexcept {
            if (remainingPool == 0U || remainingTotal == 0U) {
                return std::uint64_t{0};
            }
            const std::uint64_t bounded = std::min(weight, remainingTotal);
            const std::uint64_t share = scaledShare(bounded, remainingPool, remainingTotal);
            remainingPool -= share;
            remainingTotal -= bounded;
            return share;
        };
        plan.leaseBytes = take(configured.leaseBytes);
        plan.sceneCacheBytes = take(configured.sceneCacheBytes);
        plan.requestBytes = take(configured.requestBytes);
    }
    finalizeCacheAndEntries(plan);
    return plan;
}

} // namespace bloom::runtime
