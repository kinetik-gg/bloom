#include <bloom/runtime/memory_budget_ledger.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
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

void sequence() {
    MemoryBudgetLedger ledger(60 * gib, 60 * gib);
    std::size_t held = 5 * gib, operation = 0, preview = 0;
    ledger.registerCache(
        &operation, 18 * gib, [&] { return held; }, [&](std::size_t bytes) { operation = bytes; });
    ledger.registerCache(
        &preview, 12 * gib, [] { return 0; }, [&](std::size_t bytes) { preview = bytes; });
    auto state = ledger.poll({.availableBytes = 20 * gib}, at(0));
    expect(state.effectiveBytes == 20 * gib && state.configuredBytes == 30 * gib,
           "cap includes held bytes and follows current contention");
    expect(state.retentionPercent == 25 && state.memoryNotice && !state.swapNotice,
           "first pressure poll reduces admission to 25 percent and reports once");
    expect(operation == 3 * gib && preview == 2 * gib, "all shares scale proportionally");
    held = 0;
    state = ledger.poll({.availableBytes = 10 * gib}, at(1));
    expect(state.effectiveBytes == 20 * gib && state.retentionPercent == 10 && !state.memoryNotice,
           "cap cooldown is five seconds; second pressure poll reaches 10 percent");
    state = ledger.poll({.availableBytes = 45 * gib / 2}, at(5));
    expect(state.effectiveBytes == 20 * gib, "an exact ten percent change stays inside hysteresis");
    state = ledger.poll({.availableBytes = 23 * gib}, at(5));
    expect(state.effectiveBytes == 20 * gib, "an eight percent difference is inside hysteresis");
    state = ledger.poll({.availableBytes = 10 * gib}, at(6));
    expect(state.effectiveBytes == 8 * gib && state.retentionPercent == 10,
           "a larger drop applies after the cooldown");
    expect(operation + preview <= state.effectiveBytes / 10,
           "all callback allowances remain inside the eviction target");
    state = ledger.poll({}, at(7));
    expect(state.memoryPressure && state.retentionPercent == 10,
           "missing availability cannot assert recovery");
    state = ledger.poll({.availableBytes = 40 * gib}, at(10));
    expect(!state.memoryPressure && state.retentionPercent == 10 && state.effectiveBytes == 8 * gib,
           "recovery starts a ten-second hold before restoring budgets");
    state = ledger.poll({.availableBytes = 40 * gib}, at(19));
    expect(state.retentionPercent == 10 && state.effectiveBytes == 8 * gib, "no early restore");
    state = ledger.poll({.availableBytes = 40 * gib}, at(20));
    expect(state.retentionPercent == 25 && state.effectiveBytes == 10 * gib, "first recovery step");
    state = ledger.poll({.availableBytes = 40 * gib}, at(30));
    expect(state.retentionPercent == 50 && state.effectiveBytes == 25 * gib / 2,
           "second recovery step");
    state = ledger.poll({.availableBytes = 40 * gib}, at(40));
    expect(state.retentionPercent == 100 && state.effectiveBytes == 125 * gib / 8,
           "third recovery step");
    state = ledger.poll({.availableBytes = 0}, at(45));
    expect(state.effectiveBytes == 3 * gib && state.retentionPercent == 25 && state.memoryNotice,
           "measured zero engages pressure again and respects the 3 GiB floor");
    std::size_t late = 0;
    ledger.registerCache(&late, gib, [] { return 0; }, [&](std::size_t bytes) { late = bytes; });
    expect(late > 0 && late <= gib / 4,
           "a cache created during pressure starts with reduced admission immediately");
    const auto priorLate = late;
    const auto updatedLate = ledger.setCacheCeiling(&late, 2 * gib);
    expect(late == updatedLate && late > priorLate,
           "ceiling changes apply their current allowance inside the serialized callback");
    ledger.unregisterCache(&late);
    ledger.unregisterCache(&operation);
    ledger.unregisterCache(&preview);
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
    expect(state.swapPressure && !state.memoryPressure && state.swapNotice && state.memoryNotice &&
               state.retentionPercent == 25,
           "actively rising swap trims the caches");
    state = ledger.poll(swapSample(4 * gib * 9 / 10 + 2 * gib), at(15));
    expect(state.retentionPercent == 10 && !state.swapNotice,
           "persistent swap growth escalates once");
    // Once growth stops, the episode clears through the ordinary recovery hold and ladder. High
    // static occupancy does not block recovery.
    state = ledger.poll(swapSample(4 * gib * 9 / 10 + 2 * gib), at(20));
    expect(!state.swapPressure && state.retentionPercent == 10,
           "a static reading after an episode starts the recovery hold");
    state = ledger.poll(swapSample(4 * gib * 9 / 10 + 2 * gib), at(29));
    expect(state.retentionPercent == 10, "no early restore");
    state = ledger.poll(swapSample(4 * gib * 9 / 10 + 2 * gib), at(30));
    expect(state.retentionPercent == 25, "ample RAM recovers a previously pressured cache");
    state = ledger.poll(swapSample(4 * gib * 9 / 10 + 2 * gib), at(40));
    expect(state.retentionPercent == 50, "recovery continues through the ladder");
    state = ledger.poll(swapSample(4 * gib * 9 / 10 + 2 * gib), at(50));
    expect(state.retentionPercent == 100, "recovery reaches full admission");
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

void ceilings() {
    MemoryBudgetLedger small(16 * gib, 16 * gib);
    expect(small.state().effectiveBytes == 8 * gib, "16 GiB machines start at an 8 GiB cap");
    small.setConfiguredTotal(std::numeric_limits<std::size_t>::max());
    expect(small.state().effectiveBytes == 8 * gib, "explicit ceilings cannot bypass physical cap");
    std::size_t tiny = 0;
    small.registerCache(&tiny, 1024, [] { return 0; }, [&](std::size_t bytes) { tiny = bytes; });
    small.setConfiguredTotal(1024);
    auto state = small.poll({.availableBytes = 16 * gib}, at(0));
    expect(state.effectiveBytes == 3 * gib && tiny <= 1024, "floor never enlarges small overrides");
    MemoryBudgetLedger busy(60 * gib, 60 * gib);
    busy.setConfiguredTotal(16 * gib);
    state = busy.poll({.availableBytes = 40 * gib}, at(0));
    const auto trimmed = busy.poll({.availableBytes = 2 * gib}, at(5));
    expect(trimmed.memoryPressure && trimmed.effectiveBytes == 3 * gib,
           "low RAM trims even with no swap data, so overrides stay ceilings");
}
void machineExamplesAndOverflow() {
    MemoryBudgetLedger sixteen(16 * gib, 16 * gib);
    std::size_t bytes = 0;
    sixteen.registerCache(
        &bytes, 4 * gib, [] { return 4 * gib; }, [&](std::size_t limit) { bytes = limit; });
    const auto state = sixteen.poll({.availableBytes = 3 * gib}, at(0));
    expect(state.effectiveBytes == 7 * gib * 4 / 5 && state.retentionPercent == 25,
           "16 GiB: 3 GiB available plus 4 GiB held gives a 5.6 GiB cap and pressure trimming");
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    MemoryBudgetLedger huge(60 * gib, 60 * gib);
    std::size_t left = 0, right = 0;
    huge.registerCache(
        &left, maximum, [] { return maximum; }, [&](std::size_t limit) { left = limit; });
    huge.registerCache(
        &right, maximum, [] { return maximum; }, [&](std::size_t limit) { right = limit; });
    const auto saturated = huge.poll({.availableBytes = maximum}, at(0));
    expect(left + right <= saturated.effectiveBytes,
           "overflow-sized ceiling sums still divide one aggregate cap");
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
    expect(state.effectiveBytes == gib / 2 && state.processCacheCeilingBytes == gib &&
               state.reusableHostCreditBytes == 0 && !state.awaitingProcessSample,
           "first finite sample starts below half paired hard allowance without retained credit");
    const auto requested = state.configuredBytes;
    auto partial =
        processSample(std::size_t{256} * 1024 * 1024, {}, 1, ProcessMemoryStatus::Partial);
    state = ledger.applySample(partial, at(1));
    expect(state.effectiveBytes <= std::size_t{224} * 1024 * 1024 &&
               state.configuredBytes == requested,
           "partial tighter limit applies immediately inside cooldown while requested ceiling "
           "persists");
    state = ledger.applySample(processSample(0, {}, 2, ProcessMemoryStatus::Partial), at(2));
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
    expect(state.effectiveBytes > 0 && state.effectiveBytes <= 5 * gib / 2,
           "verified recovery from zero uses bounded half-headroom seed after recovery hold");
    const auto before = state.effectiveBytes;
    state = ledger.applySample(processSample(8 * gib, 8 * gib - before + before / 20 - gib, 51),
                               at(51));
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

void processHierarchyAndStableFill() {
    MemoryBudgetLedger hierarchy(60 * gib, 60 * gib);
    hierarchy.beginProcessSampling();
    auto sample = processSample(8 * gib, 2 * gib, 0);
    sample.process.domains.push_back(
        {"/parent", ProcessMemoryLimitKind::Finite, 12 * gib, 11 * gib});
    auto state = hierarchy.applySample(sample, at(0));
    expect(state.effectiveBytes == 0 && state.processCacheCeilingBytes == 0,
           "each ancestor's own usage and reserve beats a roomy leaf");
    for (const auto limit : {std::size_t{0}, std::size_t{1024}, std::size_t{2} * gib}) {
        MemoryBudgetLedger tiny(60 * gib, 60 * gib);
        tiny.beginProcessSampling();
        const auto result = tiny.applySample(processSample(limit, 0, 0), at(0));
        expect(result.effectiveBytes <= limit &&
                   (limit >= std::size_t{16} * 1024 * 1024 || result.effectiveBytes == 0),
               "tiny process limits never inherit the host cache floor");
    }
    MemoryBudgetLedger ledger(60 * gib, 60 * gib);
    ledger.beginProcessSampling();
    std::size_t held = 0, allowance = 0;
    unsigned evictions = 0;
    ledger.registerCache(
        &held, 30 * gib, [&] { return held; },
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
    expect(held > 0 && stable >= 30 && evictions == 0 && state.reusableHostCreditBytes == 0,
           "zero-credit fill/poll/refill converges positively without recurring eviction cycles");
    const auto settled = held;
    state = ledger.applySample(processSample(8 * gib, 3 * gib + held, 401), at(401));
    expect(held < settled && evictions == 1 &&
               state.effectiveBytes <= state.processCacheCeilingBytes,
           "real unrelated usage immediately trims a settled cache");
    const auto afterPressure = evictions;
    for (int index = 41; index < 80; ++index) {
        state =
            ledger.applySample(processSample(8 * gib, 2 * gib + held, index * 10), at(index * 10));
        held = allowance;
    }
    expect(held > 0 && evictions == afterPressure,
           "pressure release recovers without an alternating fill/evict loop");
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
    expect(
        first + second <= state.effectiveBytes && first > second &&
            state.retainedBytes == 40 * gib && state.reusableHostCreditBytes == 0,
        "many owners split one allowance and shared retained gauges never become reusable credit");
    shares.unregisterCache(&first);
    shares.unregisterCache(&second);
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
           "effective allowance follows measured domain while requested ceiling remains unchanged");
    return failures == 0 ? 0 : 1;
#else
    return 77;
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--live-process-probe")
        return liveProcessProbe();
    sequence();
    swapActivityPolicy();
    ceilings();
    machineExamplesAndOverflow();
    processAdmissionAndFailures();
    processHierarchyAndStableFill();
    return failures == 0 ? 0 : 1;
}
