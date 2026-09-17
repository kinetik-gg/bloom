#include <bloom/runtime/memory_budget_ledger.hpp>

#include <chrono>
#include <iostream>
#include <limits>
#include <string_view>

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

void swapAndCeilings() {
    MemoryBudgetLedger ledger(60 * gib, 60 * gib);
    ledger.setConfiguredTotal(16 * gib);
    auto state = ledger.poll(
        {.availableBytes = 40 * gib, .swapTotalBytes = 4 * gib, .swapUsedBytes = gib}, at(0));
    expect(state.effectiveBytes == 16 * gib && !state.swapPressure,
           "25 percent swap is not pressure");
    state = ledger.poll(
        {.availableBytes = 40 * gib, .swapTotalBytes = 4 * gib, .swapUsedBytes = gib + 1}, at(5));
    expect(state.swapPressure && !state.memoryPressure && state.swapNotice && state.memoryNotice &&
               state.retentionPercent == 25,
           "swap alone triggers pressure above 25 percent");
    state = ledger.poll(
        {.availableBytes = 40 * gib, .swapTotalBytes = 4 * gib, .swapUsedBytes = 2 * gib}, at(10));
    expect(state.retentionPercent == 10 && !state.swapNotice, "persistent swap escalates once");
    state = ledger.poll({.availableBytes = 2 * gib}, at(15));
    expect(state.effectiveBytes == 3 * gib, "overrides are ceilings, never guaranteed allocations");
    MemoryBudgetLedger small(16 * gib, 16 * gib);
    expect(small.state().effectiveBytes == 8 * gib, "16 GiB machines start at an 8 GiB cap");
    small.setConfiguredTotal(std::numeric_limits<std::size_t>::max());
    expect(small.state().effectiveBytes == 8 * gib, "explicit ceilings cannot bypass physical cap");
    std::size_t tiny = 0;
    small.registerCache(&tiny, 1024, [] { return 0; }, [&](std::size_t bytes) { tiny = bytes; });
    small.setConfiguredTotal(1024);
    state = small.poll({.availableBytes = 16 * gib}, at(0));
    expect(state.effectiveBytes == 3 * gib && tiny <= 1024, "floor never enlarges small overrides");
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

} // namespace

int main() {
    sequence();
    swapAndCeilings();
    machineExamplesAndOverflow();
    return failures == 0 ? 0 : 1;
}
