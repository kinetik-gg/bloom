#include <bloom/output/flat_exr_export_write.hpp>

#include "flat_exr_test_support.hpp"

#include <bloom/output/output_analysis_attempt.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <source_location>
#include <string_view>
#include <thread>

namespace {

namespace output = bloom::output;
namespace platform = bloom::platform;
namespace runtime = bloom::runtime;
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

[[nodiscard]] std::shared_ptr<const output::OutputAnalysisAttemptV1>
buildAttempt(output::ExportResourceLedgerV1& ledger, support::Fixture fixture) {
    auto source = support::prepareSource(std::move(fixture));
    auto result = output::buildOutputAnalysisAttemptV1(
        {.frame = source.frame,
         .processIdentity = source.processIdentity,
         .report = source.report,
         .target = {.targetKey = bloom::core::ArtifactTargetKey::fromRaw(1),
                    .observation = platform::ArtifactTargetObservation::absent(),
                    .targetPath = "/tmp/ignored.exr",
                    .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace}},
        ledger);
    if (!result) {
        std::abort();
    }
    return result.attempt();
}

void testWrittenAndIndependentlyVerifiable(Expectations& expectations) {
    output::ExportResourceLedgerV1 ledger;
    auto attempt = buildAttempt(ledger, support::roundTripFixture());
    support::ScratchDirectory scratch("flat-exr-export-write");
    const auto scratchPath = scratch.file("attempt.exr");

    const output::FlatExrExportWriterV1 writer;
    const auto result = writer.run(*attempt, scratchPath, {});
    expectations.expect(result.status() == output::FlatExrExportWriteStatusV1::Written,
                        "writing and verifying a valid attempt succeeds");
    if (result.status() != output::FlatExrExportWriteStatusV1::Written) {
        return;
    }
    expectations.expect(std::filesystem::exists(scratchPath),
                        "the scratch file exists after a Written result");

    // Independent reopen-verification: the same F1 verifier this bridge already used, run again
    // from scratch against the same staged bytes -- "reopened file passes the F1 verifier
    // independently".
    const output::FlatExrRgba32fLinRec709SceneReopenVerifierV1 independentVerifier;
    const auto independentResult =
        independentVerifier.verify(scratchPath, attempt->processIdentity(), attempt->report(), {});
    expectations.expect(independentResult.status() == output::FlatExrVerifyStatusV1::Verified &&
                            independentResult.digest() == result.semanticDigest(),
                        "an independent reopen-verify pass reproduces the same semantic digest");
}

void testCancellationBeforeWritingProducesNoArtifact(Expectations& expectations) {
    output::ExportResourceLedgerV1 ledger;
    auto attempt = buildAttempt(ledger, support::roundTripFixture());
    support::ScratchDirectory scratch("flat-exr-export-write-cancel");
    const auto scratchPath = scratch.file("cancelled.exr");

    runtime::TaskScheduler scheduler;
    auto submission = scheduler.submit<output::FlatExrExportWriteStatusV1>(
        runtime::TaskRequest("cancel-before-writing",
                             runtime::TaskOwner{.kind = runtime::TaskOwnerKind::Export,
                                                .id = runtime::TaskOwnerId::fromRaw(1)},
                             runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo),
        [&attempt, &scratchPath](runtime::TaskContext& context) {
            // The task cancels itself immediately at entry (deterministic, no background race):
            // isCancellationRequested() observes the handle's own cancel() below only after the
            // scheduler has actually delivered it, so this task synchronously spins until it
            // observes its own requested cancellation before ever calling the writer.
            while (!context.isCancellationRequested()) {
            }
            const output::FlatExrExportWriterV1 writer;
            const auto result = writer.run(*attempt, scratchPath, context.cancellation());
            return runtime::TaskResult<output::FlatExrExportWriteStatusV1>::succeeded(
                result.status());
        });
    expectations.expect(submission.accepted(), "the cancellation-race task is accepted");
    if (!submission.accepted()) {
        return;
    }
    submission.handle.cancel();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::optional<runtime::TaskResult<output::FlatExrExportWriteStatusV1>> taken;
    while (std::chrono::steady_clock::now() < deadline) {
        taken = submission.handle.tryTakeResult();
        if (taken.has_value()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expectations.expect(taken.has_value(), "the task reaches a terminal state");
    if (taken.has_value() && taken->value().has_value()) {
        expectations.expect(*taken->value() == output::FlatExrExportWriteStatusV1::Cancelled,
                            "cancellation observed before writing yields Cancelled, not Written");
    }
    expectations.expect(!std::filesystem::exists(scratchPath),
                        "cancellation before writing publishes no artifact at the scratch path");
}

} // namespace

int main() {
    Expectations expectations;
    testWrittenAndIndependentlyVerifiable(expectations);
    testCancellationBeforeWritingProducesNoArtifact(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
