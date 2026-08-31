#include <bloom/output/output_export_resource_ledger.hpp>

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string_view>

namespace {

namespace output = bloom::output;

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

} // namespace

int main() {
    Expectations expectations;
    testReserveWithinCapSucceeds(expectations);
    testReserveExceedingJobAllowanceFails(expectations);
    testReserveExceedingServiceAllowanceFails(expectations);
    testReleaseOnDestructionIsZeroLeak(expectations);
    testExpandGrowsAndRefusesOverCap(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
