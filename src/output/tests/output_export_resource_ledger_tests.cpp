#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string_view>

namespace {

namespace output = bloom::output;
using bloom::runtime::MemoryBudgetLedger;
constexpr std::size_t gib = std::size_t{1024} * 1024 * 1024;

auto at(const int seconds) {
    return MemoryBudgetLedger::Clock::time_point{} + std::chrono::seconds(seconds);
}

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

void testReserveWithinCapSucceeds(Expectations& expectations) {
    output::ExportResourceLedgerV1 ledger(1024);
    output::ExportResourceAdmissionStatusV1 status{};
    auto reservation = ledger.reserve(100, status, /*jobAllowance=*/512);
    expectations.expect(reservation != nullptr &&
                            status == output::ExportResourceAdmissionStatusV1::Reserved,
                        "reserve within both caps succeeds");
    expectations.expect(ledger.chargedBytes() == 100,
                        "the ledger charges exactly the reserved bytes");
    expectations.expect(reservation->chargedBytes() == 100,
                        "the reservation reports its own charged bytes");
}

void testReserveExceedingJobAllowanceFails(Expectations& expectations) {
    output::ExportResourceLedgerV1 ledger(1024);
    output::ExportResourceAdmissionStatusV1 status{};
    auto reservation = ledger.reserve(600, status, /*jobAllowance=*/512);
    expectations.expect(reservation == nullptr &&
                            status == output::ExportResourceAdmissionStatusV1::JobAllowanceExceeded,
                        "a reservation over the per-job cap is refused and charges nothing");
    expectations.expect(ledger.chargedBytes() == 0, "a refused reservation charges zero bytes");
}

void testReserveExceedingServiceAllowanceFails(Expectations& expectations) {
    output::ExportResourceLedgerV1 ledger(100);
    output::ExportResourceAdmissionStatusV1 firstStatus{};
    auto first = ledger.reserve(80, firstStatus, /*jobAllowance=*/1000);
    expectations.expect(first != nullptr, "the first reservation fits the shared allowance");

    output::ExportResourceAdmissionStatusV1 secondStatus{};
    auto second = ledger.reserve(30, secondStatus, /*jobAllowance=*/1000);
    expectations.expect(
        second == nullptr &&
            secondStatus == output::ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded,
        "a second reservation that would exceed the shared concurrent allowance is refused");
    expectations.expect(ledger.chargedBytes() == 80,
                        "a refused second reservation leaves the first charge untouched");
}

void testReleaseOnDestructionIsZeroLeak(Expectations& expectations) {
    output::ExportResourceLedgerV1 ledger(1024);
    {
        output::ExportResourceAdmissionStatusV1 status{};
        auto reservation = ledger.reserve(500, status);
        expectations.expect(reservation != nullptr, "the reservation is granted");
        expectations.expect(ledger.chargedBytes() == 500, "the ledger reflects the live charge");
    }
    expectations.expect(ledger.chargedBytes() == 0,
                        "dropping the reservation releases every charged byte (zero leak)");
}

void testExpandGrowsAndRefusesOverCap(Expectations& expectations) {
    output::ExportResourceLedgerV1 ledger(1024);
    output::ExportResourceAdmissionStatusV1 status{};
    auto reservation = ledger.reserve(100, status, /*jobAllowance=*/300);
    expectations.expect(reservation != nullptr, "the initial reservation is granted");

    expectations.expect(reservation->expand(250) ==
                            output::ExportResourceAdmissionStatusV1::Reserved,
                        "expand() to a higher total within the job cap succeeds");
    expectations.expect(reservation->chargedBytes() == 250,
                        "expand() raises the reservation's own charge to the new total");
    expectations.expect(ledger.chargedBytes() == 250,
                        "expand() charges only the incremental delta against the shared ledger");

    expectations.expect(reservation->expand(200) ==
                            output::ExportResourceAdmissionStatusV1::Reserved,
                        "expand() to a lower total than already charged is a no-op success");
    expectations.expect(reservation->chargedBytes() == 250,
                        "expand() never lowers the charge (no double-charging, no shrink either)");

    expectations.expect(reservation->expand(400) ==
                            output::ExportResourceAdmissionStatusV1::JobAllowanceExceeded,
                        "expand() beyond the per-job cap is refused");
    expectations.expect(reservation->chargedBytes() == 250,
                        "a refused expand() leaves the prior charge intact");
}

void testPressureKeepsChargesAndGraduallyRestoresAdmission(Expectations& expectations) {
    MemoryBudgetLedger memory(16 * gib, 16 * gib);
    memory.setConfiguredTotal(4 * gib);
    output::ExportResourceLedgerV1 ledger(memory);
    output::ExportResourceAdmissionStatusV1 status{};
    auto first = ledger.reserve(2 * gib, status);
    auto second = ledger.reserve(gib, status);
    expectations.expect(first && second, "two jobs reserve 3 GiB within the 2 GiB per-job cap");
    if (!first || !second)
        return;

    const auto pressure = memory.poll({.availableBytes = 6 * gib}, at(0));
    expectations.expect(pressure.retainedBytes == 3 * gib && pressure.retentionPercent == 25 &&
                            ledger.concurrentAllowance() == gib,
                        "the fake pressure poll accounts for 3 GiB and trims admission to 1 GiB");
    auto refused = ledger.reserve(1, status);
    expectations.expect(
        !refused && status == output::ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded,
        "new work receives the typed service refusal while over the trimmed cap");
    expectations.expect(second->expand(gib + 1) ==
                            output::ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded,
                        "existing work cannot grow beyond the trimmed shared allowance");
    expectations.expect(second->expand(gib) == output::ExportResourceAdmissionStatusV1::Reserved,
                        "an expansion that needs no additional bytes remains a no-op success");
    expectations.expect(first->chargedBytes() == 2 * gib && second->chargedBytes() == gib &&
                            ledger.chargedBytes() == 3 * gib,
                        "trimming and refused admission leave every live reservation untouched");

    static_cast<void>(memory.poll({.availableBytes = 16 * gib}, at(1)));
    static_cast<void>(memory.poll({.availableBytes = 16 * gib}, at(10)));
    expectations.expect(ledger.concurrentAllowance() == gib,
                        "healthy samples do not restore admission before the ten-second hold");
    static_cast<void>(memory.poll({.availableBytes = 16 * gib}, at(11)));
    expectations.expect(ledger.concurrentAllowance() == 2 * gib,
                        "the first recovery step restores only half the configured ceiling");
    refused = ledger.reserve(1, status);
    expectations.expect(
        !refused && status == output::ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded,
        "partial recovery still refuses admission above the live allowance");
    static_cast<void>(memory.poll({.availableBytes = 16 * gib}, at(21)));
    auto admitted = ledger.reserve(gib / 2, status);
    expectations.expect(ledger.concurrentAllowance() == 4 * gib && admitted &&
                            status == output::ExportResourceAdmissionStatusV1::Reserved,
                        "full gradual recovery admits work again without releasing active jobs");
    expectations.expect(second->expand(3 * gib / 2) ==
                                output::ExportResourceAdmissionStatusV1::Reserved &&
                            ledger.chargedBytes() == 4 * gib,
                        "restored allowance also admits growth up to the original ceiling");
}

void testReleaseAllowsAdmissionUnderPressure(Expectations& expectations) {
    MemoryBudgetLedger memory(16 * gib, 16 * gib);
    memory.setConfiguredTotal(4 * gib);
    output::ExportResourceLedgerV1 ledger(memory);
    output::ExportResourceAdmissionStatusV1 status{};
    auto first = ledger.reserve(2 * gib, status);
    auto second = ledger.reserve(gib, status);
    static_cast<void>(memory.poll({.availableBytes = 6 * gib}, at(0)));
    first.reset();
    auto refused = ledger.reserve(1, status);
    expectations.expect(ledger.chargedBytes() == gib && !refused &&
                            status ==
                                output::ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded,
                        "releasing one job leaves the other charged at the entire trimmed cap");
    second.reset();
    auto admitted = ledger.reserve(gib, status);
    expectations.expect(
        admitted && status == output::ExportResourceAdmissionStatusV1::Reserved &&
            ledger.concurrentAllowance() == gib,
        "releases reopen admission within the lower cap without waiting for recovery");
}

void testRegistrationDuringPressure(Expectations& expectations) {
    MemoryBudgetLedger memory(16 * gib, 16 * gib);
    memory.setConfiguredTotal(4 * gib);
    static_cast<void>(memory.poll({.availableBytes = 6 * gib}, at(0)));
    output::ExportResourceLedgerV1 ledger(memory);
    output::ExportResourceAdmissionStatusV1 status{};
    auto refused = ledger.reserve(gib + 1, status);
    expectations.expect(
        ledger.concurrentAllowance() == gib && !refused &&
            status == output::ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded,
        "a queue created during pressure starts with reduced admission immediately");
}

void testUnregisterAfterFacadeAndLastReservation(Expectations& expectations) {
    MemoryBudgetLedger memory(16 * gib, 16 * gib);
    memory.setConfiguredTotal(4 * gib);
    output::ExportResourceLedgerV1 peer(memory);
    std::shared_ptr<output::ExportResourceReservationV1> reservation;
    {
        output::ExportResourceLedgerV1 ledger(memory);
        output::ExportResourceAdmissionStatusV1 status{};
        reservation = ledger.reserve(gib, status);
        expectations.expect(reservation != nullptr, "a reservation can outlive its ledger facade");
    }
    if (!reservation)
        return;
    auto alias = reservation;
    reservation.reset();
    const auto held = memory.poll({.availableBytes = 16 * gib}, at(0));
    expectations.expect(
        held.retainedBytes == gib && peer.concurrentAllowance() == 2 * gib,
        "the last reservation alias keeps both its charge and participant weight alive");
    expectations.expect(alias->expand(3 * gib / 2) ==
                            output::ExportResourceAdmissionStatusV1::Reserved,
                        "a reservation remains usable after its facade is destroyed");
    const auto pressure = memory.poll({.availableBytes = 6 * gib}, at(5));
    expectations.expect(
        pressure.retainedBytes == 3 * gib / 2 &&
            alias->expand(2 * gib) ==
                output::ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded,
        "pressure callbacks still constrain a reservation after facade destruction");
    alias.reset();
    const auto released = memory.poll({.availableBytes = 6 * gib}, at(10));
    expectations.expect(released.retainedBytes == 0 && peer.concurrentAllowance() == 4 * gib / 10,
                        "last release unregisters callbacks and their weight before the next poll");
    // With ASan, this poll also detects callbacks accessing the destroyed reservation state.
}

void testEmptyFacadeUnregisters(Expectations& expectations) {
    MemoryBudgetLedger memory(16 * gib, 16 * gib);
    memory.setConfiguredTotal(4 * gib);
    output::ExportResourceLedgerV1 peer(memory);
    {
        output::ExportResourceLedgerV1 ledger(memory);
        static_cast<void>(memory.poll({.availableBytes = 16 * gib}, at(0)));
        expectations.expect(peer.concurrentAllowance() == 2 * gib,
                            "two export ledgers share one aggregate cap");
    }
    static_cast<void>(memory.poll({.availableBytes = 16 * gib}, at(1)));
    expectations.expect(peer.concurrentAllowance() == 4 * gib,
                        "an empty facade unregisters immediately and restores its peer's share");
}

void testDefaultUsesProcessLedger(Expectations& expectations) {
    auto& memory = bloom::runtime::processMemoryBudgetLedger();
    {
        output::ExportResourceLedgerV1 ledger(1024);
        output::ExportResourceAdmissionStatusV1 status{};
        auto reservation = ledger.reserve(100, status);
        expectations.expect(reservation && memory.poll({}, at(0)).retainedBytes == 100,
                            "the default constructor registers with the process memory ledger");
    }
    expectations.expect(memory.poll({}, at(1)).retainedBytes == 0,
                        "the process ledger has no dangling participant after normal destruction");
}

} // namespace

int main() {
    Expectations expectations;
    testReserveWithinCapSucceeds(expectations);
    testReserveExceedingJobAllowanceFails(expectations);
    testReserveExceedingServiceAllowanceFails(expectations);
    testReleaseOnDestructionIsZeroLeak(expectations);
    testExpandGrowsAndRefusesOverCap(expectations);
    testPressureKeepsChargesAndGraduallyRestoresAdmission(expectations);
    testReleaseAllowsAdmissionUnderPressure(expectations);
    testRegistrationDuringPressure(expectations);
    testUnregisterAfterFacadeAndLastReservation(expectations);
    testEmptyFacadeUnregisters(expectations);
    testDefaultUsesProcessLedger(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
