#include <bloom/runtime/gpu_processing_phase_policy.hpp>

#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace bloom::runtime;
using namespace bloom::render;
constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
void require(const bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
MemoryBudgetState sample(const GpuProcessingPhasePolicy::Clock::time_point now) {
    MemoryBudgetState result;
    result.machine.availableBytes = 60 * gib;
    result.process.source = ProcessMemorySource::LinuxCgroupV2;
    result.process.status = ProcessMemoryStatus::Finite;
    result.process.sampledAt = now;
    result.process.domains.push_back({.path = "/own",
                                      .kind = ProcessMemoryLimitKind::Finite,
                                      .limitBytes = 8 * gib,
                                      .currentBytes = 6 * gib});
    result.sampleSerial = 1;
    result.processSampleFresh = true;
    return result;
}
GpuProcessingPhaseEnvelope preview() {
    return {.pixels = 1920ULL * 1080,
            .metadataBytes = 4096,
            .requestedSourceBytes = gib,
            .requestedExecutorBytes = gib,
            .minimumExecutorBytes = 4 * 1920ULL * 1080 * 16,
            .deviceWorkingBytes = 2 * 1920ULL * 1080 * 16,
            .sourceFree = true};
}
void policy() {
    const auto now = GpuProcessingPhasePolicy::Clock::now();
    GpuProcessingPhasePolicy policy;
    require(policy.reserve(preview(), now).status == GpuProcessingPhaseAdmission::AwaitingCapacity,
            "cold startup cannot prepare before sampling");
    policy.applyHostSample(sample(now), 60 * gib, now);
    policy.applyHostSample(sample(now), 60 * gib, now);
    require(policy.snapshot().reservations.hostReady,
            "duplicate delivered snapshot preserves existing readiness without growth");
    require(policy.reportDevice(1, {.source = GpuAllocationBudgetSource::MemoryBudget,
                                    .device_local_bytes = 8 * gib,
                                    .available_bytes = gib}),
            "first native owner reports its own budget");
    auto grant = policy.reserve(preview(), now);
    require(grant.lease && policy.snapshot().reservations.hostCapacityBytes == gib,
            "paired 8GiB/6GiB process reserve limits host envelope to1GiB");
    require(grant.lease->hostBytes() == 1920ULL * 1080 * 8 + 4096,
            "source-free1080 minimum admits under previous1GiB cap");
    grant.lease.reset();
    auto pressure = sample(now);
    pressure.sampleSerial = 2;
    pressure.process.status = ProcessMemoryStatus::Partial;
    pressure.process.domains[0].limitBytes = 0;
    pressure.process.domains[0].currentBytes.reset();
    policy.applyHostSample(pressure, 60 * gib, now);
    require(policy.reserve(preview(), now).status == GpuProcessingPhaseAdmission::BudgetRefused,
            "reliable zero limit refuses even when partial usage is missing");
    auto failure = sample(now);
    failure.sampleSerial = 3;
    failure.process.status = ProcessMemoryStatus::Unavailable;
    failure.process.domains.clear();
    policy.applyHostSample(failure, 60 * gib, now);
    require(policy.snapshot().reservations.hostCapacityBytes == 0,
            "read failure cannot erase earlier finite cap");
    auto recovered = sample(now + std::chrono::seconds(1));
    recovered.sampleSerial = 4;
    policy.applyHostSample(recovered, 60 * gib, now + std::chrono::seconds(1));
    require(policy.reserve(preview(), now + std::chrono::seconds(12)).status ==
                GpuProcessingPhaseAdmission::AwaitingCapacity,
            "stale capacity closes new admission without polling");
    recovered = sample(now + std::chrono::seconds(2));
    recovered.sampleSerial = 5;
    policy.applyHostSample(recovered, 60 * gib, now + std::chrono::seconds(2));
    const auto activeNow = now + std::chrono::seconds(2);
    require(policy.reportDevice(2, {.source = GpuAllocationBudgetSource::DeviceLocalEstimate,
                                    .device_local_bytes = 512ULL * 1024 * 1024,
                                    .available_bytes = 512ULL * 1024 * 1024}),
            "separate final generation joins common conservative quota");
    const auto snapshot = policy.snapshot();
    require(snapshot.reservations.deviceCapacityBytes == 256ULL * 1024 * 1024 &&
                snapshot.usesNominalDeviceEstimate && snapshot.deviceReports == 2,
            "all-device minimum is explicit and nominal provenance retained");
    auto half = preview();
    half.pixels = 960ULL * 540;
    auto fullGrant = policy.reserve(preview(), activeNow);
    require(fullGrant.lease != nullptr, "Full old request ceiling admits");
    fullGrant.lease.reset();
    auto halfGrant = policy.reserve(half, activeNow);
    require(halfGrant.lease != nullptr,
            "Full/Half width-one requests admit within shared available quota");
    fullGrant.lease.reset();
    halfGrant.lease.reset();
    auto hostOnly = preview();
    hostOnly.native = false;
    hostOnly.requestedExecutorBytes = 0;
    hostOnly.minimumExecutorBytes = 0;
    hostOnly.deviceWorkingBytes = 0;
    require(policy.reportDevice(2, {}), "unavailable device report is typed");
    require(policy.reserve(preview(), activeNow).status ==
                GpuProcessingPhaseAdmission::AwaitingCapacity,
            "unknown native report cannot silently become unlimited");
    require(!policy.retireDeviceReport(2, false), "unproven generation cannot erase its report");
    require(policy.retireDeviceReport(2, true), "proven retired owner releases report slot");
    require(policy.snapshot().deviceReports == 1 &&
                policy.reserve(preview(), activeNow).lease != nullptr,
            "a retired unavailable generation cannot block live replacement forever");
    require(policy.reserve(hostOnly, activeNow).lease != nullptr,
            "CPU route remains host admitted independently of native availability");
}
void monotonicRecoveryAndSlots() {
    const auto now = GpuProcessingPhasePolicy::Clock::now();
    GpuProcessingPhasePolicy policy;
    auto lower = sample(now + std::chrono::seconds(5));
    lower.process.domains[0].currentBytes = 7 * gib - 256ULL * 1024 * 1024;
    lower.sampleSerial = 2;
    policy.applyHostSample(lower, 60 * gib, now + std::chrono::seconds(5));
    require(policy.snapshot().reservations.hostCapacityBytes == 256ULL * 1024 * 1024,
            "newest pressure sample establishes lower allowance");
    auto older = sample(now + std::chrono::seconds(1));
    older.processSampleFresh = false;
    older.sampleSerial = 3;
    policy.applyHostSample(older, 60 * gib, now + std::chrono::seconds(6));
    require(policy.snapshot().reservations.hostCapacityBytes == 256ULL * 1024 * 1024 &&
                !policy.snapshot().reservations.hostReady,
            "superseded high headroom cannot authorize recovery");
    policy.applyHostSample(older, 60 * gib, now + std::chrono::seconds(6));
    require(!policy.snapshot().reservations.hostReady,
            "duplicate snapshot cannot reopen admission");
    auto fresh = sample(now + std::chrono::seconds(7));
    fresh.sampleSerial = 4;
    policy.applyHostSample(fresh, 60 * gib, now + std::chrono::seconds(7));
    require(policy.snapshot().reservations.hostCapacityBytes == gib &&
                policy.snapshot().reservations.hostReady,
            "only a fresh monotonic complete sample permits recovery");
    const GpuAllocationBudget native{.source = GpuAllocationBudgetSource::MemoryBudget,
                                     .device_local_bytes = gib,
                                     .available_bytes = gib};
    for (std::uint64_t epoch = 1; epoch <= 32; ++epoch) {
        require(policy.reportDevice(epoch, native), "bounded epoch slots fill");
    }
    require(!policy.reportDevice(33, native), "report exhaustion is explicit");
    require(policy.retireDeviceReport(1, true) && policy.reportDevice(33, native),
            "proven retired owner slot is reusable without erasing live reservations");
}
void malformedFallbackCannotRecover() {
    const auto now = GpuProcessingPhasePolicy::Clock::now();
    auto zero = sample(now);
    zero.process.domains[0].limitBytes = 0;
    GpuProcessingPhasePolicy policy;
    policy.applyHostSample(zero, 60 * gib, now);
    auto malformed = sample(now + std::chrono::seconds(1));
    malformed.sampleSerial = 2;
    malformed.process.status = ProcessMemoryStatus::Unsupported;
    policy.applyHostSample(malformed, 60 * gib, malformed.process.sampledAt);
    require(policy.snapshot().reservations.hostCapacityBytes == 0,
            "unsupported label with finite payload cannot raise prior measured zero");
}
void verifiedConstraintRemoval() {
    const auto now = GpuProcessingPhasePolicy::Clock::now();
    auto finite = sample(now);
    finite.process.membershipPath = "/own";
    finite.process.view =
        ProcessMemoryView{.mountRoot = "/",
                          .mountPoint = "/",
                          .filesystemDevice = "0:32",
                          .mountId = 36,
                          .namespaces = {.cgroup = ProcessMemoryNamespaceIdentity{1, 2},
                                         .mount = ProcessMemoryNamespaceIdentity{1, 3}}};
#if defined(__linux__)
    auto root = finite;
    root.process = parseLinuxProcessMemorySample(
        "0::/\n", "36 25 0:32 / /cgroup rw - cgroup2 cgroup rw\n",
        [](const std::filesystem::path&) {
            return ProcessMemoryFileRead{ProcessMemoryReadStatus::Missing, {}};
        },
        now,
        {.cgroup = ProcessMemoryNamespaceIdentity{1, 2},
         .mount = ProcessMemoryNamespaceIdentity{1, 3}});
    GpuProcessingPhasePolicy rootPolicy;
    rootPolicy.applyHostSample(root, 60 * gib, now);
    auto hostRequest = preview();
    hostRequest.native = false;
    hostRequest.deviceWorkingBytes = 0;
    hostRequest.requestedExecutorBytes = 0;
    hostRequest.minimumExecutorBytes = 0;
    require(rootPolicy.reserve(hostRequest, now).status == GpuProcessingPhaseAdmission::Accepted,
            "parser-proven cold hierarchy root admits host phase without permanent Awaiting");
#endif
    GpuProcessingPhasePolicy policy;
    policy.applyHostSample(finite, 60 * gib, now);
    const auto oldCap = policy.snapshot().reservations.hostCapacityBytes;
    auto held = policy.reserve({.pixels = 1, .sourceFree = true, .native = false}, now);
    require(held.lease != nullptr, "small existing phase admitted before limit removal");
    const auto heldSnapshot = policy.snapshot().reservations;
    auto unlimited = finite;
    unlimited.sampleSerial = 2;
    unlimited.process.sampledAt = now + std::chrono::seconds(1);
    unlimited.process.status = ProcessMemoryStatus::NoFiniteVisibleLimit;
    unlimited.process.domains[0].kind = ProcessMemoryLimitKind::Unlimited;
    unlimited.process.domains[0].limitBytes = 0;
    auto changed = unlimited;
    if (!changed.process.view)
        throw std::runtime_error("fixture view missing");
    changed.process.view->namespaces.mount = ProcessMemoryNamespaceIdentity{1, 4};
    policy.applyHostSample(changed, 60 * gib, changed.process.sampledAt);
    require(policy.snapshot().reservations.hostCapacityBytes == oldCap,
            "phase policy keeps finite constraint under changed namespace");
    changed = unlimited;
    changed.sampleSerial = 3;
    changed.process.sampledAt = now + std::chrono::seconds(2);
    changed.process.domains.clear();
    policy.applyHostSample(changed, 60 * gib, changed.process.sampledAt);
    require(policy.snapshot().reservations.hostCapacityBytes == oldCap,
            "phase policy missing domains cannot clear constraint");
    unlimited.sampleSerial = 4;
    unlimited.processSampleFresh = false;
    policy.applyHostSample(unlimited, 60 * gib, now + std::chrono::seconds(3));
    require(policy.snapshot().reservations.hostCapacityBytes == oldCap,
            "superseded unlimited proof cannot reopen capacity");
    unlimited.sampleSerial = 5;
    unlimited.processSampleFresh = true;
    unlimited.process.sampledAt = now + std::chrono::seconds(4);
    policy.applyHostSample(unlimited, 60 * gib, unlimited.process.sampledAt);
    require(policy.snapshot().reservations.hostCapacityBytes > oldCap &&
                policy.snapshot().reservations.hostReady,
            "verified removed constraint restores only measured host phase allowance");
    require(policy.snapshot().reservations.hostReservedBytes == heldSnapshot.hostReservedBytes &&
                policy.snapshot().reservations.admittedPhases == heldSnapshot.admittedPhases,
            "capacity recovery never releases or restamps an admitted phase");
    const auto recovered = policy.snapshot().reservations.hostCapacityBytes;
    policy.applyHostSample(unlimited, 60 * gib, unlimited.process.sampledAt);
    require(policy.snapshot().reservations.hostCapacityBytes == recovered &&
                policy.snapshot().reservations.hostReady,
            "duplicate verified removal leaves capacity stable and ready");
    auto zero = finite;
    zero.sampleSerial = 6;
    zero.process.sampledAt = now + std::chrono::seconds(5);
    zero.process.domains[0].limitBytes = 0;
    policy.applyHostSample(zero, 60 * gib, zero.process.sampledAt);
    require(
        policy.reserve({.pixels = 1, .sourceFree = true, .native = false}, zero.process.sampledAt)
                    .status == GpuProcessingPhaseAdmission::BudgetRefused &&
            policy.snapshot().reservations.hostReservedBytes == heldSnapshot.hostReservedBytes &&
            policy.snapshot().reservations.admittedPhases == heldSnapshot.admittedPhases,
        "new measured zero refuses growth without freeing held phase");
    held.lease.reset();
    require(policy.snapshot().reservations.admittedPhases == 0,
            "last held phase owner releases normally under measured-zero quota");
}

void envelope() {
    auto input = preview();
    auto packed = processingPhaseRequest(input);
    require(packed && packed->fixedHostBytes == 2 * input.pixels * 4 + input.metadataBytes,
            "packed envelope includes staging and immutable output overlap");
    input.output = GpuProcessingPhaseOutput::FloatFinal;
    input.encodedBytes = input.pixels * 4;
    auto final = processingPhaseRequest(input);
    require(final && final->fixedHostBytes ==
                         3 * input.pixels * 16 + 2 * input.encodedBytes + input.metadataBytes,
            "final envelope includes readback vectors and adoption/encoding overlap");
    input.pixels = std::numeric_limits<std::uint64_t>::max();
    require(!processingPhaseRequest(input), "overflow refuses before any preparation");
    input.pixels = 1;
    input.encodedBytes = std::numeric_limits<std::uint64_t>::max();
    require(!processingPhaseRequest(input), "encoded overlap overflow refuses");
    input.encodedBytes = 0;
    input.pixels = 0;
    require(!processingPhaseRequest(input), "empty output is not a processing phase");
}
int liveConstraintRemoval() {
#if defined(__linux__)
    MemoryBudgetLedger ledger;
    ledger.beginProcessSampling();
    GpuProcessingPhasePolicy policy;
    const auto before = memoryBudgetSample();
    auto state = ledger.applySample(before);
    policy.applyHostSample(state, before.physicalBytes);
    require(before.process.status == ProcessMemoryStatus::Finite && before.process.view &&
                before.process.view->namespaces.cgroup && before.process.view->namespaces.mount &&
                state.processCacheCeilingBytes,
            "private helper must start in a verified finite Linux view");
    const auto print = [](const char* phase, const MemoryBudgetSample& probe,
                          const MemoryBudgetState& budgets,
                          const GpuProcessingPhasePolicySnapshot& phases) {
        require(probe.process.view.has_value(), "private helper view remains available");
        const auto& view = *probe.process.view;
        require(view.namespaces.cgroup && view.namespaces.mount, "namespace identities verified");
        std::cout << phase << " membership=" << probe.process.membershipPath
                  << " mount_id=" << view.mountId << " filesystem=" << view.filesystemDevice
                  << " root=" << view.mountRoot << " point=" << view.mountPoint
                  << " cgroup_ns=" << view.namespaces.cgroup->device << ':'
                  << view.namespaces.cgroup->inode << " mount_ns=" << view.namespaces.mount->device
                  << ':' << view.namespaces.mount->inode
                  << " status=" << static_cast<int>(probe.process.status)
                  << " configured=" << budgets.configuredBytes
                  << " effective=" << budgets.effectiveBytes
                  << " ceiling_known=" << budgets.processCacheCeilingBytes.has_value()
                  << " ceiling=" << budgets.processCacheCeilingBytes.value_or(0)
                  << " phase_host=" << phases.reservations.hostCapacityBytes.value_or(0)
                  << " phase_ready=" << phases.reservations.hostReady << '\n';
        for (const auto& domain : probe.process.domains)
            std::cout << phase << " domain=" << domain.path
                      << " kind=" << static_cast<int>(domain.kind) << " limit=" << domain.limitBytes
                      << " current=" << domain.currentBytes.value_or(0)
                      << " current_known=" << domain.currentBytes.has_value() << '\n';
    };
    print("before", before, state, policy.snapshot());
    const auto initial = state.effectiveBytes;
    const auto phaseInitial = policy.snapshot().reservations.hostCapacityBytes;
    std::cout << "READY_FOR_PRIVATE_LIMIT_REMOVAL" << '\n' << std::flush;
    std::string token;
    require(static_cast<bool>(std::getline(std::cin, token)) && token == "continue",
            "private helper requires controlled external limit-removal handoff");
    const auto after = memoryBudgetSample();
    state = ledger.applySample(after);
    policy.applyHostSample(state, after.physicalBytes);
    print("after", after, state, policy.snapshot());
    require(after.process.status == ProcessMemoryStatus::NoFiniteVisibleLimit &&
                before.process.view == after.process.view &&
                before.process.membershipPath == after.process.membershipPath &&
                !state.processCacheCeilingBytes && state.effectiveBytes == initial &&
                policy.snapshot().reservations.hostCapacityBytes > phaseInitial,
            "actual same-view explicit max removes observed constraint without retention jump");
    const auto recovery = memoryBudgetSample();
    // Actual OS samples, deterministic policy hold clock: avoid a ten-second live sleep.
    const auto recoveryNow = recovery.process.sampledAt + std::chrono::seconds(10);
    state = ledger.applySample(recovery, recoveryNow);
    policy.applyHostSample(state, recovery.physicalBytes, recoveryNow);
    print("recovery", recovery, state, policy.snapshot());
    require(state.effectiveBytes > initial && state.effectiveBytes <= initial + initial / 4,
            "actual removed constraint follows bounded deterministic recovery hold");
    std::cout << "policy_clock_offset_seconds=10 (not real-time waiting)\n";
    return 0;
#else
    return 77;
#endif
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--live-unlimited-probe")
            return liveConstraintRemoval();
        policy();
        monotonicRecoveryAndSlots();
        malformedFallbackCannotRecover();
        verifiedConstraintRemoval();
        envelope();
        std::cout << "sampled paired policy/preview+final sizing PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
