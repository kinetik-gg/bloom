#include <bloom/render/gpu_processing_phase_budget.hpp>
#include <bloom/render/native_buffer_storage.hpp>

#include <array>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace bloom::render;
void require(const bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
GpuProcessingPhaseRequest request() {
    return {.fixedHostBytes = 10,
            .requestedSourceBytes = 20,
            .minimumSourceBytes = 20,
            .deviceWorkingBytes = 10,
            .requestedExecutorBytes = 20,
            .minimumExecutorBytes = 20};
}
void readinessAndRollback() {
    GpuProcessingPhaseBudget budget;
    require(budget.reserve(request()).status == GpuProcessingPhaseAdmission::AwaitingCapacity,
            "unknown host must await capacity");
    budget.setHostCapacity(70, true);
    require(budget.reserve(request()).domain == GpuProcessingPhaseDomain::Device,
            "native request must await device independently");
    budget.setDeviceCapacity(29, true);
    require(budget.reserve(request()).status == GpuProcessingPhaseAdmission::BudgetRefused,
            "failed device half must refuse");
    require(budget.snapshot().hostReservedBytes == 0 && budget.snapshot().admittedPhases == 0,
            "failed paired admission must leave host/count untouched");
    budget.setDeviceCapacity(30, true);
    auto grant = budget.reserve(request());
    require(grant.lease && grant.lease->hostBytes() == 70 && grant.lease->deviceBytes() == 30,
            "exact paired minimum must fit");
    budget.setHostCapacity(0, true);
    require(budget.reserve(request()).status == GpuProcessingPhaseAdmission::BudgetRefused,
            "measured zero must refuse instead of await");
    require(budget.snapshot().hostReservedBytes == 70,
            "shrinking capacity must not free live phase");
    budget.setHostCapacity(std::nullopt, false);
    require(budget.snapshot().hostCapacityBytes == 0 &&
                budget.reserve(request()).status == GpuProcessingPhaseAdmission::AwaitingCapacity,
            "unknown closes new admission without losing prior cap");
    grant.lease.reset();
    require(budget.snapshot().hostReservedBytes == 0 && budget.snapshot().deviceReservedBytes == 0,
            "release returns both halves after shrink");
    budget.setHostCapacity(10, true);
    auto hostOnly = request();
    hostOnly.native = false;
    hostOnly.sourceFree = true;
    hostOnly.minimumSourceBytes = 0;
    hostOnly.deviceWorkingBytes = 0;
    hostOnly.requestedExecutorBytes = 0;
    hostOnly.minimumExecutorBytes = 0;
    budget.setDeviceCapacity(std::nullopt, false);
    require(budget.reserve(hostOnly).status == GpuProcessingPhaseAdmission::Accepted,
            "admitted CPU route does not need device readiness");
}
void emptyAndZeroGrant() {
    GpuProcessingPhaseBudget budget;
    GpuProcessingPhaseRequest empty;
    empty.sourceFree = true;
    budget.setHostCapacity(0, true);
    require(budget.reserve(empty).domain == GpuProcessingPhaseDomain::Host,
            "measured zero host wins even while device capacity is unknown");
    budget.setDeviceCapacity(0, true);
    require(budget.reserve(empty).domain == GpuProcessingPhaseDomain::Host,
            "zero host refuses even an empty preparation request");
    budget.setHostCapacity(std::nullopt, false);
    require(budget.reserve(empty).domain == GpuProcessingPhaseDomain::Device,
            "measured zero device wins even while host capacity is unknown");
    budget.setHostCapacity(10, true);
    require(budget.reserve(empty).domain == GpuProcessingPhaseDomain::Device,
            "zero applicable device refuses even an empty preparation request");
    budget.setDeviceCapacity(10, true);
    empty.sourceFree = false;
    empty.fixedHostBytes = 10;
    empty.requestedSourceBytes = 1;
    require(budget.reserve(empty).domain == GpuProcessingPhaseDomain::Host,
            "positive source request cannot receive a zero cap");
    empty.sourceFree = true;
    empty.deviceWorkingBytes = 10;
    empty.requestedExecutorBytes = 1;
    require(budget.reserve(empty).domain == GpuProcessingPhaseDomain::Device,
            "positive executor request cannot receive a zero cap");
    require(budget.snapshot().admittedPhases == 0, "zero-grant refusals consume no request slots");
}
void sizingAndLifetime() {
    GpuProcessingPhaseBudget budget(1);
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    budget.setHostCapacity(gib, true);
    budget.setDeviceCapacity(gib, true);
    constexpr std::uint64_t fullPixels = 1920ULL * 1080;
    auto full = request();
    full.fixedHostBytes = 2 * fullPixels * 4 + 4096;
    full.requestedSourceBytes = gib;
    full.minimumSourceBytes = 0;
    full.sourceFree = true;
    full.deviceWorkingBytes = 2 * fullPixels * 16;
    full.requestedExecutorBytes = gib;
    full.minimumExecutorBytes = 4 * fullPixels * 16;
    auto grant = budget.reserve(full);
    require(grant.lease && grant.lease->sourceByteLimit() == 0 &&
                grant.lease->hostBytes() == full.fixedHostBytes &&
                grant.lease->deviceBytes() == gib,
            "1080p source-free envelope admits without whole-domain reservation");
    auto alias = grant.lease;
    grant.lease->markUnproven();
    alias->markUnproven();
    grant.lease.reset();
    require(budget.snapshot().admittedPhases == 1 && budget.snapshot().unprovenPhases == 1,
            "unproven aliases charge once until last owner retires");
    require(budget.reserve(full).domain == GpuProcessingPhaseDomain::RequestCount,
            "generation retention cannot grow beyond bounded requests");
    alias.reset();
    require(budget.snapshot().admittedPhases == 0 && budget.snapshot().unprovenPhases == 0,
            "last alias releases retained unproven reservation");
    full.sourceFree = false;
    auto media = budget.reserve(full);
    const auto sourceRoom = (gib - full.fixedHostBytes) / 3;
    require(media.lease && media.lease->sourceByteLimit() == sourceRoom &&
                media.lease->sourceByteLimit() > gib / 4 && media.lease->hostBytes() <= gib,
            "media source beyond a quarter admits within the exact paired host remainder");
    media.lease.reset();
    GpuProcessingPhaseBudget concurrent(2);
    concurrent.setHostCapacity(1000, true);
    concurrent.setDeviceCapacity(1000, true);
    GpuProcessingPhaseRequest fitting{.fixedHostBytes = 40,
                                      .requestedSourceBytes = 100,
                                      .minimumSourceBytes = 100,
                                      .deviceWorkingBytes = 100,
                                      .requestedExecutorBytes = 500,
                                      .minimumExecutorBytes = 500};
    auto first = concurrent.reserve(fitting);
    require(first.lease && first.lease->executorByteLimit() == 500,
            "executor beyond a quarter admits when actual device capacity fits");
    auto held = concurrent.reserve(fitting);
    require(!held.lease && held.domain == GpuProcessingPhaseDomain::Device,
            "held device claims refuse a second phase that would overcommit");
    first.lease.reset();
    held = concurrent.reserve(fitting);
    require(held.lease != nullptr, "retired claims restore paired admission");
    held.lease.reset();
    const auto max = std::numeric_limits<std::uint64_t>::max();
    budget.setHostCapacity(max, true);
    budget.setDeviceCapacity(max, true);
    auto huge = request();
    huge.fixedHostBytes = max - 2;
    huge.minimumSourceBytes = 1;
    require(budget.reserve(huge).domain == GpuProcessingPhaseDomain::Host,
            "overflow-sized minimum refuses before multiplication");
    huge.minimumSourceBytes = 0;
    huge.requestedSourceBytes = 0;
    huge.requestedExecutorBytes = max;
    huge.minimumExecutorBytes = 0;
    auto bounded = budget.reserve(huge);
    require(bounded.lease && bounded.lease->hostBytes() == max - 2,
            "wide valid envelope remains bounded without overflow");
    bounded.lease.reset();
    std::shared_ptr<GpuProcessingPhaseLease> surviving;
    {
        GpuProcessingPhaseBudget temporary;
        temporary.setHostCapacity(70, true);
        temporary.setDeviceCapacity(30, true);
        surviving = temporary.reserve(request()).lease;
    }
    require(surviving && surviving->hostBytes() == 70,
            "native owner lease survives composition-root destruction");
    surviving.reset();
}
void retryWatermarks() {
    GpuProcessingPhaseBudget budget(4);
    budget.setHostCapacity(1000, true);
    budget.setDeviceCapacity(1000, true);
    const auto baseline = budget.snapshot().admissionRevision;
    budget.setHostCapacity(1000, true);
    require(budget.snapshot().admissionRevision == baseline,
            "identical samples do not create readiness events");
    auto own = budget.reserve(request());
    require(own.lease && own.admissionRevision == baseline,
            "atomic admission captures exact refusal baseline");
    const auto ignoredOwnRelease = own.lease->revisionAfterOwnRelease();
    auto alias = own.lease;
    own.lease.reset();
    require(budget.snapshot().admissionRevision == baseline,
            "nonlast alias is not a release event");
    alias.reset();
    require(budget.snapshot().admissionRevision == ignoredOwnRelease,
            "failed attempt's own last release alone cannot exceed its retry watermark");
    own = budget.reserve(request());
    const auto watermark = own.lease->revisionAfterOwnRelease();
    auto external = budget.reserve(request());
    require(external.lease != nullptr, "competing phase admitted");
    external.lease.reset(); // Genuine external release before own refusal delivery.
    own.lease.reset();
    require(budget.snapshot().admissionRevision > watermark,
            "external release before failed delivery remains visible beyond excluded own release");
    own = budget.reserve(request());
    const auto capacityWatermark = own.lease->revisionAfterOwnRelease();
    budget.setHostCapacity(1100, true);
    own.lease.reset();
    require(budget.snapshot().admissionRevision > capacityWatermark,
            "capacity change is not erased by failed attempt disposal");
}
void finishedPreparationReleasesDecodeScratch() {
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(1000, true);
    budget.setDeviceCapacity(1000, true);
    auto grant = budget.reserve(
        {.fixedHostBytes = 100, .requestedSourceBytes = 300, .deviceWorkingBytes = 1000});
    require(grant.lease && budget.snapshot().totalHostClaimBytes == 1000,
            "preparation initially protects its full decode/conversion/upload overlap");
    const NativeBufferStorageIdentity identity{
        .allocatorGeneration = 1, .memoryType = 1, .heap = 0, .deviceLocal = false};
    auto blocked = NativeBufferStorageToken::reserveProcessing(grant.lease, 64, identity);
    require(!blocked.token, "unneeded decode reservation previously starved native scratch");
    auto earlyUpload = NativeBufferStorageToken::reserveUpload(grant.lease, 10, identity);
    require(earlyUpload.token && !grant.lease->finishSourcePreparation(20),
            "preparation cannot change host accounting while an earlier upload is live");
    earlyUpload.token.reset();
    require(!grant.lease->finishSourcePreparation(301),
            "final source storage cannot exceed its admitted allowance");
    require(grant.lease->finishSourcePreparation(20) &&
                budget.snapshot().totalHostClaimBytes == 120,
            "finished preparation retains actual sources, not unused decode or upload storage");
    require(!grant.lease->finishSourcePreparation(20) &&
                budget.snapshot().totalHostClaimBytes == 120,
            "completion cannot release the same reservation twice");
    auto scratch = NativeBufferStorageToken::reserveProcessing(grant.lease, 64, identity);
    require(scratch.token && budget.snapshot().totalHostClaimBytes == 184,
            "native scratch can use released headroom without changing the device limit");
    budget.setHostCapacity(184, true);
    auto exhausted = NativeBufferStorageToken::reserveUpload(grant.lease, 1, identity);
    require(!exhausted.token && budget.snapshot().totalHostClaimBytes == 184,
            "deferred upload admission still refuses real host exhaustion without a partial claim");
    budget.setHostCapacity(1000, true);
    auto upload = NativeBufferStorageToken::reserveUpload(grant.lease, 300, identity);
    require(upload.token && budget.snapshot().totalHostClaimBytes == 484,
            "native staging acquires its real host allocation within the original upload ceiling");
    require(upload.token->reconcile(280, identity) && budget.snapshot().totalHostClaimBytes == 464,
            "actual smaller upload backing returns only its own supplemental host claim");
    scratch.token.reset();
    upload.token.reset();
    grant.lease.reset();
    require(budget.snapshot().totalHostClaimBytes == 0 &&
                budget.snapshot().totalDeviceClaimBytes == 0 &&
                budget.snapshot().admittedPhases == 0,
            "trimmed phase and actual allocations retire with exact conservation");
    grant = budget.reserve(
        {.fixedHostBytes = 100, .requestedSourceBytes = 300, .deviceWorkingBytes = 1000});
    require(grant.lease && grant.lease->finishSourcePreparation(20),
            "a second preparation can release unused scratch");
    const auto watermark = grant.lease->revisionAfterOwnRelease();
    grant.lease.reset();
    require(budget.snapshot().admissionRevision == watermark,
            "own scratch release followed by teardown cannot create a self-retry loop");
}
void rootIdentity() {
    GpuProcessingPhaseBudget first;
    GpuProcessingPhaseBudget second;
    for (auto* budget : {&first, &second}) {
        budget->setHostCapacity(70, true);
        budget->setDeviceCapacity(30, true);
    }
    auto a = first.reserve(request());
    auto b = second.reserve(request());
    require(a.lease && b.lease && a.lease->id() == b.lease->id(),
            "distinct roots can have the same numeric phase id");
    const auto identity = a.lease->rootIdentity();
    require(identity && identity != b.lease->rootIdentity(),
            "root identity must distinguish equal phase ids from different roots");
    const std::weak_ptr<GpuProcessingPhaseLease> weak = a.lease;
    a.lease.reset();
    require(weak.expired() && first.snapshot().admittedPhases == 0,
            "retaining neutral root identity must not retain a phase or its claims");
    require(!b.lease->isUnproven(), "new authority begins proven");
    b.lease->markUnproven();
    require(b.lease->isUnproven(), "authority exposes the sticky unproven latch");
    auto next = first.reserve(request());
    require(next.lease && next.lease->rootIdentity() == identity,
            "later phases preserve the exact neutral root identity");
}
void contention() {
    constexpr std::size_t contenders = 32;
    GpuProcessingPhaseBudget budget(2);
    budget.setHostCapacity(140, true);
    budget.setDeviceCapacity(60, true);
    std::array<GpuProcessingPhaseGrant, contenders> grants;
    std::mutex startMutex;
    std::condition_variable_any start;
    bool ready = false;
    std::vector<std::jthread> workers;
    workers.reserve(contenders);
    for (std::size_t index = 0; index < contenders; ++index) {
        workers.emplace_back([&, index](const std::stop_token& stop) {
            {
                std::unique_lock lock(startMutex);
                if (!start.wait(lock, stop, [&] { return ready; })) {
                    return;
                }
            }
            grants[index] = budget.reserve(request());
        });
    }
    {
        const std::lock_guard lock(startMutex);
        ready = true;
    }
    start.notify_all();
    for (auto& worker : workers) {
        worker.join();
    }
    workers.clear();
    std::size_t accepted = 0;
    for (const auto& grant : grants) {
        if (grant.status == GpuProcessingPhaseAdmission::Accepted) {
            ++accepted;
        }
    }
    const auto snapshot = budget.snapshot();
    require(accepted == 2 && snapshot.hostReservedBytes == 140 &&
                snapshot.deviceReservedBytes == 60 && snapshot.budgetRefusals == contenders - 2,
            "competing requests never acquire a partial or over-capacity pair");
    for (auto& grant : grants) {
        grant.lease.reset();
    }
    require(budget.snapshot().admittedPhases == 0 && budget.snapshot().hostReservedBytes == 0 &&
                budget.snapshot().deviceReservedBytes == 0,
            "all contending aliases retire exactly once");
}
} // namespace
int main() {
    try {
        readinessAndRollback();
        emptyAndZeroGrant();
        sizingAndLifetime();
        rootIdentity();
        contention();
        retryWatermarks();
        finishedPreparationReleasesDecodeScratch();
        std::cout << "paired phase readiness/sizing/lifetime/32-way contention PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
