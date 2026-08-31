// Tests for PngExportWriterV1 (issue #111): the approved export job's PNG half -- ColorPreparing
// through the retained qualified display processor, PreparingOutput, G1's writer, G1's reopen
// verifier with all four identity-issuance inputs, and the artifact hash. Mirrors
// flat_exr_export_write_tests.cpp's shape, plus the PNG-only retained-display-product and
// prepared-bytes-limit cases flat OpenEXR has no equivalent of.

#include "png_test_support.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/artifact_target_key.hpp>
#include <bloom/output/output_analysis_attempt.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/output/png_export_write.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace color = bloom::color;
namespace core = bloom::core;
namespace output = bloom::output;
namespace platform = bloom::platform;
namespace runtime = bloom::runtime;
namespace support = bloom_output_png_test_support;

[[nodiscard]] output::OutputAnalysisAttemptTargetV1 fixtureTarget() {
    return {.targetKey = core::ArtifactTargetKey::fromRaw(11),
            .observation = platform::ArtifactTargetObservation::absent(),
            .targetPath = "/tmp/does-not-matter.png",
            .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace};
}

// The real qualified Bloom Neutral CPU display processor, resolved and built exactly the way the
// attempt's own blocking stage does (C2's in-process registry + processor builder against the
// embedded payload's frozen expectedRevision). Built once per process: the OCIO config parse is the
// expensive part of these tests and nothing here mutates the handle.
[[nodiscard]] std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> qualifiedHandle() {
    static const std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> handle = [] {
        auto resolution = color::resolveBloomNeutralV1BuiltIn(
            color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
            color::kBloomNeutralV1ConfigDigest);
        if (!resolution.ready()) {
            std::abort();
        }
        auto resolved = std::move(resolution).takeResolved();
        if (!resolved.has_value()) {
            std::abort();
        }
        auto built = color::buildBloomNeutralCpuDisplayProcessor(*resolved);
        auto handleValue = std::move(built).takeHandle();
        if (!handleValue.has_value()) {
            std::abort();
        }
        return std::make_shared<const color::PreparedCpuDisplayProcessorHandle>(
            std::move(*handleValue));
    }();
    return handle;
}

[[nodiscard]] output::OutputAnalysisAttemptDisplayProductsV1 fixtureDisplayProducts() {
    auto handle = qualifiedHandle();
    // The aliasing shared_ptr the production runner uses: the identity is the handle's own, never
    // an independently adopted copy.
    std::shared_ptr<const color::DisplayProcessorIdentityV1> identity(handle, &handle->identity());
    return {.processor = std::move(handle),
            .identity = std::move(identity),
            .expectedOcioRevision = color::kBloomNeutralV1ConfigDigest};
}

// Runs the same pre-approval pipeline the production attempt graph runs -- identity preparation,
// the PNG analyzer with the resolved color state, then buildOutputAnalysisAttemptV1 -- so every
// test below exercises a genuine retained attempt rather than a hand-assembled stand-in.
[[nodiscard]] std::shared_ptr<const output::OutputAnalysisAttemptV1>
buildPngAttempt(support::Expectations& expectations, output::ExportResourceLedgerV1& ledger,
                const std::uint32_t width, const std::uint32_t height,
                const output::PngRgba8SrgbColorResolutionStateV1 colorResolution =
                    output::PngRgba8SrgbColorResolutionStateV1::Ready) {
    auto frame = support::publish(support::pngShapedFixture(width, height));
    const output::ProcessFrameSemanticIdentityV1Preparer identityPreparer;
    const auto identityResult = identityPreparer.prepare(frame, {});
    if (identityResult.status() !=
            output::ProcessFrameSemanticIdentityPreparationStatus::Prepared ||
        identityResult.identity() == nullptr) {
        expectations.expect(false, "attempt fixture: the process identity prepares");
        return nullptr;
    }
    const auto analyzed = output::analyzePngRgba8SrgbV1(
        {.process = {.state = output::OutputAnalysisProcessSourceStateV1::Ready,
                     .readyIdentity = identityResult.identity(),
                     .missingDescriptor = std::nullopt},
         .expectedOcioRevision = color::kBloomNeutralV1ConfigDigest,
         .colorResolution = colorResolution});
    if (!analyzed.hasReport()) {
        expectations.expect(false, "attempt fixture: the PNG analyzer produces a report");
        return nullptr;
    }
    const bool ready = colorResolution == output::PngRgba8SrgbColorResolutionStateV1::Ready;
    auto result = output::buildOutputAnalysisAttemptV1(
        {.frame = frame,
         .processIdentity = identityResult.identity(),
         .report = analyzed.report(),
         .target = fixtureTarget(),
         .display =
             ready ? fixtureDisplayProducts() : output::OutputAnalysisAttemptDisplayProductsV1{}},
        ledger);
    expectations.expect(static_cast<bool>(result), "attempt fixture: the PNG attempt builds");
    return result ? result.attempt() : nullptr;
}

// -------------------------------------------------------------------------------------------

void testPngExportWriteRoundTripAndIndependentDecode(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("png-export-round-trip");
    output::ExportResourceLedgerV1 ledger;
    const auto attempt = buildPngAttempt(expectations, ledger, 5, 3);
    if (attempt == nullptr) {
        return;
    }
    expectations.expect(attempt->preset() == output::OutputPresetV1::PngRgba8SrgbV1 &&
                            attempt->approvable() && attempt->display().isPresent(),
                        "round trip: the PNG attempt is approvable and retains its display "
                        "products");

    const auto destination = scratch.file("round-trip.png");
    const output::PngExportWriterV1 writer;
    const auto result = writer.run(*attempt, destination, {});
    expectations.expect(result.status() == output::PngExportWriteStatusV1::Written,
                        "round trip: the PNG export write/verify sequence succeeds");
    if (result.status() != output::PngExportWriteStatusV1::Written) {
        return;
    }
    expectations.expect(result.preparedByteCount() == std::size_t{5} * 3 * 4,
                        "round trip: the charged prepared byte count is width * height * 4");
    expectations.expect(result.artifactByteCount() > 0 &&
                            result.artifactByteCount() == std::filesystem::file_size(destination),
                        "round trip: the artifact byte count matches the staged file");

    const auto decoded = support::independentlyDecodePng(destination);
    expectations.expect(decoded.ok && decoded.everyCrcValid && decoded.everyRowFilterZero,
                        "round trip: the staged file independently decodes with valid CRCs and "
                        "zero row filters");
    expectations.expect(decoded.chunkTypesInOrder ==
                            std::vector<std::string>{"IHDR", "sRGB", "IDAT", "IEND"},
                        "round trip: the independent decode sees exactly the closed chunk profile");
    expectations.expect(decoded.width == 5 && decoded.height == 3 && decoded.bitDepth == 8 &&
                            decoded.colorType == 6 && decoded.interlaceMethod == 0 &&
                            decoded.srgbIntent == 0,
                        "round trip: the independent decode sees the required IHDR/sRGB fields");
}

void testKind1DigestIsStableAcrossTwoRuns(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("png-export-digest-stability");
    output::ExportResourceLedgerV1 ledgerA;
    output::ExportResourceLedgerV1 ledgerB;
    const auto attemptA = buildPngAttempt(expectations, ledgerA, 4, 4);
    const auto attemptB = buildPngAttempt(expectations, ledgerB, 4, 4);
    if (attemptA == nullptr || attemptB == nullptr) {
        return;
    }
    const output::PngExportWriterV1 writer;
    const auto resultA = writer.run(*attemptA, scratch.file("stable-a.png"), {});
    const auto resultB = writer.run(*attemptB, scratch.file("stable-b.png"), {});
    expectations.expect(resultA.status() == output::PngExportWriteStatusV1::Written &&
                            resultB.status() == output::PngExportWriteStatusV1::Written,
                        "digest stability: both independent runs write and verify");
    if (resultA.status() != output::PngExportWriteStatusV1::Written ||
        resultB.status() != output::PngExportWriteStatusV1::Written) {
        return;
    }
    expectations.expect(resultA.semanticDigest() == resultB.semanticDigest(),
                        "digest stability: the kind-1 semantic identity digest is identical across "
                        "two independent runs over the identical fixture");
}

void testPreparedBytesLimitExceededIsTyped(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("png-export-prepared-limit");
    output::ExportResourceLedgerV1 ledger;
    const auto attempt = buildPngAttempt(expectations, ledger, 8, 8);
    if (attempt == nullptr) {
        return;
    }
    const auto destination = scratch.file("over-limit.png");
    const output::PngExportWriterV1 writer;
    // 8 * 8 * 4 = 256 prepared bytes; a lowered ceiling of 255 is one byte under.
    // frame-output.md's closed 256 MiB ceiling can never itself be EXCEEDED through the production
    // pipeline (the closed 2^26 pixel-count limit caps prepared bytes at exactly 256 MiB), so the
    // "a request may lower but not raise them" seam is the only way to reach this rejection --
    // see output_limits.hpp's own reachability note.
    const auto result = writer.run(*attempt, destination, {}, {}, 255);
    expectations.expect(result.status() == output::PngExportWriteStatusV1::Failed &&
                            result.error() ==
                                output::PngExportWriteErrorCodeV1::PreparedBytesLimitExceeded,
                        "prepared-bytes limit: an over-limit prepared stream is a typed "
                        "PreparedBytesLimitExceeded failure");
    expectations.expect(!std::filesystem::exists(destination),
                        "prepared-bytes limit: nothing is written and no file is created");

    const auto atLimit = writer.run(*attempt, scratch.file("at-limit.png"), {}, {}, 256);
    expectations.expect(atLimit.status() == output::PngExportWriteStatusV1::Written,
                        "prepared-bytes limit: a prepared stream exactly AT the limit is accepted "
                        "(values exactly at a limit do not exceed it)");
}

void testMissingDisplayProductsIsTyped(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("png-export-missing-display");
    output::ExportResourceLedgerV1 ledger;
    // A PNG attempt whose color resolution was Missing: it completes (a truthful, non-approvable
    // report) but retains no display products, so the writer must refuse it typed rather than
    // dereference a null processor.
    const auto attempt = buildPngAttempt(expectations, ledger, 4, 4,
                                         output::PngRgba8SrgbColorResolutionStateV1::Missing);
    if (attempt == nullptr) {
        return;
    }
    expectations.expect(!attempt->approvable() && attempt->display().isAbsent(),
                        "missing display: a non-Ready color resolution completes a non-approvable "
                        "attempt with no display products");
    const auto destination = scratch.file("missing-display.png");
    const output::PngExportWriterV1 writer;
    const auto result = writer.run(*attempt, destination, {});
    expectations.expect(result.status() == output::PngExportWriteStatusV1::Failed &&
                            result.error() ==
                                output::PngExportWriteErrorCodeV1::MissingDisplayProducts,
                        "missing display: the writer refuses typed MissingDisplayProducts");
    expectations.expect(!std::filesystem::exists(destination),
                        "missing display: no file is created");
}

void testCancellationDuringColorPreparingWritesNothing(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("png-export-cancel-color");
    output::ExportResourceLedgerV1 ledger;
    // 400 * 400 = 160000 pixels spans three of C2's 65536-pixel apply chunks, so this exercises a
    // genuinely multi-chunk apply. The rendezvous is the ColorPreparing progress report emitted
    // immediately before that chunk loop (the unique completed==0/total==0 one), which gives a
    // DETERMINISTIC synchronization point instead of racing a background thread; cancellation is
    // therefore observed at the loop's first chunk-boundary check. C2's chunk loop exposes no
    // per-chunk progress hook of its own, so a later boundary cannot be targeted without adding one
    // -- out of scope here, and immaterial to what this proves: cancellation inside ColorPreparing
    // packs no pixels, creates no file, and publishes nothing.
    const auto attempt = buildPngAttempt(expectations, ledger, 400, 400);
    if (attempt == nullptr) {
        return;
    }
    const auto destination = scratch.file("cancelled.png");

    runtime::TaskSchedulerConfig config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    runtime::TaskScheduler scheduler(config);
    std::mutex mutex;
    std::condition_variable condition;
    bool reachedColorPreparing = false;
    bool released = false;
    std::atomic_bool cancelled = false;
    std::atomic_bool written = false;
    const output::PngExportWriterV1 writer;

    auto submission = scheduler.submit<void>(
        runtime::TaskRequest(
            "PNG export color cancellation",
            {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(1)}),
        [&](runtime::TaskContext& context) {
            const auto result =
                writer.run(*attempt, destination, context.cancellation(),
                           [&](const output::OutputExportProgressV1& progress) {
                               // The one ColorPreparing report emitted immediately before C2's
                               // apply loop.
                               if (progress.stage != output::OutputExportStageV1::ColorPreparing ||
                                   progress.completed != 0 || progress.total != 0) {
                                   return;
                               }
                               std::unique_lock lock(mutex);
                               reachedColorPreparing = true;
                               condition.notify_all();
                               condition.wait(lock, [&released] { return released; });
                           });
            cancelled.store(result.status() == output::PngExportWriteStatusV1::Cancelled,
                            std::memory_order_release);
            written.store(result.status() == output::PngExportWriteStatusV1::Written,
                          std::memory_order_release);
            return runtime::TaskResult<void>::succeeded();
        });
    {
        std::unique_lock lock(mutex);
        expectations.expect(
            submission.accepted() &&
                condition.wait_for(lock, 10s,
                                   [&reachedColorPreparing] { return reachedColorPreparing; }),
            "color cancellation: the fixture reaches the ColorPreparing rendezvous");
    }
    submission.handle.cancel();
    {
        const std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    std::optional<runtime::TaskResult<void>> taskResult;
    while (!taskResult.has_value() && std::chrono::steady_clock::now() < deadline) {
        taskResult = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expectations.expect(cancelled.load(std::memory_order_acquire) &&
                            !written.load(std::memory_order_acquire),
                        "color cancellation: the run reports a typed Cancelled result, never "
                        "Written");
    expectations.expect(!std::filesystem::exists(destination),
                        "color cancellation: cancellation during ColorPreparing writes no file at "
                        "all");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    expectations.expect(scheduler.isQuiescent(), "color cancellation: the fixture shuts down "
                                                 "cleanly");
}

} // namespace

int main() {
    support::Expectations expectations;
    testPngExportWriteRoundTripAndIndependentDecode(expectations);
    testKind1DigestIsStableAcrossTwoRuns(expectations);
    testPreparedBytesLimitExceededIsTyped(expectations);
    testMissingDisplayProductsIsTyped(expectations);
    testCancellationDuringColorPreparingWritesNothing(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
