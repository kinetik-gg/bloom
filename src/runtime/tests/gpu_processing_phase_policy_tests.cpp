#include <bloom/render/native_image_storage.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>

#include <algorithm>
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
void lowMemoryHostFinalAdmission() {
    const auto now = GpuProcessingPhasePolicy::Clock::now();
    constexpr std::size_t mib = std::size_t{1024} * 1024;
    const auto deliver = [&](GpuProcessingPhasePolicy& policy, const std::size_t physical,
                             const std::optional<std::size_t> available,
                             const std::optional<std::size_t> finiteAllowance = std::nullopt) {
        MemoryBudgetSample reading;
        reading.physicalBytes = physical;
        reading.machine.availableBytes = available;
        reading.process.source = ProcessMemorySource::MacOsHostOnly;
        reading.process.status = ProcessMemoryStatus::Unsupported;
        reading.process.sampledAt = now;
        if (finiteAllowance) {
            reading.process.source = ProcessMemorySource::LinuxCgroupV2;
            reading.process.status = ProcessMemoryStatus::Finite;
            reading.process.domains.push_back({.path = "/own",
                                               .kind = ProcessMemoryLimitKind::Finite,
                                               .limitBytes = 4 * gib,
                                               .currentBytes = 4 * gib - *finiteAllowance});
        }
        MemoryBudgetLedger ledger(physical, available.value_or(0));
        ledger.beginProcessSampling();
        const auto delivered = ledger.applySample(std::move(reading), now);
        policy.applyHostSample(delivered, physical, now);
        return delivered;
    };
    const auto final = [](GpuProcessingPhasePolicy& policy) {
        return reserveHostEvaluationPhase(policy, 64ULL * 48, mib,
                                          GpuProcessingPhaseOutput::FloatFinal);
    };
    for (const auto physical : {7 * gib, 8 * gib}) {
        GpuProcessingPhasePolicy policy;
        deliver(policy, physical, physical);
        require(policy.snapshot().reservations.hostReady &&
                    policy.snapshot().reservations.hostCapacityBytes == physical,
                "small hosts expose measured headroom without a hidden reserve");
        auto grant = final(policy);
        require(grant.status == GpuProcessingPhaseAdmission::Accepted && grant.lease &&
                    grant.lease->sourceByteLimit() >= 64ULL * 48 * 16,
                "a fresh small-host CPU final gets a genuine bounded source allowance");
        grant.lease.reset();
        require(policy.snapshot().reservations.totalHostClaimBytes == 0,
                "small-host final admission releases its exact conservative claim");
        GpuProcessingPhasePolicy oversampled;
        deliver(oversampled, physical, physical * 2);
        require(oversampled.snapshot().reservations.hostCapacityBytes == physical,
                "overlapping machine counters cannot exceed physically bounded usable memory");
        require(policy.reserve({.pixels = 1, .sourceFree = true, .native = false},
                               now + std::chrono::seconds(11))
                        .status == GpuProcessingPhaseAdmission::AwaitingCapacity,
                "the small-host floor cannot extend the sample deadline");
    }
    const std::size_t smallPhysical = 7 * gib;
    for (const auto available : {std::size_t{0}, std::size_t{32} * mib}) {
        GpuProcessingPhasePolicy policy;
        deliver(policy, smallPhysical, available);
        require(policy.snapshot().reservations.hostReady &&
                    policy.snapshot().reservations.hostCapacityBytes == available &&
                    final(policy).status == GpuProcessingPhaseAdmission::BudgetRefused,
                "measured zero and low availability still refuse the actual final minimum");
    }
    for (const auto processAllowance : {std::size_t{0}, std::size_t{32} * mib}) {
        GpuProcessingPhasePolicy policy;
        deliver(policy, smallPhysical, smallPhysical, processAllowance);
        require(policy.snapshot().reservations.hostReady &&
                    policy.snapshot().reservations.hostCapacityBytes == processAllowance &&
                    final(policy).status == GpuProcessingPhaseAdmission::BudgetRefused,
                "finite process headroom remains authoritative below the small-host floor");
    }
    GpuProcessingPhasePolicy unavailable;
    deliver(unavailable, smallPhysical, std::nullopt);
    require(!unavailable.snapshot().reservations.hostReady &&
                final(unavailable).status == GpuProcessingPhaseAdmission::AwaitingCapacity,
            "unknown machine availability never acquires the small-host floor");
    GpuProcessingPhasePolicy stale;
    auto delivered = deliver(stale, smallPhysical, smallPhysical);
    ++delivered.sampleSerial;
    delivered.processSampleFresh = false;
    stale.applyHostSample(delivered, smallPhysical, now);
    require(!stale.snapshot().reservations.hostReady &&
                final(stale).status == GpuProcessingPhaseAdmission::AwaitingCapacity,
            "a nonfresh delivered sample cannot authorize small-host processing");
    GpuProcessingPhasePolicy unknownPhysical;
    deliver(unknownPhysical, 0, smallPhysical);
    require(unknownPhysical.snapshot().reservations.hostCapacityBytes == smallPhysical &&
                final(unknownPhysical).status == GpuProcessingPhaseAdmission::Accepted,
            "unknown physical capacity does not discard valid measured headroom");
    GpuProcessingPhasePolicy unknownPhysicalLow;
    deliver(unknownPhysicalLow, 0, 32 * mib);
    require(unknownPhysicalLow.snapshot().reservations.hostCapacityBytes == 32 * mib &&
                final(unknownPhysicalLow).status == GpuProcessingPhaseAdmission::BudgetRefused,
            "unknown physical capacity never promotes low availability to the fallback ceiling");
    GpuProcessingPhasePolicy ordinary;
    deliver(ordinary, 60 * gib, 60 * gib);
    require(ordinary.snapshot().reservations.hostCapacityBytes ==
                MemoryBudgetLedger::usableByteBudgetForPhysicalMemory(60 * gib),
            "ordinary hosts retain the configured physical usable ceiling");
    GpuProcessingPhasePolicy largest;
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    deliver(largest, maximum, maximum);
    require(largest.snapshot().reservations.hostCapacityBytes ==
                MemoryBudgetLedger::usableByteBudgetForPhysicalMemory(maximum),
            "largest host counters do not overflow the available-memory percentage");
    auto finite = sample(now);
    finite.machine.availableBytes = 22 * gib;
    finite.process.domains.push_back({.path = "/ancestor",
                                      .kind = ProcessMemoryLimitKind::Finite,
                                      .limitBytes = 16 * gib,
                                      .currentBytes = 16 * gib - 32 * mib});
    GpuProcessingPhasePolicy ancestor;
    ancestor.applyHostSample(finite, 64 * gib, now);
    require(ancestor.snapshot().reservations.hostCapacityBytes == 32 * mib &&
                final(ancestor).status == GpuProcessingPhaseAdmission::BudgetRefused,
            "every finite process domain hard-caps otherwise ample live host capacity");
}
void liveHeadroomDuringCachePressure() {
    const auto now = GpuProcessingPhasePolicy::Clock::now();
    MemoryBudgetSample reading;
    reading.physicalBytes = 64 * gib;
    reading.machine.availableBytes = 22 * gib;
    reading.process.source = ProcessMemorySource::MacOsHostOnly;
    reading.process.status = ProcessMemoryStatus::Unsupported;
    reading.process.sampledAt = now;
    MemoryBudgetLedger ledger(reading.physicalBytes, *reading.machine.availableBytes);
    ledger.beginProcessSampling();
    auto delivered = ledger.applySample(reading, now);
    GpuProcessingPhasePolicy policy(2);
    policy.applyHostSample(delivered, reading.physicalBytes, now);
    require(!delivered.memoryPressure && delivered.retentionPercent == 100 &&
                delivered.reusableHostCreditBytes == 0 &&
                policy.snapshot().reservations.hostCapacityBytes == 22 * gib,
            "64GiB/22GiB preserves cache targets and exposes real processing headroom");
    auto cpu = reserveHostEvaluationPhase(policy, 1920ULL * 1080, gib,
                                          GpuProcessingPhaseOutput::FloatFinal);
    require(policy.reportDevice(1, {.source = GpuAllocationBudgetSource::MemoryBudget,
                                    .device_local_bytes = 8 * gib,
                                    .available_bytes = gib}),
            "live native owner supplies its separate device allowance");
    auto native = policy.reserve(preview(), now);
    require(cpu.lease && native.lease,
            "CPU final and native preview fit within shared measured headroom");
    require(policy.reserve(preview(), now).domain == GpuProcessingPhaseDomain::RequestCount,
            "positive host capacity does not bypass request-count bounds");
    const auto held = policy.snapshot().reservations;
    delivered.machine.availableBytes = 0;
    delivered.process.sampledAt = now + std::chrono::seconds(1);
    ++delivered.sampleSerial;
    policy.applyHostSample(delivered, reading.physicalBytes, delivered.process.sampledAt);
    require(policy.reserve(preview(), delivered.process.sampledAt).status ==
                    GpuProcessingPhaseAdmission::BudgetRefused &&
                policy.snapshot().reservations.totalHostClaimBytes == held.totalHostClaimBytes &&
                policy.snapshot().reservations.totalDeviceClaimBytes == held.totalDeviceClaimBytes,
            "measured zero refuses growth without crediting or retiring held claims");
    native.lease.reset();
    cpu.lease.reset();
    delivered.machine.availableBytes = 64 * gib;
    policy.applyHostSample(delivered, reading.physicalBytes, delivered.process.sampledAt);
    require(policy.snapshot().reservations.hostCapacityBytes == 0,
            "duplicate host-only delivery cannot grow capacity");
    ++delivered.sampleSerial;
    delivered.processSampleFresh = false;
    policy.applyHostSample(delivered, reading.physicalBytes, delivered.process.sampledAt);
    require(policy.snapshot().reservations.hostCapacityBytes == 0 &&
                !policy.snapshot().reservations.hostReady,
            "newer stale host-only delivery closes readiness without growing capacity");
    ++delivered.sampleSerial;
    delivered.processSampleFresh = true;
    delivered.process.sampledAt = now;
    policy.applyHostSample(delivered, reading.physicalBytes, now + std::chrono::seconds(2));
    require(policy.snapshot().reservations.hostCapacityBytes == 0 &&
                !policy.snapshot().reservations.hostReady,
            "regressed host-only timestamp cannot recover capacity despite a fresh label");
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
    require(grant.lease && policy.snapshot().reservations.hostCapacityBytes == 2 * gib,
            "paired 8GiB/6GiB process counters expose the actual2GiB remaining");
    require(grant.lease->hostBytes() == 1920ULL * 1080 * 8 + 4096,
            "source-free1080 minimum admits under previous1GiB cap");
    grant.lease.reset();
    auto pressure = sample(now + std::chrono::seconds(1));
    pressure.sampleSerial = 2;
    pressure.process.status = ProcessMemoryStatus::Partial;
    pressure.process.domains[0].limitBytes = 0;
    pressure.process.domains[0].currentBytes.reset();
    policy.applyHostSample(pressure, 60 * gib, pressure.process.sampledAt);
    require(policy.reserve(preview(), pressure.process.sampledAt).status ==
                GpuProcessingPhaseAdmission::BudgetRefused,
            "reliable zero limit refuses even when partial usage is missing");
    auto failure = sample(now + std::chrono::seconds(2));
    failure.sampleSerial = 3;
    failure.process.status = ProcessMemoryStatus::Unavailable;
    failure.process.domains.clear();
    policy.applyHostSample(failure, 60 * gib, failure.process.sampledAt);
    require(policy.snapshot().reservations.hostCapacityBytes == 0,
            "read failure cannot erase earlier finite cap");
    auto recovered = sample(now + std::chrono::seconds(3));
    recovered.sampleSerial = 4;
    policy.applyHostSample(recovered, 60 * gib, recovered.process.sampledAt);
    require(policy.reserve(preview(), now + std::chrono::seconds(14)).status ==
                GpuProcessingPhaseAdmission::AwaitingCapacity,
            "stale capacity closes new admission without polling");
    recovered = sample(now + std::chrono::seconds(15));
    recovered.sampleSerial = 5;
    policy.applyHostSample(recovered, 60 * gib, recovered.process.sampledAt);
    const auto activeNow = recovered.process.sampledAt;
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
void deviceReportRecoversAfterPressure() {
    const auto now = GpuProcessingPhasePolicy::Clock::now();
    GpuProcessingPhasePolicy policy;
    policy.applyHostSample(sample(now), 60 * gib, now);
    const GpuAllocationBudget zero{.source = GpuAllocationBudgetSource::MemoryBudget,
                                   .device_local_bytes = 8 * gib,
                                   .available_bytes = 0};
    require(policy.reportDevice(90, zero), "zero free device capacity is reported");
    require(policy.reserve(preview(), now).status == GpuProcessingPhaseAdmission::BudgetRefused,
            "a startup zero cannot admit a native phase");
    auto free = zero;
    free.available_bytes = gib;
    require(policy.reportDevice(90, free), "same owner refreshes its live free capacity");
    auto active = policy.reserve(preview(), now);
    require(active.lease != nullptr, "native phase recovers after a zero startup sample");
    free.available_bytes = 64 * 1024 * 1024;
    require(policy.reportDevice(90, free), "pressure refresh preserves the owner report slot");
    require(policy.reserve(preview(), now).status == GpuProcessingPhaseAdmission::BudgetRefused,
            "live free bytes do not credit held phase claims to new work");
    active.lease.reset();
    free.available_bytes = gib;
    require(policy.reportDevice(90, free) && policy.reserve(preview(), now).lease != nullptr,
            "proven release and recovered free capacity admit the next native phase");
}
void liveDeviceReportCreditsOnlyOwnedNativeBacking() {
    const auto now = GpuProcessingPhasePolicy::Clock::now();
    GpuProcessingPhasePolicy policy;
    policy.applyHostSample(sample(now), 60 * gib, now);
    GpuAllocationBudget report{.source = GpuAllocationBudgetSource::MemoryBudget,
                               .device_local_bytes = 8 * gib,
                               .available_bytes = gib};
    require(policy.reportDevice(91, report), "one native owner reports live free bytes");
    auto phase = policy.reserve(preview(), now);
    require(phase.lease != nullptr, "the native phase has a real reserved envelope");
    const auto held = policy.snapshot().reservations.totalDeviceClaimBytes;
    constexpr std::uint64_t backing = 16 * 1024 * 1024;
    auto image = NativeImageStorageToken::reserve(phase.lease, backing,
                                                  NativeImageStorageIdentity{91, 1, 0, true}, now);
    require(image.token.has_value() && image.token->promote(),
            "the phase publishes exact local native image ownership");
    phase.lease.reset();
    report.available_bytes -= backing;
    require(policy.reportDevice(91, report), "the owner refreshes VMA free after allocation");
    const auto after = policy.snapshot().reservations;
    require(after.ownedNativeImageBytes == backing && after.totalDeviceClaimBytes == backing &&
                after.deviceCapacityBytes == gib,
            "only allocated root ownership is credited against VMA free, not the old phase claim");
    require(held > backing && policy.reserve(preview(), now).lease != nullptr,
            "retained backing does not permanently double-charge the recovered free bytes");
    require(policy.reportDevice(92, report) &&
                policy.snapshot().reservations.deviceCapacityBytes == gib - backing,
            "multiple owners never receive unproven cross-device ownership credit");
    require(policy.retireDeviceReport(92, true) &&
                policy.snapshot().reservations.deviceCapacityBytes == gib,
            "retiring the second owner restores exact single-owner accounting");
    image.token.reset();
    report.available_bytes += backing;
    require(policy.reportDevice(91, report) &&
                policy.snapshot().reservations.totalDeviceClaimBytes == 0,
            "native release removes its exact claim before the next report");
}
void nativeSupplementDeadline() {
    const auto now = GpuProcessingPhasePolicy::Clock::now();
    GpuProcessingPhasePolicy policy;
    policy.applyHostSample(sample(now), 60 * gib, now);
    require(policy.reportDevice(11, {.source = GpuAllocationBudgetSource::MemoryBudget,
                                     .device_local_bytes = 8 * gib,
                                     .available_bytes = gib}),
            "deadline native quota delivered");
    auto phase = policy.reserve(preview(), now);
    require(phase.lease != nullptr, "deadline existing phase admitted");
    const NativeImageStorageIdentity identity{11, 1, 0, false};
    auto before =
        NativeImageStorageToken::reserve(phase.lease, 64, identity, now + std::chrono::seconds(9));
    require(before.token.has_value(), "new host supplement uses accepted delivered deadline");
    before.token.reset();
    auto duplicate = sample(now);
    duplicate.process.domains[0].currentBytes = 6 * gib + 1;
    policy.applyHostSample(duplicate, 60 * gib, now + std::chrono::seconds(9));
    const auto stale =
        NativeImageStorageToken::reserve(phase.lease, 64, identity, now + std::chrono::seconds(11));
    require(stale.status == GpuProcessingPhaseAdmission::AwaitingCapacity && !stale.token,
            "duplicate tightening cannot extend deadline or install unlimited freshness");
    const NativeImageStorageIdentity local{11, 0, 1, true};
    auto grandfathered =
        NativeImageStorageToken::reserve(phase.lease, 64, local, now + std::chrono::seconds(11));
    require(grandfathered.token.has_value(), "existing device-only conversion remains granted");
    grandfathered.token.reset();
    const auto closed = policy.snapshot().reservations.admissionRevision;
    auto fresh = sample(now + std::chrono::seconds(11));
    fresh.sampleSerial = 2;
    policy.applyHostSample(fresh, 60 * gib, fresh.process.sampledAt);
    require(policy.snapshot().reservations.admissionRevision > closed,
            "fresh production delivery reopens an expired supplemental host domain");
    auto recovered =
        NativeImageStorageToken::reserve(phase.lease, 64, identity, fresh.process.sampledAt);
    require(recovered.token.has_value(), "fresh same root admits supplemental host again");
    recovered.token.reset();
    phase.lease.reset();
    const auto final = policy.snapshot().reservations;
    require(final.totalHostClaimBytes == 0 && final.totalDeviceClaimBytes == 0 &&
                final.ownedNativeImages == 0 && final.admittedPhases == 0,
            "deadline witness leaves all phase and image domains empty");
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

void deliveredNumericView() {
    const auto now = GpuProcessingPhasePolicy::Clock::now();
    auto view = std::make_shared<TaskBudgetView>();
    GpuProcessingPhasePolicy policy(4, {}, view);
    require(!view->load(), "omitted default starts without delivered capacity");
    const MemoryBudgetAllocation configured{.usableByteBudget = 60 * gib,
                                            .operationCacheByteBudget = 18 * gib,
                                            .previewFrameCacheByteBudget = 12 * gib};
    auto delivered = sample(now);
    delivered.configuredBytes = 30 * gib;
    delivered.processingEffectiveBytes = 10 * gib;
    delivered.process.domains[0].limitBytes = 128 * gib;
    delivered.process.domains[0].currentBytes = 0;
    policy.applyHostSample(delivered, 60 * gib, configured, now);
    auto numeric = view->load();
    require(numeric && numeric->fresh(now) && numeric->aggregateGpuBytes == 60 * gib &&
                numeric->previewRequestBytes == 60 * gib &&
                numeric->assignedOperationBytes == 18 * gib &&
                numeric->assignedPreviewBytes == 12 * gib,
            "retention selections are descriptive, not hidden computation limits");
    auto held = policy.reserve({.pixels = 1, .sourceFree = true, .native = false}, now);
    require(held.lease != nullptr, "ordinary host phase admitted before headroom shrink");
    const auto charged = policy.snapshot().reservations;
    delivered.sampleSerial = 2;
    delivered.process.sampledAt = now + std::chrono::seconds(1);
    delivered.machine.availableBytes = 5 * gib;
    policy.applyHostSample(delivered, 60 * gib, configured, delivered.process.sampledAt);
    numeric = view->load();
    require(numeric && numeric->aggregateGpuBytes == 5 * gib &&
                numeric->previewRequestBytes == 5 * gib &&
                policy.snapshot().reservations.hostReservedBytes == charged.hostReservedBytes,
            "measured headroom shrink preserves admitted phase claims");
    delivered.sampleSerial = 3;
    delivered.process.sampledAt = now + std::chrono::seconds(2);
    delivered.machine.availableBytes = 20 * gib;
    policy.applyHostSample(delivered, 60 * gib, configured, delivered.process.sampledAt);
    numeric = view->load();
    require(numeric && numeric->aggregateGpuBytes == 20 * gib &&
                numeric->previewRequestBytes == 20 * gib,
            "fresh actual headroom is available without a percentage recovery ladder");
    auto duplicate = delivered;
    duplicate.process.sampledAt += std::chrono::seconds(5);
    duplicate.machine.availableBytes = 30 * gib;
    policy.applyHostSample(duplicate, 60 * gib, configured, duplicate.process.sampledAt);
    require(view->load() == numeric && !view->load()->fresh(now + std::chrono::seconds(13)),
            "duplicate serial cannot grow capacity or extend sample freshness");
    auto superseded = duplicate;
    superseded.sampleSerial = 1;
    policy.applyHostSample(superseded, 60 * gib, configured, delivered.process.sampledAt);
    require(view->load() == numeric, "older delivery cannot replace a newer budget snapshot");
    delivered.sampleSerial = 4;
    delivered.process.sampledAt = now + std::chrono::seconds(3);
    delivered.processSampleFresh = false;
    policy.applyHostSample(delivered, 60 * gib, configured, delivered.process.sampledAt);
    numeric = view->load();
    require(numeric && !numeric->fresh(delivered.process.sampledAt) &&
                policy.snapshot().reservations.hostReservedBytes == charged.hostReservedBytes,
            "stale sampling closes new admission without freeing live work");
    held.lease.reset();
    require(policy.snapshot().reservations.admittedPhases == 0,
            "held grant releases normally under readiness loss");
    auto tinyView = std::make_shared<TaskBudgetView>();
    GpuProcessingPhasePolicy tinyPolicy(4, {}, tinyView);
    const MemoryBudgetAllocation tiny{.usableByteBudget = 60 * gib,
                                      .operationCacheByteBudget = 128,
                                      .previewFrameCacheByteBudget = 64};
    auto tinySample = sample(now);
    tinySample.configuredBytes = 192;
    tinySample.processingEffectiveBytes = 192;
    tinyPolicy.applyHostSample(tinySample, 60 * gib, tiny, now);
    require(tinyView->load() && tinyView->load()->aggregateGpuBytes == 2 * gib &&
                tinyView->load()->previewRequestBytes == 2 * gib &&
                tinyView->load()->assignedOperationBytes == 128 &&
                tinyView->load()->assignedPreviewBytes == 64,
            "tiny explicit caches do not constrain rendering within actual headroom");
    auto emptyView = std::make_shared<TaskBudgetView>();
    GpuProcessingPhasePolicy replay(4, {}, emptyView);
    replay.applyHostSample(tinySample, 60 * gib, now);
    replay.applyHostSample(tinySample, 60 * gib, tiny, now);
    require(!emptyView->load(),
            "duplicate host delivery cannot seed an absent numeric view without a new sample");
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
        lowMemoryHostFinalAdmission();
        liveHeadroomDuringCachePressure();
        policy();
        deviceReportRecoversAfterPressure();
        liveDeviceReportCreditsOnlyOwnedNativeBacking();
        nativeSupplementDeadline();
        monotonicRecoveryAndSlots();
        malformedFallbackCannotRecover();
        verifiedConstraintRemoval();
        deliveredNumericView();
        envelope();
        std::cout << "sampled paired policy/preview+final sizing PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
