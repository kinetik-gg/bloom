#include <bloom/render/processing_host_storage.hpp>

#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace bloom::render;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
std::shared_ptr<GpuProcessingPhaseLease> phase(GpuProcessingPhaseBudget& budget) {
    auto result = budget.reserve({.fixedHostBytes = 80, .sourceFree = true, .native = false});
    if (!result.lease)
        throw std::runtime_error("expected admitted host phase");
    return std::move(result.lease);
}
void allocatorBacking() {
    GpuProcessingPhaseBudget budget(1);
    budget.setHostCapacity(1000, true);
    auto origin = phase(budget);
    const auto revision = budget.snapshot().admissionRevision;
    std::optional<ProcessingHostStorageRefusal> refusal;
    using Allocator = ProcessingHostAllocator<float>;
    {
        std::vector<float, Allocator> scratch(Allocator(origin, &refusal));
        scratch.resize(100, 0.5F);
        const auto bytes = scratch.capacity() * sizeof(float);
        const auto snapshot = budget.snapshot();
        require(
            snapshot.ownedProcessingHostBytes == bytes &&
                snapshot.ownedProcessingHostMetadataBytes == Allocator::allocationHeaderBytes() &&
                snapshot.ownedProcessingHostAllocations == 1 &&
                snapshot.totalHostClaimBytes == 80 + bytes + Allocator::allocationHeaderBytes() &&
                snapshot.hostReservedBytes == 80 && !refusal,
            "real vector allocator request charges exact payload/header before allocation");
        bool refused = false;
        try {
            scratch.reserve(1000);
        } catch (const std::bad_alloc&) {
            refused = true;
        }
        require(refused && refusal &&
                    refusal->status == GpuProcessingPhaseAdmission::BudgetRefused &&
                    refusal->domain == GpuProcessingPhaseDomain::Host && scratch.size() == 100 &&
                    scratch.front() == 0.5F && budget.snapshot().ownedProcessingHostBytes == bytes,
                "refused growth preserves readable prior backing and exact typed host provenance");
        auto moved = std::move(scratch);
        require(moved.size() == 100 && budget.snapshot().ownedProcessingHostAllocations == 1,
                "container move preserves one backing charge");
        origin.reset();
        require(budget.snapshot().admittedPhases == 1,
                "scratch retains its origin through actual backing lifetime");
    }
    const auto empty = budget.snapshot();
    require(empty.ownedProcessingHostBytes == 0 && empty.ownedProcessingHostMetadataBytes == 0 &&
                empty.ownedProcessingHostAllocations == 0 && empty.totalHostClaimBytes == 0 &&
                empty.admittedPhases == 0 && empty.admissionRevision == revision + 1,
            "real backing free releases payload/header and final phase once outside lock");
}
void freshnessAndLifetime() {
    GpuProcessingPhaseBudget budget;
    const auto now = std::chrono::steady_clock::now();
    budget.setHostCapacity(1000, true, now);
    auto origin = phase(budget);
    auto stale = ProcessingHostStorageToken::reserve(origin, 1, 0, now + std::chrono::seconds(1));
    require(!stale.token && stale.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                budget.snapshot().ownedProcessingHostAllocations == 0,
            "stale additive host headroom refuses before backing");
    budget.setHostCapacity(1000, true);
    auto owned = ProcessingHostStorageToken::reserve(origin, 400, 64);
    require(owned.token.has_value(), "exact payload/header authority admitted");
    origin->markUnproven();
    require(!ProcessingHostStorageToken::reserve(origin, 1).token,
            "unproven authority cannot allocate scratch");
    origin.reset();
    const auto before = budget.snapshot();
    require(before.totalHostClaimBytes == 544 && before.unprovenPhases == 1,
            "live scratch retains unproven phase and complete exact claim");
    std::thread drop([held = std::move(owned.token)]() mutable { held.reset(); });
    drop.join();
    require(budget.snapshot().totalHostClaimBytes == 0 && budget.snapshot().unprovenPhases == 0,
            "foreign neutral disposal releases after leaving the ledger mutex");
    budget.setHostCapacity(std::numeric_limits<std::uint64_t>::max(), true);
    origin = phase(budget);
    require(!ProcessingHostStorageToken::reserve(origin, UINT64_MAX, 1).token &&
                !ProcessingHostStorageToken::reserve(origin, UINT64_MAX).token,
            "header arithmetic and remaining host claim reject overflow atomically");
    origin.reset();
    std::vector<float, ProcessingHostAllocator<float>> legacy;
    legacy.resize(16, 1.0F);
    require(budget.snapshot().ownedProcessingHostAllocations == 0,
            "explicit null-phase compatibility remains visibly unrooted");
}
struct ObservedBacking final {
    GpuProcessingPhaseBudget* budget = nullptr;
    bool* sawRetirementBeforeDelete = nullptr;
    std::uint64_t expectedCapacity = 0;
    std::uint64_t expectedOwnedBytes = 0;
    ~ObservedBacking() noexcept {
        if (!budget || !sawRetirementBeforeDelete)
            return;
        const auto snapshot = budget->snapshot();
        *sawRetirementBeforeDelete =
            snapshot.hostCapacityBytes == expectedCapacity &&
            snapshot.totalHostClaimBytes >= sizeof(ObservedBacking) &&
            snapshot.ownedProcessingHostBytes == expectedOwnedBytes &&
            budget->rootedHostBackingSnapshot().materializedBytes == 0;
    }
};
void adoptedBackingRetiresBeforeDelete() {
    constexpr auto bytes = sizeof(ObservedBacking);
    GpuProcessingPhaseBudget budget(2);
    budget.setHostCapacity(4 * bytes, true);
    auto result = budget.reserve({.fixedHostBytes = bytes, .sourceFree = true, .native = false});
    require(result.lease != nullptr, "fixed host phase admits the future real allocation");
    bool retiredBeforeDelete = false;
    auto backing = std::make_unique<ObservedBacking>();
    backing->budget = &budget;
    backing->sawRetirementBeforeDelete = &retiredBeforeDelete;
    backing->expectedCapacity = bytes;
    backing->expectedOwnedBytes = bytes;
    auto retained = adoptProcessingHostBacking(std::move(backing), result.lease, bytes, bytes);
    require(retained && budget.rootedHostBackingSnapshot().materializedBytes == bytes,
            "only completed unique backing becomes exact rooted sample credit");
    const auto owned = budget.snapshot();
    require(owned.hostReservedBytes == 0 && owned.ownedProcessingHostBytes == bytes &&
                owned.totalHostClaimBytes == bytes,
            "ownership transfer does not add a second host claim");
    const auto retirement = budget.rootedHostBackingSnapshot();
    budget.setSampledHostCapacity(2 * bytes, true, std::chrono::steady_clock::time_point::max(),
                                  bytes, retirement.retiredMaterializedBytes);
    result.lease.reset();
    std::thread destroy([held = std::move(retained)]() mutable { held.reset(); });
    destroy.join();
    const auto released = budget.snapshot();
    require(retiredBeforeDelete && released.hostCapacityBytes == bytes &&
                released.totalHostClaimBytes == 0 && released.ownedProcessingHostBytes == 0 &&
                released.admittedPhases == 0 &&
                budget.rootedHostBackingSnapshot().materializedBytes == 0,
            "foreign-thread deleter revokes credit before delete and releases claim afterward");
}
void failedAdoptionCannotCreateCredit() {
    constexpr auto bytes = sizeof(ObservedBacking);
    GpuProcessingPhaseBudget budget(1);
    budget.setHostCapacity(4 * bytes, true);
    auto result = budget.reserve({.fixedHostBytes = bytes, .sourceFree = true, .native = false});
    require(result.lease != nullptr, "fixed host promise admits");
    bool deletedWhileClaimHeld = false;
    auto oversized = std::make_unique<ObservedBacking>();
    oversized->budget = &budget;
    oversized->sawRetirementBeforeDelete = &deletedWhileClaimHeld;
    oversized->expectedCapacity = 4 * bytes;
    require(!adoptProcessingHostBacking(std::move(oversized), result.lease, bytes + 1, bytes),
            "actual backing beyond fixed phase claim refuses transfer");
    require(deletedWhileClaimHeld &&
                budget.rootedHostBackingSnapshot().materializedBytes == 0 &&
                budget.snapshot().totalHostClaimBytes == bytes,
            "failed adoption destroys backing before phase can release its original claim");
    require(!adoptProcessingHostBacking(std::make_unique<ObservedBacking>(), result.lease, 1, 1),
            "adoption cannot claim less than the owned object itself");
    require(!adoptProcessingHostBacking(std::make_unique<ObservedBacking>(), result.lease,
                                        bytes, bytes + 1),
            "initialized bytes cannot exceed the allocation claim");
    result.lease->markUnproven();
    require(!adoptProcessingHostBacking(std::make_unique<ObservedBacking>(), result.lease,
                                        bytes, bytes),
            "unproven phase cannot turn a completed backing into sampled credit");
    std::unique_ptr<ObservedBacking> cancelled = std::make_unique<ObservedBacking>();
    cancelled.reset();
    require(!adoptProcessingHostBacking(std::move(cancelled), result.lease, bytes, bytes) &&
                budget.rootedHostBackingSnapshot().materializedBytes == 0,
            "cancelled backing cannot become a materialized claim");
    result.lease.reset();
    require(budget.snapshot().totalHostClaimBytes == 0,
            "failure and cancellation leave no owned or held allocation");
}
} // namespace
int main() {
    try {
        allocatorBacking();
        freshnessAndLifetime();
        adoptedBackingRetiresBeforeDelete();
        failedAdoptionCannotCreateCredit();
        std::cout << "PASS: transient processing host allocator ownership\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
