#pragma once

#include <bloom/render/gpu_processing_phase_budget.hpp>

#include <condition_variable>
#include <limits>
#include <mutex>

namespace bloom::render::detail {
struct GpuProcessingPhaseBudgetState final {
    mutable std::mutex mutex;
    // Signalled after phase releases and capacity changes (outside `mutex`).
    mutable std::condition_variable changed;
    GpuProcessingPhaseSnapshot counters;
    RootedHostBackingSnapshot rootedHostBacking;
    // The current host-capacity sample may include this much rooted backing credit. It is
    // revoked before an eligible allocation begins freeing, even if malloc retains its pages.
    std::uint64_t hostBackingCreditRemaining = 0;
    std::uint64_t nextId = 1;
    std::shared_ptr<PackedHostRetirementState> retirement;
    std::chrono::steady_clock::time_point hostValidUntil =
        std::chrono::steady_clock::time_point::max();
};
[[nodiscard]] inline std::uint64_t remaining(const std::uint64_t cap,
                                             const std::uint64_t reserved) noexcept {
    return cap > reserved ? cap - reserved : 0;
}
inline void incrementSaturating(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

struct PresentationAdmission final {
    GpuProcessingPhaseAdmission status;
    GpuProcessingPhaseDomain domain;
    std::uint64_t revision;
};
// Independent, exact-root allocation admission. This is a native backing claim, never a phase
// count slot or a conversion of another frame's working/source/output allowance.
[[nodiscard]] inline PresentationAdmission
admitPresentation(GpuProcessingPhaseBudgetState& state, const std::uint64_t bytes, const bool local,
                  const bool image, const std::chrono::steady_clock::time_point now) noexcept {
    const std::lock_guard lock(state.mutex);
    auto& counters = state.counters;
    const auto refuse = [&](GpuProcessingPhaseAdmission status, GpuProcessingPhaseDomain domain) {
        if (status == GpuProcessingPhaseAdmission::BudgetRefused)
            incrementSaturating(counters.budgetRefusals);
        return PresentationAdmission{status, domain, counters.admissionRevision};
    };
    if (counters.hostReady && now > state.hostValidUntil) {
        counters.hostReady = false;
        incrementSaturating(counters.admissionRevision);
    }
    if (!counters.hostReady || !counters.hostCapacityBytes || now > state.hostValidUntil)
        return refuse(GpuProcessingPhaseAdmission::AwaitingCapacity,
                      GpuProcessingPhaseDomain::Host);
    if (!counters.deviceReady || !counters.deviceCapacityBytes)
        return refuse(GpuProcessingPhaseAdmission::AwaitingCapacity,
                      GpuProcessingPhaseDomain::Device);
    if (bytes > remaining(*counters.deviceCapacityBytes, counters.totalDeviceClaimBytes))
        return refuse(GpuProcessingPhaseAdmission::BudgetRefused, GpuProcessingPhaseDomain::Device);
    if (!local && bytes > remaining(*counters.hostCapacityBytes, counters.totalHostClaimBytes))
        return refuse(GpuProcessingPhaseAdmission::BudgetRefused, GpuProcessingPhaseDomain::Host);
    counters.totalDeviceClaimBytes += bytes;
    if (image) {
        counters.ownedNativeImageBytes += bytes;
        counters.ownedPresentationImageBytes += bytes;
        ++counters.ownedNativeImages;
        ++counters.ownedPresentationImages;
    } else {
        counters.ownedNativeBufferBytes += bytes;
        counters.ownedPresentationBufferBytes += bytes;
        ++counters.ownedNativeBuffers;
        ++counters.ownedPresentationBuffers;
    }
    if (!local) {
        counters.totalHostClaimBytes += bytes;
        if (image)
            counters.ownedNonlocalNativeHostBytes += bytes;
        else
            counters.ownedNonlocalNativeBufferHostBytes += bytes;
    }
    return {GpuProcessingPhaseAdmission::Accepted, GpuProcessingPhaseDomain::None,
            counters.admissionRevision};
}
} // namespace bloom::render::detail
