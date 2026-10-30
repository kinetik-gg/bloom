// Admission by actual capacity and actual need: pressure, waits and true exhaustion.
//
// Usable capacity is measured capacity minus the headroom preferences. A phase claims its need
// (fixed output, 3x its minimum source bytes, device working plus minimum executor bytes); its
// requested source/executor values are unclaimed ceilings. A need that fits the usable capacity
// but not what other work holds now waits (AwaitingCapacity) and is admitted on release. Only a
// need larger than the whole usable capacity is refused (BudgetRefused).
#include <bloom/render/native_image_storage.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace bloom::runtime;
using namespace bloom::render;
using Clock = GpuProcessingPhasePolicy::Clock;
constexpr std::uint64_t kMiB = 1024ULL * 1024;
constexpr std::uint64_t kGiB = 1024 * kMiB;

void require(const bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void deliverHost(GpuProcessingPhasePolicy& policy, const std::uint64_t available,
                 const std::uint64_t serial) {
    MemoryBudgetState sample;
    sample.machine.availableBytes = available;
    sample.process.status = ProcessMemoryStatus::Unsupported;
    sample.process.source = ProcessMemorySource::OtherHostOnly;
    sample.process.sampledAt = Clock::now();
    sample.processSampleFresh = true;
    sample.sampleSerial = serial;
    policy.applyHostSample(sample, 64 * kGiB, sample.process.sampledAt);
}

void reportDevice(GpuProcessingPhasePolicy& policy, const std::uint64_t epoch,
                  const std::uint64_t available) {
    require(policy.reportDevice(epoch, {.source = GpuAllocationBudgetSource::MemoryBudget,
                                        .device_local_bytes = 24 * kGiB,
                                        .available_bytes = available}),
            "device report accepted");
}

// One 2048x858 frame: float output and metadata on the host, one float frame of source, the
// executor's live-image peak on the device.
GpuProcessingPhaseEnvelope frame(const std::uint64_t deviceWorking = 64 * kMiB) {
    const std::uint64_t pixels = 2048ULL * 858;
    return {.output = GpuProcessingPhaseOutput::FloatFinal,
            .pixels = pixels,
            .metadataBytes = 16 * kMiB,
            .requestedSourceBytes = 64 * kGiB,
            .minimumSourceBytes = pixels * 16,
            .requestedExecutorBytes = 64 * kGiB,
            .minimumExecutorBytes = pixels * 16,
            .deviceWorkingBytes = deviceWorking};
}

void headroomIsTheOnlyMargin() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 2 * kGiB, .vramBytes = 2 * kGiB});
    deliverHost(policy, 20 * kGiB, 1);
    reportDevice(policy, 1, 22 * kGiB);
    auto usable = policy.usableCapacity();
    require(usable.hostReady && usable.hostUsableBytes == 18 * kGiB,
            "usable RAM is available memory minus the 2 GiB default headroom");
    require(usable.deviceReady && usable.deviceUsableBytes == 20 * kGiB,
            "usable VRAM is the live budget minus the 2 GiB default headroom");
    policy.setHeadroom({.ramBytes = 4 * kGiB, .vramBytes = 1 * kGiB});
    usable = policy.usableCapacity();
    require(usable.hostUsableBytes == 16 * kGiB && usable.deviceUsableBytes == 21 * kGiB,
            "a headroom preference change applies at once to both domains");
    policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
    require(policy.usableCapacity().hostUsableBytes == 20 * kGiB &&
                policy.usableCapacity().deviceUsableBytes == 22 * kGiB,
            "zero headroom exposes the whole measured capacity");
    deliverHost(policy, 10 * kGiB, 2);
    require(policy.usableCapacity().hostUsableBytes == 10 * kGiB,
            "the next measured sample replaces the capacity");
}

void phasesClaimOnlyTheirNeed() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 2 * kGiB, .vramBytes = 2 * kGiB});
    deliverHost(policy, 20 * kGiB, 1);
    reportDevice(policy, 1, 22 * kGiB);
    std::vector<GpuProcessingPhaseGrant> grants;
    // The desktop failure: Viewer, RAM fill and export frames together, each asking the whole
    // machine as a ceiling. Every one is admitted and the claims stay a small fraction.
    for (int index = 0; index < 8; ++index) {
        grants.push_back(policy.reserve(frame()));
        require(grants.back().status == GpuProcessingPhaseAdmission::Accepted,
                "concurrent 2048x858 frames are all admitted on a mostly free machine");
    }
    const auto usable = policy.usableCapacity();
    require(usable.hostClaimBytes < 3 * kGiB && usable.deviceClaimBytes < 1 * kGiB,
            "eight frames claim their need, not the whole machine");
    require(grants.front().lease->sourceByteLimit() > 1 * kGiB &&
                grants.front().lease->executorByteLimit() > 1 * kGiB,
            "the requested ceilings remain available to a phase without being claimed");
}

void contentionWaitsAndResumesOnRelease() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
    deliverHost(policy, 20 * kGiB, 1);
    // An artificially small device: room for two frames' device need, not three.
    const auto perFrameDevice = 64 * kMiB + 2048ULL * 858 * 16;
    reportDevice(policy, 1, 2 * perFrameDevice + perFrameDevice / 2);
    auto first = policy.reserve(frame());
    auto second = policy.reserve(frame());
    require(first.lease && second.lease, "two frames fit the small device");
    const auto seen = policy.usableCapacity().admissionRevision;
    auto third = policy.reserve(frame());
    require(third.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                third.domain == GpuProcessingPhaseDomain::Device && !third.lease,
            "a frame that fits the device but not the held claims waits, it is not refused");
    std::atomic_bool woke{false};
    std::thread waiter(
        [&] { woke = policy.waitForAdmissionChange(seen, {}, std::chrono::seconds(10)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    first.lease.reset();
    waiter.join();
    require(woke.load(), "a release wakes the waiting request");
    third = policy.reserve(frame());
    require(third.status == GpuProcessingPhaseAdmission::Accepted,
            "the waiting frame is admitted after the release");
    const auto start = Clock::now();
    require(!policy.waitForAdmissionChange(policy.usableCapacity().admissionRevision, {},
                                           std::chrono::milliseconds(100)) &&
                Clock::now() - start < std::chrono::seconds(2),
            "an unchanged root ends the wait at its bound so the caller re-checks cancellation");
}

void exhaustionIsRefusedWithReadableCounters() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 2 * kGiB, .vramBytes = 2 * kGiB});
    deliverHost(policy, 4 * kGiB, 1);
    reportDevice(policy, 1, 3 * kGiB);
    // Usable now: 2 GiB RAM, 1 GiB VRAM; the host ceiling is the 64 GiB machine minus headroom.
    // A 16384x16384 float frame (about 24 GiB of host need) fits the machine, so it waits for
    // memory other processes hold; a 32768x32768 frame can never fit and is refused.
    auto large = frame();
    large.pixels = 16384ULL * 16384;
    large.minimumSourceBytes = large.pixels * 16;
    large.minimumExecutorBytes = large.pixels * 16;
    const auto waits = policy.reserve(large);
    require(waits.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                waits.domain == GpuProcessingPhaseDomain::Host && !waits.lease,
            "a frame that fits the machine but not the memory free now waits");
    auto huge = large;
    huge.pixels = 32768ULL * 32768;
    huge.requestedSourceBytes = huge.pixels * 16;
    huge.minimumSourceBytes = huge.pixels * 16;
    huge.minimumExecutorBytes = huge.pixels * 16;
    const auto refused = policy.reserve(huge);
    require(refused.status == GpuProcessingPhaseAdmission::BudgetRefused &&
                refused.domain == GpuProcessingPhaseDomain::Host && !refused.lease,
            "a frame larger than the host ceiling is refused");
    auto deviceHuge = frame(2 * kGiB);
    const auto deviceRefused = policy.reserve(deviceHuge);
    require(deviceRefused.status == GpuProcessingPhaseAdmission::BudgetRefused &&
                deviceRefused.domain == GpuProcessingPhaseDomain::Device,
            "a device need larger than the whole usable device capacity is refused");
    const auto usable = policy.usableCapacity();
    require(usable.hostClaimBytes == 0 && usable.deviceClaimBytes == 0 &&
                usable.admittedPhases == 0,
            "a refusal leaves no claim behind");
    const auto text = describeAdmission(deviceRefused, usable);
    require(text.find("BudgetRefused") != std::string::npos &&
                text.find("domain=Device") != std::string::npos &&
                text.find("deviceUsable=1073741824") != std::string::npos &&
                text.find("phases=0") != std::string::npos,
            "the diagnostic names the status, the domain and the counters");
}

void manyConcurrentPhases() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 2 * kGiB, .vramBytes = 2 * kGiB});
    deliverHost(policy, 60 * kGiB, 1);
    reportDevice(policy, 1, 22 * kGiB);
    std::vector<std::shared_ptr<GpuProcessingPhaseLease>> leases;
    std::vector<std::thread> threads;
    std::mutex mutex;
    for (int index = 0; index < 32; ++index) {
        threads.emplace_back([&] {
            auto grant = policy.reserve(frame());
            const std::lock_guard lock(mutex);
            if (grant.lease)
                leases.push_back(std::move(grant.lease));
        });
    }
    for (auto& thread : threads)
        thread.join();
    require(leases.size() == 32, "32 concurrent frames are admitted from 32 threads");
    const auto overflow = policy.reserve(frame());
    require(overflow.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                overflow.domain == GpuProcessingPhaseDomain::RequestCount,
            "the request slots are a wait, not a refusal");
    leases.pop_back();
    require(policy.reserve(frame()).status == GpuProcessingPhaseAdmission::Accepted,
            "a released slot admits the next frame");
    leases.clear();
    require(policy.usableCapacity().hostClaimBytes == 0 &&
                policy.usableCapacity().deviceClaimBytes == 0,
            "every claim returns when the phases end");
}

void retentionIsEvictedBeforeWaiting() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
    deliverHost(policy, 1 * kGiB, 1);
    reportDevice(policy, 1, 22 * kGiB);
    std::atomic<std::uint64_t> asked{0};
    std::atomic<int> calls{0};
    policy.addRetentionReclaimer([&](const GpuProcessingPhaseDomain domain,
                                     const std::uint64_t bytes) -> std::uint64_t {
        require(domain == GpuProcessingPhaseDomain::Host, "host retention is asked for host room");
        asked = bytes;
        ++calls;
        return 0;
    });
    auto held = policy.reserve(frame());
    require(held.lease != nullptr, "the first frame fits");
    auto big = frame();
    big.minimumSourceBytes = 300 * kMiB;
    const auto free = policy.usableCapacity().hostFreeBytes;
    const auto waiting = policy.reserve(big);
    const auto need = policy.lastAwaitingNeed();
    require(waiting.status == GpuProcessingPhaseAdmission::AwaitingCapacity && calls == 1 &&
                need && need->domain == GpuProcessingPhaseDomain::Host &&
                need->bytes >= 3 * big.minimumSourceBytes && asked == need->bytes - free &&
                need->shortfallBytes == asked,
            "a waiting request first asks reusable retention to release its shortfall");
}

// The owner's stuck export: reusable caches filled the memory beyond the headroom (inside a
// 24 GB cgroup cap), and an export frame waited forever because nothing evicted them. With the
// ledger's evict-on-demand registered as the policy's reclaimer, the frame is admitted at the
// next sample instead of waiting.
void cachesYieldToWork() {
    for (const bool cgroupCapped : {false, true}) {
        MemoryBudgetLedger ledger(64 * kGiB, 64 * kGiB);
        ledger.setRamHeadroom(2 * kGiB);
        std::size_t cached = 0;
        ledger.registerCache(
            &cached, 64 * kGiB, [&] { return cached; },
            [&](const std::size_t bytes) { cached = std::min(cached, bytes); });
        // What the machine (or the 24 GB cgroup) has left once the caches hold `cached`.
        const std::uint64_t room = cgroupCapped ? 24 * kGiB : 60 * kGiB;
        const auto availableNow = [&] { return room - std::min<std::uint64_t>(room, cached); };
        cached = room - 3 * kGiB; // Caches filled to headroom plus margin plus a little.
        MemoryBudgetState state;
        state.machine.availableBytes = cgroupCapped ? 40 * kGiB : availableNow();
        if (cgroupCapped) {
            state.process.status = ProcessMemoryStatus::Finite;
            state.process.source = ProcessMemorySource::LinuxCgroupV2;
            state.process.domains = {{"/owner.scope", ProcessMemoryLimitKind::Finite, room,
                                      static_cast<std::size_t>(room - availableNow())}};
            state.process.reclaimableBytes = {std::size_t{0}};
        }
        static_cast<void>(ledger.poll(state.machine));
        GpuProcessingPhasePolicy policy(32);
        policy.setHeadroom({.ramBytes = 2 * kGiB, .vramBytes = 0});
        std::uint64_t serial = 1;
        const auto sample = [&] {
            if (cgroupCapped)
                state.process.domains.front().currentBytes =
                    static_cast<std::size_t>(room - availableNow());
            else
                state.machine.availableBytes = availableNow();
            state.process.sampledAt = Clock::now();
            state.processSampleFresh = true;
            state.sampleSerial = serial++;
            if (!cgroupCapped) {
                state.process.status = ProcessMemoryStatus::Unsupported;
                state.process.source = ProcessMemorySource::OtherHostOnly;
            }
            policy.applyHostSample(state, 64 * kGiB, state.process.sampledAt);
        };
        sample();
        reportDevice(policy, 1, 22 * kGiB);
        policy.addRetentionReclaimer(
            [&](const GpuProcessingPhaseDomain domain, const std::uint64_t shortfall) {
                return domain == GpuProcessingPhaseDomain::Host
                           ? ledger.reclaim(static_cast<std::size_t>(shortfall))
                           : std::uint64_t{0};
            });
        auto exportFrame = frame();
        exportFrame.admissionClass = GpuAdmissionClass::Export;
        exportFrame.minimumSourceBytes = 512 * kMiB; // A 4K float source.
        const auto before = cached;
        const auto first = policy.reserve(exportFrame);
        require(first.status == GpuProcessingPhaseAdmission::AwaitingCapacity && cached < before,
                cgroupCapped ? "capped: a frame that does not fit makes the caches give back"
                             : "a frame that does not fit makes the caches give back");
        sample(); // The fresh sample the reclaimer requests.
        const auto admitted = policy.reserve(exportFrame);
        require(admitted.status == GpuProcessingPhaseAdmission::Accepted,
                cgroupCapped ? "capped: the export frame is admitted at the next sample"
                             : "the export frame is admitted at the next sample");
        ledger.unregisterCache(&cached);
    }
}

void strictViewerPriority() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
    deliverHost(policy, 20 * kGiB, 1);
    const auto perFrameDevice = 64 * kMiB + 2048ULL * 858 * 16;
    reportDevice(policy, 1, 2 * perFrameDevice + perFrameDevice / 2);
    auto exportFrame = frame();
    exportFrame.admissionClass = GpuAdmissionClass::Export;
    const auto start = Clock::now();
    // Idle Viewer: an export starts at once, and a second export frame too.
    auto first = policy.reserve(exportFrame, start);
    auto second = policy.reserve(exportFrame, start);
    require(first.lease && second.lease, "with an idle Viewer exports use the whole root");
    // A Viewer frame now waits for device room; while it waits no export frame is granted.
    const auto viewerWaiting = policy.reserve(frame(), start);
    require(viewerWaiting.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                policy.viewerPriority(start).waiting,
            "the Viewer request waits for device room and is recorded as waiting");
    first.lease.reset();
    const auto yielded = policy.reserve(exportFrame, start);
    require(yielded.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                yielded.domain == GpuProcessingPhaseDomain::Viewer,
            "a released slot goes to the waiting Viewer, not to the next export frame");
    auto viewer = policy.reserve(frame(), start);
    require(viewer.lease && policy.viewerPriority(start).active &&
                !policy.viewerPriority(start).waiting,
            "the Viewer is admitted and becomes active");
    // While the Viewer is active an export frame must leave the Viewer's need free.
    second.lease.reset();
    const auto spare = policy.reserve(exportFrame, start);
    require(spare.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                spare.domain == GpuProcessingPhaseDomain::Viewer,
            "an active Viewer keeps its reserve; the export uses spare capacity only");
    // Once the Viewer goes idle (no Viewer frame within the activity window) exports resume.
    const auto idle =
        start + GpuProcessingPhasePolicy::kViewerActivityWindow + std::chrono::milliseconds(1);
    deliverHost(policy, 20 * kGiB, 2);
    auto resumed = policy.reserve(exportFrame, idle);
    require(resumed.status == GpuProcessingPhaseAdmission::Accepted,
            "an idle Viewer holds nothing: the export frame is admitted");
}

void deviceGrowthBeyondTheClaimUsesLiveRoom() {
    GpuProcessingPhaseBudget budget;
    const auto now = std::chrono::steady_clock::now();
    budget.setHostCapacity(1 * kGiB, true, now + std::chrono::seconds(10));
    budget.setDeviceCapacity(1000, true);
    auto grant = budget.reserve({.fixedHostBytes = 10,
                                 .deviceWorkingBytes = 40,
                                 .requestedExecutorBytes = 5000,
                                 .minimumExecutorBytes = 60,
                                 .sourceFree = true});
    require(grant.lease && grant.lease->deviceClaimBytes() == 100 &&
                grant.lease->executorByteLimit() == 960,
            "the phase claims working + minimum and keeps the live room as its ceiling");
    require(budget.snapshot().totalDeviceClaimBytes == 100, "only the need is claimed");
    const NativeImageStorageIdentity local{.allocatorGeneration = 7, .memoryType = 2, .heap = 1};
    auto image = NativeImageStorageToken::reserve(grant.lease, 600, local, now);
    require(image.token.has_value() && budget.snapshot().totalDeviceClaimBytes == 600,
            "an allocation beyond the claim is admitted against live room");
    auto other = budget.reserve({.fixedHostBytes = 10,
                                 .deviceWorkingBytes = 300,
                                 .minimumExecutorBytes = 0,
                                 .sourceFree = true});
    require(other.lease != nullptr, "another phase takes the remaining room");
    auto denied = NativeImageStorageToken::reserve(grant.lease, 200, local, now);
    require(!denied.token && denied.domain == GpuProcessingPhaseDomain::Device,
            "growth beyond the live room is refused at allocation");
    image.token.reset();
    other.lease.reset();
    grant.lease.reset();
    const auto empty = budget.snapshot();
    require(empty.totalDeviceClaimBytes == 0 && empty.deviceReservedBytes == 0 &&
                empty.admittedPhases == 0,
            "growth and claims are returned exactly");
}
} // namespace

// Preview width is bounded by concurrency and by a quarter of the free capacity, never by how
// much memory happens to be free: a nearly empty 64 GB machine gets at most eight frames.
void previewWidthIsBounded() {
    const std::uint64_t pixels = 2048ULL * 858;
    const std::uint64_t perFrameHost = pixels * (8 + 3 * 16) + 64 * kMiB;
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
    deliverHost(policy, 60 * kGiB, 1);
    reportDevice(policy, 1, 22 * kGiB);
    const auto ample = previewFramesThatFit(policy, pixels, 512 * kMiB);
    require(ample >= 1 && ample <= kMaximumPreviewFramesInFlight,
            "an ample machine still runs at most eight preview frames in flight");
    deliverHost(policy, 13 * perFrameHost, 2);
    require(previewFramesThatFit(policy, pixels, 512 * kMiB) <= 3,
            "a run sizes itself to a quarter of the free host capacity");
    deliverHost(policy, perFrameHost + perFrameHost / 2, 3);
    require(previewFramesThatFit(policy, pixels, 512 * kMiB) == 1,
            "one frame still runs when only one fits");
    deliverHost(policy, perFrameHost / 2, 4);
    require(previewFramesThatFit(policy, pixels, 512 * kMiB) == 0,
            "nothing runs when not even one frame fits");
}

int main() {
    try {
        headroomIsTheOnlyMargin();
        previewWidthIsBounded();
        phasesClaimOnlyTheirNeed();
        contentionWaitsAndResumesOnRelease();
        exhaustionIsRefusedWithReadableCounters();
        manyConcurrentPhases();
        strictViewerPriority();
        retentionIsEvictedBeforeWaiting();
        cachesYieldToWork();
        deviceGrowthBeyondTheClaimUsesLiveRoom();
    } catch (const std::exception& error) {
        std::cerr << "admission pressure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "admission pressure: all expectations passed\n";
    return EXIT_SUCCESS;
}
