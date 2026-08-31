#include <bloom/output/output_analysis_attempt.hpp>

#include "flat_exr_test_support.hpp"

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string_view>

namespace {

namespace output = bloom::output;
namespace platform = bloom::platform;
namespace support = bloom_output_flat_exr_test_support;

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

[[nodiscard]] output::OutputAnalysisAttemptTargetV1 fixtureTarget() {
    return {.targetKey = bloom::core::ArtifactTargetKey::fromRaw(7),
            .observation = platform::ArtifactTargetObservation::absent(),
            .targetPath = "/tmp/does-not-matter.exr",
            .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace};
}

void testBuildSucceedsAndRetainsProducts(Expectations& expectations) {
    auto source = support::prepareSource(support::roundTripFixture());
    output::ExportResourceLedgerV1 ledger;
    auto result = output::buildOutputAnalysisAttemptV1({.frame = source.frame,
                                                        .processIdentity = source.processIdentity,
                                                        .report = source.report,
                                                        .target = fixtureTarget()},
                                                       ledger);
    expectations.expect(static_cast<bool>(result),
                        "a valid ready EXR source builds a completed attempt");
    if (!result) {
        return;
    }
    const auto& attempt = *result.attempt();
    expectations.expect(attempt.frame() == source.frame,
                        "the attempt retains the exact evaluated frame");
    expectations.expect(attempt.processIdentity() == source.processIdentity,
                        "the attempt retains the exact process identity");
    expectations.expect(attempt.report() == source.report, "the attempt retains the exact report");
    expectations.expect(attempt.approvable() && attempt.digest().has_value(),
                        "a nominal EXR report is approvable and carries a digest");
    expectations.expect(attempt.target().targetKey == fixtureTarget().targetKey,
                        "the attempt retains the canonical target preflight");
    expectations.expect(attempt.resources() != nullptr && attempt.resources()->chargedBytes() > 0,
                        "the attempt reserves nonzero retained bytes");
}

void testDigestIsStableAcrossTwoBuilds(Expectations& expectations) {
    auto sourceA = support::prepareSource(support::roundTripFixture());
    auto sourceB = support::prepareSource(support::roundTripFixture());
    output::ExportResourceLedgerV1 ledger;
    auto resultA = output::buildOutputAnalysisAttemptV1({.frame = sourceA.frame,
                                                         .processIdentity = sourceA.processIdentity,
                                                         .report = sourceA.report,
                                                         .target = fixtureTarget()},
                                                        ledger);
    auto resultB = output::buildOutputAnalysisAttemptV1({.frame = sourceB.frame,
                                                         .processIdentity = sourceB.processIdentity,
                                                         .report = sourceB.report,
                                                         .target = fixtureTarget()},
                                                        ledger);
    expectations.expect(static_cast<bool>(resultA) && static_cast<bool>(resultB),
                        "both identical-fixture builds succeed");
    if (!resultA || !resultB) {
        return;
    }
    expectations.expect(resultA.attempt()->digest().has_value() &&
                            resultB.attempt()->digest().has_value() &&
                            *resultA.attempt()->digest() == *resultB.attempt()->digest(),
                        "the approval digest is stable across two runs over the same fixture");
}

void testInvalidTargetIsRejected(Expectations& expectations) {
    auto source = support::prepareSource(support::roundTripFixture());
    output::ExportResourceLedgerV1 ledger;
    auto result = output::buildOutputAnalysisAttemptV1(
        {.frame = source.frame,
         .processIdentity = source.processIdentity,
         .report = source.report,
         .target = {}}, // default-constructed target has an invalid ArtifactTargetKey
        ledger);
    expectations.expect(!result && result.error() ==
                                       output::OutputAnalysisAttemptErrorCodeV1::InvalidTarget,
                        "an invalid (never-preflighted) target is a typed InvalidTarget failure");
}

void testResourceExhaustionIsTypedWithZeroLeak(Expectations& expectations) {
    auto source = support::prepareSource(support::roundTripFixture());
    output::ExportResourceLedgerV1 ledger(/*concurrentAllowance=*/1);
    auto result = output::buildOutputAnalysisAttemptV1({.frame = source.frame,
                                                        .processIdentity = source.processIdentity,
                                                        .report = source.report,
                                                        .target = fixtureTarget()},
                                                       ledger);
    expectations.expect(
        !result &&
            result.error() == output::OutputAnalysisAttemptErrorCodeV1::ResourceReservationFailed,
        "a ledger too small for the retained bytes is a typed ResourceReservationFailed failure");
    expectations.expect(ledger.chargedBytes() == 0,
                        "a refused attempt build charges nothing against the ledger (zero leak)");
}

} // namespace

int main() {
    Expectations expectations;
    testBuildSucceedsAndRetainsProducts(expectations);
    testDigestIsStableAcrossTwoBuilds(expectations);
    testInvalidTargetIsRejected(expectations);
    testResourceExhaustionIsTypedWithZeroLeak(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
