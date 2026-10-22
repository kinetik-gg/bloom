#include <bloom/runtime/memory_budget_ledger.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using bloom::runtime::MemoryBudgetLedger;
constexpr std::size_t gib = std::size_t{1024} * 1024 * 1024;
int failures = 0;
void expect(bool value, std::string_view message) {
    if (!value) {
        std::cerr << message << '\n';
        ++failures;
    }
}
auto at(int seconds) {
    return MemoryBudgetLedger::Clock::time_point{} + std::chrono::seconds(seconds);
}

void selectedBudgetLifecycle() {
    MemoryBudgetLedger ledger(60 * gib, 60 * gib);
    std::size_t held = 5 * gib, operation = 0, preview = 0, auxiliary = 0, live = 0;
    ledger.setConfiguredTotal(30 * gib);
    ledger.registerCache(
        &operation, 18 * gib, [&] { return held; }, [&](std::size_t bytes) { operation = bytes; });
    ledger.registerCache(
        &preview, 12 * gib, [] { return 0; }, [&](std::size_t bytes) { preview = bytes; });
    ledger.registerCache(
        &auxiliary, 4 * gib, [] { return 0; }, [&](std::size_t bytes) { auxiliary = bytes; });
    ledger.registerCache(
        &live, 2 * gib, [] { return 0; }, [&](std::size_t bytes) { live = bytes; },
        bloom::runtime::MemoryBudgetOwnerKind::LiveWork);
    for (int seconds = 0; seconds != 60; ++seconds) {
        const auto state =
            ledger.poll({.availableBytes = seconds % 2 == 0 ? 0 : 40 * gib}, at(seconds));
        expect(state.effectiveBytes == 30 * gib && state.configuredBytes == 30 * gib &&
                   state.retentionPercent == 100 && !state.memoryPressure && !state.memoryNotice,
               "host availability never changes the selected reusable budget");
        expect(operation == 18 * gib && preview == 12 * gib && auxiliary == 4 * gib &&
                   live == 2 * gib,
               "auxiliary and live pools never normalize independent selected cache budgets");
    }
    std::size_t late = 0;
    ledger.registerCache(&late, gib, [] { return 0; }, [&](std::size_t bytes) { late = bytes; });
    expect(late == gib && operation == 18 * gib && preview == 12 * gib,
           "a newly registered pool receives its exact selection");
    const auto updatedLate = ledger.setCacheCeiling(&late, 80 * gib);
    expect(late == 80 * gib && updatedLate == late && operation == 18 * gib,
           "changing a pool target applies that exact value without shrinking other pools");
    ledger.unregisterCache(&late);
    ledger.unregisterCache(&operation);
    ledger.unregisterCache(&preview);
    ledger.unregisterCache(&auxiliary);
    ledger.unregisterCache(&live);
    expect(ledger.cacheByteBudget(&operation) == 0, "removed participants cannot be called again");
}

void swapActivityPolicy() {
    constexpr auto ample = std::size_t{40} * gib;
    const auto swapSample = [ample](const std::size_t used) {
        return bloom::runtime::MachineMemorySample{
            .availableBytes = ample, .swapTotalBytes = 4 * gib, .swapUsedBytes = used};
    };
    MemoryBudgetLedger ledger(60 * gib, 60 * gib);
    ledger.setConfiguredTotal(16 * gib);
    // A swap file that was filled long ago and no longer changes is not ongoing thrashing, however
    // full it is; the first reading is only a baseline.
    auto state = ledger.poll(swapSample(4 * gib * 9 / 10), at(0));
    expect(!state.swapPressure && !state.memoryPressure && state.retentionPercent == 100,
           "a first static swap reading is a baseline, not pressure");
    state = ledger.poll(swapSample(4 * gib * 9 / 10), at(5));
    expect(!state.swapPressure && state.effectiveBytes == 16 * gib && state.retentionPercent == 100,
           "static 90 percent swap with ample RAM never trims the caches");
    // Active growth is the signal, even while available memory looks healthy.
    state = ledger.poll(swapSample(4 * gib * 9 / 10 + gib), at(10));
    expect(state.swapPressure && !state.memoryPressure && state.swapNotice && !state.memoryNotice &&
               state.retentionPercent == 100,
           "actively rising swap is visible without changing cache targets");
    state = ledger.poll(swapSample(4 * gib * 9 / 10 + 2 * gib), at(15));
    expect(state.retentionPercent == 100 && !state.swapNotice,
           "persistent swap growth never adds a retention multiplier");
    // Once growth stops, activity clears. High static occupancy never changes the budget.
    state = ledger.poll(swapSample(4 * gib * 9 / 10 + 2 * gib), at(20));
    expect(!state.swapPressure && state.retentionPercent == 100,
           "a static reading clears swap activity without changing retention");
    state = ledger.poll(swapSample(4 * gib * 9 / 10 + 2 * gib), at(29));
    expect(state.retentionPercent == 100, "swap activity never changes the selected budget");
    state = ledger.poll(swapSample(4 * gib * 9 / 10 + 2 * gib), at(30));
    expect(state.retentionPercent == 100, "ample RAM leaves the selection unchanged");
    state = ledger.poll(swapSample(4 * gib * 9 / 10 + 2 * gib), at(40));
    expect(state.retentionPercent == 100, "later samples leave full selected admission");
    state = ledger.poll(swapSample(4 * gib * 9 / 10 + 2 * gib), at(50));
    expect(state.retentionPercent == 100, "retention remains at the selected allowance");
    // Releasing swap rebaselines, so a later rise is measured from the new low.
    state = ledger.poll(swapSample(gib), at(60));
    expect(!state.swapPressure, "a released sample rebaselines without pressure");
    state = ledger.poll(swapSample(gib + std::size_t{4} * 1024 * 1024), at(65));
    expect(!state.swapPressure, "small background churn below the entry threshold is ignored");
    // Clock rollback and a first sample are deterministic and never manufacture growth.
    state = ledger.poll(swapSample(2 * gib), at(60));
    expect(!state.swapPressure, "a rolled-back timestamp establishes a fresh baseline");
    state = ledger.poll(swapSample(3 * gib), at(70));
    expect(state.swapPressure, "real growth from the rebaselined sample is still detected");
    // Bounded hysteresis: a smaller sustained rise keeps an active episode latched, while a static
    // reading releases it.
    state = ledger.poll(swapSample(3 * gib + std::size_t{8} * 1024 * 1024), at(75));
    expect(state.swapPressure, "a smaller sustained rise keeps an active episode latched");
    state = ledger.poll(swapSample(3 * gib + std::size_t{8} * 1024 * 1024), at(80));
    expect(!state.swapPressure, "a static reading releases the latched episode");
    // Missing counters cannot assert recovery from a live episode, and a returning valid sample is
    // a fresh baseline rather than a spike measured against the intervening gap.
    state = ledger.poll(swapSample(4 * gib), at(85));
    expect(state.swapPressure, "a further rise keeps the episode active");
    state = ledger.poll({.availableBytes = ample}, at(90));
    expect(state.swapPressure && !state.memoryPressure,
           "missing swap counters cannot assert recovery");
    state = ledger.poll(swapSample(4 * gib), at(95));
    expect(!state.swapPressure, "a valid sample after missing counters is a fresh baseline");
}

void selectedTotalsAndHardwareDefaults() {
    MemoryBudgetLedger small(16 * gib, 16 * gib);
    expect(small.state().effectiveBytes == 8 * gib &&
               small.state().processingEffectiveBytes == 16 * gib,
           "automatic retention selection and independent processing capacity remain distinct");
    small.setConfiguredTotal(std::numeric_limits<std::size_t>::max());
    expect(small.state().effectiveBytes == std::numeric_limits<std::size_t>::max() &&
               small.state().processingEffectiveBytes == 16 * gib,
           "explicit cache selections never receive a hidden physical clamp or alter processing");
    std::size_t tiny = 0;
    small.registerCache(&tiny, 1024, [] { return 0; }, [&](std::size_t bytes) { tiny = bytes; });
    small.setConfiguredTotal(1024);
    auto state = small.poll({.availableBytes = 0}, at(0));
    expect(state.effectiveBytes == 1024 && tiny == 1024,
           "automatic floors never enlarge an explicit small selection");

    MemoryBudgetLedger unresolved;
    const auto fallback = unresolved.allocate();
    auto sample = bloom::runtime::MemoryBudgetSample{.machine = {.availableBytes = gib},
                                                     .physicalBytes = 60 * gib};
    state = unresolved.applySample(sample, at(0));
    const auto selected = unresolved.allocate();
    expect(selected.operationCacheByteBudget == 18 * gib &&
               selected.previewFrameCacheByteBudget == 12 * gib && selected != fallback,
           "the first verified hardware sample resolves automatic preferences");
    sample.machine.availableBytes = 0;
    sample.physicalBytes = 64 * gib;
    state = unresolved.applySample(sample, at(5));
    const auto later = unresolved.allocate();
    expect(
        later.operationCacheByteBudget == selected.operationCacheByteBudget &&
            later.previewFrameCacheByteBudget == selected.previewFrameCacheByteBudget &&
            state.processingEffectiveBytes == 64 * gib,
        "resolved defaults stay stable while current hardware capacity remains separately visible");

    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    MemoryBudgetLedger huge(60 * gib, 60 * gib);
    std::size_t left = 0, right = 0;
    huge.registerCache(
        &left, maximum, [] { return maximum; }, [&](std::size_t limit) { left = limit; });
    huge.registerCache(
        &right, maximum, [] { return maximum; }, [&](std::size_t limit) { right = limit; });
    const auto saturated = huge.poll({.availableBytes = maximum}, at(0));
    expect(left == maximum && right == maximum && saturated.retainedBytes == maximum,
           "unconstrained targets remain exact and retained gauges saturate without overflow");
}

using namespace bloom::runtime;
MemoryBudgetSample processSample(const std::size_t limit, const std::optional<std::size_t> used,
                                 const int seconds,
                                 const ProcessMemoryStatus status = ProcessMemoryStatus::Finite) {
    return {.machine = {.availableBytes = 60 * gib},
            .physicalBytes = 60 * gib,
            .process = {.source = ProcessMemorySource::LinuxCgroupV2,
                        .status = status,
                        .sampledAt = at(seconds),
                        .membershipPath = "/test",
                        .domains = {{"/test", ProcessMemoryLimitKind::Finite, limit, used}}}};
}

void processAdmissionAndFailures() {
    MemoryBudgetLedger legacy(60 * gib, 60 * gib);
    const auto legacyState = legacy.applySample(processSample(0, 0, 0), at(0));
    expect(legacyState.effectiveBytes == 30 * gib && !legacyState.processCacheCeilingBytes &&
               !legacyState.awaitingProcessSample,
           "unactivated standalone policy ignores process constraints until explicit activation");
    MemoryBudgetLedger ledger(60 * gib, 60 * gib);
    std::size_t cache = 0, live = 0;
    ledger.beginProcessSampling();
    ledger.registerCache(
        &cache, 20 * gib, [] { return 0; }, [&](const std::size_t value) { cache = value; });
    ledger.registerCache(
        &live, gib, [] { return 0; }, [&](const std::size_t value) { live = value; },
        MemoryBudgetOwnerKind::LiveWork);
    expect(cache == 0 && live > 0 && ledger.allocate().usableByteBudget > 0,
           "cold process sampling gates reusable retention only, not live work or processing");
    ledger.setConfiguredTotal(40 * gib);
    auto state = ledger.applySample(processSample(8 * gib, 6 * gib, 0), at(0));
    expect(state.effectiveBytes == 8 * gib && state.processCacheCeilingBytes == 8 * gib &&
               state.reusableHostCreditBytes == 0 && !state.awaitingProcessSample,
           "first finite sample applies the real OS limit without a reserve or hidden haircut");
    const auto requested = state.configuredBytes;
    state = ledger.applySample(processSample(16 * gib, {}, 1, ProcessMemoryStatus::Partial), at(1));
    expect(state.processCacheCeilingBytes == 8 * gib && cache == 8 * gib,
           "an incomplete larger finite limit cannot authorize expanded reusable retention");
    auto partial =
        processSample(std::size_t{256} * 1024 * 1024, {}, 2, ProcessMemoryStatus::Partial);
    state = ledger.applySample(partial, at(2));
    expect(state.effectiveBytes == std::size_t{256} * 1024 * 1024 &&
               state.configuredBytes == requested,
           "partial tighter OS limit applies immediately while selected preferences persist");
    state = ledger.applySample(processSample(0, {}, 3, ProcessMemoryStatus::Partial), at(3));
    expect(state.effectiveBytes == 0 && live > 0,
           "finite zero with unread usage defeats the floor");
    auto unknown = processSample(8 * gib, {}, 20, ProcessMemoryStatus::Unavailable);
    unknown.process.domains.clear();
    state = ledger.applySample(unknown, at(20));
    expect(state.effectiveBytes == 0 && state.processCacheCeilingBytes == 0,
           "failed sampling cannot expand a previously finite zero cap");
    unknown.process.status = ProcessMemoryStatus::Unsupported;
    unknown.process.source = ProcessMemorySource::LinuxCgroupV1Unsupported;
    unknown.process.sampledAt = at(30);
    state = ledger.applySample(unknown, at(30));
    expect(state.effectiveBytes == 0 && state.processCacheCeilingBytes == 0,
           "unsupported fallback never erases prior finite protection");
    unknown.process.status = ProcessMemoryStatus::NoFiniteVisibleLimit;
    unknown.process.source = ProcessMemorySource::LinuxCgroupV2;
    unknown.process.sampledAt = at(35);
    state = ledger.applySample(unknown, at(35));
    expect(state.effectiveBytes == 0 && state.processCacheCeilingBytes == 0,
           "later no-finite-visible topology cannot silently discard prior finite constraint");
    state = ledger.applySample(processSample(8 * gib, 2 * gib, 0), at(40));
    expect(state.effectiveBytes == 0 && !state.processSampleFresh,
           "stale snapshot cannot authorize growth and freshness remains explicit");
    state = ledger.applySample(processSample(8 * gib, 2 * gib, 50), at(50));
    expect(state.effectiveBytes == 8 * gib,
           "a fresh complete finite limit restores its actual capacity without a recovery delay");
    const auto before = state.effectiveBytes;
    state = ledger.applySample(processSample(before - before / 20, gib, 51), at(51));
    expect(state.effectiveBytes == before - before / 20,
           "even a five-percent newly hard shrink applies without cooldown or hysteresis");
    ledger.unregisterCache(&cache);
    ledger.unregisterCache(&live);

    MemoryBudgetLedger fallback(60 * gib, 60 * gib);
    fallback.beginProcessSampling();
    unknown.process.status = ProcessMemoryStatus::Unsupported;
    unknown.process.source = ProcessMemorySource::LinuxCgroupV1Unsupported;
    unknown.process.sampledAt = at(0);
    state = fallback.applySample(unknown, at(0));
    expect(state.effectiveBytes > 0 && !state.processCacheCeilingBytes &&
               state.process.status == ProcessMemoryStatus::Unsupported,
           "initial explicit unsupported sample activates honest host-only best effort");
}

bloom::runtime::ProcessMemoryView& checkedView(bloom::runtime::ProcessMemorySample& sample) {
    if (!sample.view)
        throw std::runtime_error("fixture view missing");
    return *sample.view;
}
void verifiedConstraintRemovalRecovery() {
    using namespace bloom::runtime;
    const auto trusted = [](const int seconds) {
        auto result = processSample(2 * gib, 0, seconds);
        result.process.view =
            ProcessMemoryView{.mountRoot = "/",
                              .mountPoint = "/",
                              .filesystemDevice = "0:32",
                              .mountId = 36,
                              .namespaces = {.cgroup = ProcessMemoryNamespaceIdentity{1, 2},
                                             .mount = ProcessMemoryNamespaceIdentity{1, 3}}};
        return result;
    };
#if defined(__linux__)
    auto root = trusted(0);
    root.process = parseLinuxProcessMemorySample(
        "0::/\n", "36 25 0:32 / /cgroup rw - cgroup2 cgroup rw\n",
        [](const std::filesystem::path&) {
            return ProcessMemoryFileRead{ProcessMemoryReadStatus::Missing, {}};
        },
        at(0),
        {.cgroup = ProcessMemoryNamespaceIdentity{1, 2},
         .mount = ProcessMemoryNamespaceIdentity{1, 3}});
    MemoryBudgetLedger rootLedger(60 * gib, 60 * gib);
    rootLedger.beginProcessSampling();
    const auto rootState = rootLedger.applySample(root, at(0));
    expect(!rootState.awaitingProcessSample && rootState.effectiveBytes > 0,
           "parser-proven cold hierarchy root can admit reusable retention");
#endif
    MemoryBudgetLedger ledger(60 * gib, 60 * gib);
    ledger.beginProcessSampling();
    auto state = ledger.applySample(trusted(0), at(0));
    const auto prior = state.effectiveBytes;
    const auto cap = state.processCacheCeilingBytes;
    const auto requested = state.configuredBytes;
    auto unlimited = trusted(1);
    unlimited.process.status = ProcessMemoryStatus::NoFiniteVisibleLimit;
    unlimited.process.domains[0].kind = ProcessMemoryLimitKind::Unlimited;
    unlimited.process.domains[0].limitBytes = 0;
    auto wrongView = unlimited;
    checkedView(wrongView.process).namespaces.cgroup = ProcessMemoryNamespaceIdentity{1, 4};
    state = ledger.applySample(wrongView, at(1));
    expect(state.processCacheCeilingBytes == cap && state.effectiveBytes == prior,
           "same paths under a changed cgroup namespace cannot release retention ceiling");
    wrongView = unlimited;
    wrongView.process.sampledAt = at(2);
    checkedView(wrongView.process).mountId += 1;
    state = ledger.applySample(wrongView, at(2));
    expect(state.processCacheCeilingBytes == cap && state.effectiveBytes == prior,
           "changed mount cannot authorize retention growth");
    wrongView = unlimited;
    wrongView.process.sampledAt = at(3);
    checkedView(wrongView.process).namespaces.mount.reset();
    state = ledger.applySample(wrongView, at(3));
    expect(state.processCacheCeilingBytes == cap, "missing namespace verification stays latched");
    wrongView = unlimited;
    wrongView.process.sampledAt = at(4);
    wrongView.process.domains.clear();
    state = ledger.applySample(wrongView, at(4));
    expect(state.processCacheCeilingBytes == cap, "missing observed domain stays latched");
    state = ledger.applySample(unlimited, at(5));
    expect(state.processCacheCeilingBytes == cap && !state.processSampleFresh,
           "superseded otherwise-valid unlimited proof cannot release constraint");
    unlimited.process.sampledAt = at(6);
    state = ledger.applySample(unlimited, at(30));
    expect(state.processCacheCeilingBytes == cap && state.effectiveBytes == prior,
           "expired valid unlimited proof cannot release constraint");
    unlimited.process.sampledAt = at(31);
    state = ledger.applySample(unlimited, at(31));
    expect(
        !state.processCacheCeilingBytes && state.effectiveBytes == requested &&
            state.configuredBytes == requested,
        "fresh same-view explicit max removes the constraint without changing selected settings");
    unlimited.process.sampledAt = at(40);
    state = ledger.applySample(unlimited, at(40));
    expect(state.effectiveBytes == requested &&
               state.effectiveBytes < state.processingEffectiveBytes,
           "later samples preserve the full selected target without a recovery ladder");
    auto failed = unlimited;
    failed.process.status = ProcessMemoryStatus::Partial;
    failed.process.sampledAt = at(50);
    failed.process.domains[0].currentBytes.reset();
    const auto recovered = state.effectiveBytes;
    state = ledger.applySample(failed, at(50));
    expect(state.effectiveBytes == recovered,
           "failed later reads cannot change an already verified selection");
}

void processHierarchyAndStableFill() {
    MemoryBudgetLedger hierarchy(60 * gib, 60 * gib);
    hierarchy.beginProcessSampling();
    auto sample = processSample(8 * gib, 2 * gib, 0);
    sample.process.domains.push_back(
        {"/parent", ProcessMemoryLimitKind::Finite, 12 * gib, 11 * gib});
    auto state = hierarchy.applySample(sample, at(0));
    expect(state.effectiveBytes == 8 * gib && state.processCacheCeilingBytes == 8 * gib,
           "the smallest actual ancestor or leaf limit constrains reusable retention");
    sample.process.domains[1].limitBytes = 4 * gib;
    sample.process.sampledAt = at(1);
    state = hierarchy.applySample(sample, at(1));
    expect(state.effectiveBytes == 4 * gib && state.processCacheCeilingBytes == 4 * gib,
           "a smaller finite ancestor wins independently of its sampled usage");
    for (const auto limit : {std::size_t{0}, std::size_t{1024}, std::size_t{2} * gib}) {
        MemoryBudgetLedger tiny(60 * gib, 60 * gib);
        tiny.beginProcessSampling();
        const auto result = tiny.applySample(processSample(limit, 0, 0), at(0));
        expect(result.effectiveBytes == limit,
               "tiny actual process limits never inherit an automatic floor or reserve");
    }
    MemoryBudgetLedger ledger(60 * gib, 60 * gib);
    ledger.beginProcessSampling();
    std::size_t held = 0, allowance = 0;
    unsigned evictions = 0;
    ledger.registerCache(
        &held, 6 * gib, [&] { return held; },
        [&](const std::size_t value) {
            allowance = value;
            if (held > value) {
                held = value;
                ++evictions;
            }
        });
    std::size_t previous = 0;
    unsigned stable = 0;
    for (int index = 0; index < 40; ++index) {
        state =
            ledger.applySample(processSample(8 * gib, 2 * gib + held, index * 10), at(index * 10));
        held = allowance; // A real consumer fills its current allowance between samples.
        stable = held == previous ? stable + 1 : 0;
        previous = held;
    }
    expect(held == 6 * gib && stable >= 30 && evictions == 0 && state.reusableHostCreditBytes == 0,
           "filling caches does not shrink their selected targets or invent reclaimable credit");
    const auto settled = held;
    state = ledger.applySample(processSample(8 * gib, 3 * gib + held, 400), at(400));
    expect(held == settled && evictions == 0,
           "usage changes leave retained targets stable; live phase admission checks headroom");
    state = ledger.applySample(processSample(4 * gib, 3 * gib + held, 401), at(401));
    expect(held == 4 * gib && evictions == 1 &&
               state.effectiveBytes <= state.processCacheCeilingBytes,
           "an actual lower OS limit immediately trims reusable retention");
    const auto afterPressure = evictions;
    for (int index = 41; index < 80; ++index) {
        state =
            ledger.applySample(processSample(8 * gib, 2 * gib + held, index * 10), at(index * 10));
        held = allowance;
    }
    expect(held > 0 && evictions == afterPressure,
           "a raised actual OS limit restores the selection without an alternating eviction loop");
    ledger.unregisterCache(&held);
    MemoryBudgetLedger shares(60 * gib, 60 * gib);
    shares.beginProcessSampling();
    std::size_t first = 0, second = 0;
    shares.registerCache(
        &first, 18 * gib, [] { return 20 * gib; }, [&](const std::size_t bytes) { first = bytes; });
    shares.registerCache(
        &second, 12 * gib, [] { return 20 * gib; },
        [&](const std::size_t bytes) { second = bytes; });
    state = shares.applySample(processSample(8 * gib, 6 * gib, 0), at(0));
    expect(first + second <= state.effectiveBytes && first > second &&
               state.retainedBytes == 40 * gib && state.reusableHostCreditBytes == 0,
           "reusable owners share one real OS limit without crediting shared retained gauges");
    std::size_t auxiliary = 0;
    shares.registerCache(
        &auxiliary, 10 * gib, [] { return 0; },
        [&](const std::size_t bytes) { auxiliary = bytes; });
    expect(
        first + second + auxiliary <= 8 * gib && first < 18 * gib && second < 12 * gib,
        "registration immediately reapplies one actual OS constraint across every reusable pool");
    std::size_t live = 0;
    shares.registerCache(
        &live, 20 * gib, [] { return 0; }, [&](const std::size_t bytes) { live = bytes; },
        MemoryBudgetOwnerKind::LiveWork);
    expect(live == 20 * gib && first + second + auxiliary <= 8 * gib,
           "live work stays separately admitted and never reduces reusable shares");
    static_cast<void>(shares.setCacheCeiling(&first, 6 * gib));
    expect(first + second + auxiliary <= 8 * gib && first < second,
           "a target change immediately updates every share of the actual OS constraint");
    shares.unregisterCache(&auxiliary);
    expect(first + second <= 8 * gib,
           "removing a constrained participant restores other shares inside the same OS limit");
    shares.unregisterCache(&live);
    shares.unregisterCache(&first);
    shares.unregisterCache(&second);

    MemoryBudgetLedger extreme(60 * gib, 60 * gib);
    extreme.beginProcessSampling();
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    std::size_t left = 0, right = 0;
    extreme.registerCache(
        &left, maximum, [] { return 0; }, [&](const std::size_t bytes) { left = bytes; });
    extreme.registerCache(
        &right, maximum, [] { return 0; }, [&](const std::size_t bytes) { right = bytes; });
    static_cast<void>(extreme.applySample(processSample(maximum, 0, 0), at(0)));
    expect(left <= maximum - right,
           "overflow-sized selected targets still share an actual finite OS limit exactly");
}

int liveProcessProbe() {
#if defined(__linux__)
    for (const auto* const name : {"/proc/self/cgroup", "/proc/self/mountinfo"}) {
        std::ifstream input(name);
        std::string line;
        std::size_t bytes = 0;
        while (std::getline(input, line) && bytes <= std::size_t{1024} * 1024) {
            bytes += line.size();
            if (std::string_view(name).ends_with("cgroup") ||
                line.find(" - cgroup2 ") != std::string::npos)
                std::cout << "raw " << name << ' ' << line << '\n';
        }
    }
    MemoryBudgetLedger ledger;
    ledger.beginProcessSampling();
    auto before = memoryBudgetSample();
    expect(before.process.source == ProcessMemorySource::LinuxCgroupV2 &&
               before.process.status == ProcessMemoryStatus::Finite,
           "live private helper discovers finite Linux cgroup v2");
    auto state = ledger.applySample(before);
    ledger.setConfiguredTotal(8 * gib);
    const auto print = [&ledger](const char* phase, const MemoryBudgetSample& snapshot) {
        const auto current = ledger.state();
        std::cout << phase << " source=" << static_cast<int>(snapshot.process.source)
                  << " status=" << static_cast<int>(snapshot.process.status)
                  << " membership=" << snapshot.process.membershipPath
                  << " requested=" << current.configuredBytes
                  << " effective=" << current.effectiveBytes
                  << " process_ceiling=" << current.processCacheCeilingBytes.value_or(0)
                  << " credit=" << current.reusableHostCreditBytes << '\n';
        for (const auto& domain : snapshot.process.domains)
            std::cout << phase << " domain=" << domain.path
                      << " kind=" << static_cast<int>(domain.kind) << " limit=" << domain.limitBytes
                      << " current=" << domain.currentBytes.value_or(0)
                      << " usage_known=" << domain.currentBytes.has_value() << '\n';
    };
    print("before", before);
    constexpr auto touchedBytes = std::size_t{16} * 1024 * 1024;
    const std::vector<std::byte> touched(touchedBytes, std::byte{0x5a});
    std::cout << "touched=" << touched.size()
              << " first=" << std::to_integer<unsigned>(touched.front())
              << " last=" << std::to_integer<unsigned>(touched.back()) << '\n';
    const auto after = memoryBudgetSample();
    state = ledger.applySample(after);
    print("after", after);
    const auto own = std::find_if(before.process.domains.begin(), before.process.domains.end(),
                                  [](const auto& domain) {
                                      return domain.kind == ProcessMemoryLimitKind::Finite &&
                                             domain.limitBytes == std::size_t{256} * 1024 * 1024;
                                  });
    expect(own != before.process.domains.end() && own->currentBytes.has_value(),
           "private helper finds its actual matched 256MiB domain");
    if (own != before.process.domains.end() && own->currentBytes.has_value()) {
        const auto usageBefore = own->currentBytes.value_or(0);
        const auto updated =
            std::find_if(after.process.domains.begin(), after.process.domains.end(),
                         [&](const auto& domain) { return domain.path == own->path; });
        expect(updated != after.process.domains.end() && updated->currentBytes &&
                   *updated->currentBytes > usageBefore,
               "touched allocation appears in the same measured domain usage");
    }
    expect(state.configuredBytes == 8 * gib &&
               state.effectiveBytes <= std::size_t{256} * 1024 * 1024 &&
               minimumProcessCacheAllowance(after.process, 0) <=
                   minimumProcessCacheAllowance(before.process, 0),
           "the actual finite domain constrains the target while selected preferences remain "
           "unchanged");
    return failures == 0 ? 0 : 1;
#else
    return 77;
#endif
}

} // namespace

int main(int argc, char** argv) try {
    if (argc == 2 && std::string_view(argv[1]) == "--live-process-probe")
        return liveProcessProbe();
    selectedBudgetLifecycle();
    swapActivityPolicy();
    selectedTotalsAndHardwareDefaults();
    processAdmissionAndFailures();
    verifiedConstraintRemovalRecovery();
    processHierarchyAndStableFill();
    return failures == 0 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
