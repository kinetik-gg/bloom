// Tests for FlatExrRgba32fLinRec709SceneReopenVerifierV1 (issue #99): round trip (write, verify,
// issue identity; semantic determinism across two independent runs), adversarial byte-surgery
// fixtures (each a typed failure naming the defect), and cancellation mid-verify.

#include "flat_exr_test_support.hpp"

#include <bloom/output/flat_exr_output_adapter.hpp>
#include <bloom/output/flat_exr_reopen_verifier.hpp>
#include <bloom/runtime/task_scheduler.hpp>

// Adversarial fixture construction reopens/rewrites with the real library (see
// writeSingleBitPerturbedVariant below) and ExrHeaderBytes does raw file I/O; neither exposes an
// OpenEXR/Imath type through any bloom/output public header.
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputFile.h>
#include <ImfOutputFile.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace core = bloom::core;
namespace output = bloom::output;
namespace render = bloom::render;
namespace runtime = bloom::runtime;
namespace support = bloom_output_flat_exr_test_support;

struct WrittenFixture final {
    support::PreparedSource source;
    std::filesystem::path path;
};

[[nodiscard]] WrittenFixture writeValidFixture(const support::ScratchDirectory& scratch,
                                               const std::string& fileName) {
    auto source = support::prepareSource(support::roundTripFixture());
    const auto path = scratch.file(fileName);
    const output::FlatExrRgba32fLinRec709SceneWriterV1 writer;
    const auto written = writer.write(*source.frame, path, {});
    if (written.status() != output::FlatExrWriteStatusV1::Written) {
        std::abort();
    }
    return {std::move(source), path};
}

void testRoundTrip(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("round-trip");
    auto runA = writeValidFixture(scratch, "run-a.exr");
    auto runB = writeValidFixture(scratch, "run-b.exr");

    const output::FlatExrRgba32fLinRec709SceneReopenVerifierV1 verifier;
    const auto resultA =
        verifier.verify(runA.path, runA.source.processIdentity, runA.source.report, {});
    const auto resultB =
        verifier.verify(runB.path, runB.source.processIdentity, runB.source.report, {});

    expectations.expect(resultA.status() == output::FlatExrVerifyStatusV1::Verified,
                        "an independent write+verify of a valid fixture is Verified");
    expectations.expect(resultB.status() == output::FlatExrVerifyStatusV1::Verified,
                        "a second independent write+verify of the same fixture is Verified");
    expectations.expect(resultA.digest() == resultB.digest(),
                        "the kind-2 semantic identity digest is stable across two independent "
                        "write+verify runs (semantic determinism)");

    const auto bytesA = std::filesystem::file_size(runA.path);
    const auto bytesB = std::filesystem::file_size(runB.path);
    if (bytesA == bytesB) {
        std::ifstream streamA(runA.path, std::ios::binary);
        std::ifstream streamB(runB.path, std::ios::binary);
        const std::vector<char> contentA((std::istreambuf_iterator<char>(streamA)),
                                         std::istreambuf_iterator<char>());
        const std::vector<char> contentB((std::istreambuf_iterator<char>(streamB)),
                                         std::istreambuf_iterator<char>());
        if (contentA == contentB) {
            // Note only, per the task: byte-identical artifacts across runs with the qualified
            // zlib are test evidence of this build's determinism, not a claimed portable-preset
            // contract (docs/architecture/frame-output.md, "Determinism And Portable Output
            // Identity" -- "A qualified dependency profile may additionally assert byte-for-byte
            // reproducibility ... That stronger profile-specific claim is test evidence, not part
            // of portable preset semantics").
            expectations.expect(
                true, "note: artifact bytes were also byte-identical across two runs with this "
                      "build's qualified zlib (not claimed as portable preset contract)");
        }
    }
}

void testHeaderConformanceRejectsForeignAttribute(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("adversarial-foreign-attribute");
    auto fixture = writeValidFixture(scratch, "base.exr");
    const auto corruptPath = scratch.file("foreign-attribute.exr");
    std::filesystem::copy_file(fixture.path, corruptPath);

    support::ExrHeaderBytes bytes(corruptPath);
    // A minimal well-formed int attribute record: name\0 type\0 size(=4, LE) value(4 bytes).
    std::vector<unsigned char> record{'f', 'o', 'r', 'e', 'i', 'g', 'n', 0, 'i', 'n',
                                      't', 0,   4,   0,   0,   0,   0,   0, 0,   0};
    bytes.insertAttributeRecord(record);
    bytes.save();

    const output::FlatExrRgba32fLinRec709SceneReopenVerifierV1 verifier;
    const auto result =
        verifier.verify(corruptPath, fixture.source.processIdentity, fixture.source.report, {});
    expectations.expect(
        result.status() == output::FlatExrVerifyStatusV1::Failed &&
            result.diagnostic().code == output::FlatExrVerifyErrorCodeV1::UnexpectedAttribute &&
            result.diagnostic().attributeName == "foreign",
        "an added foreign attribute is a typed failure naming it, no identity issued");
}

void testWrongCompressionEnum(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("adversarial-compression");
    auto fixture = writeValidFixture(scratch, "base.exr");
    const auto corruptPath = scratch.file("wrong-compression.exr");
    std::filesystem::copy_file(fixture.path, corruptPath);

    support::ExrHeaderBytes bytes(corruptPath);
    const auto [valueOffset, valueSize] = bytes.valueRange("compression");
    expectations.expect(valueSize == 1, "the compression attribute is a single byte on disk");
    bytes.patchByte(valueOffset, 0); // NO_COMPRESSION instead of ZIP_COMPRESSION
    bytes.save();

    const output::FlatExrRgba32fLinRec709SceneReopenVerifierV1 verifier;
    const auto result =
        verifier.verify(corruptPath, fixture.source.processIdentity, fixture.source.report, {});
    expectations.expect(
        result.status() == output::FlatExrVerifyStatusV1::Failed &&
            result.diagnostic().code == output::FlatExrVerifyErrorCodeV1::AttributeValueMismatch &&
            result.diagnostic().attributeName == "compression",
        "a wrong compression enum is a typed failure naming the compression attribute");
}

void testRenamedChannel(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("adversarial-channel");
    auto fixture = writeValidFixture(scratch, "base.exr");
    const auto corruptPath = scratch.file("renamed-channel.exr");
    std::filesystem::copy_file(fixture.path, corruptPath);

    support::ExrHeaderBytes bytes(corruptPath);
    const auto [valueOffset, valueSize] = bytes.valueRange("channels");
    bool patched = false;
    for (std::size_t offset = valueOffset; offset + 1 < valueOffset + valueSize; ++offset) {
        if (bytes.byteAt(offset) == 'R' && bytes.byteAt(offset + 1) == 0) {
            bytes.patchByte(offset, 'X');
            patched = true;
            break;
        }
    }
    if (!patched) {
        std::abort();
    }
    bytes.save();

    const output::FlatExrRgba32fLinRec709SceneReopenVerifierV1 verifier;
    const auto result =
        verifier.verify(corruptPath, fixture.source.processIdentity, fixture.source.report, {});
    expectations.expect(result.status() == output::FlatExrVerifyStatusV1::Failed &&
                            result.diagnostic().code ==
                                output::FlatExrVerifyErrorCodeV1::InvalidChannelList,
                        "a renamed channel ('R' -> 'X') is a typed channel-list failure");
}

// Reconstructs a byte-valid EXR with the identical header (all ten attributes copied verbatim,
// so every attribute/channel check still passes) but exactly one Float32 sample bit flipped.
// Raw byte surgery on the ZIP-compressed chunk bytes themselves is not attempted: ZIP compresses
// in 16-scanline blocks, so a single flipped compressed byte would corrupt the whole block's
// zlib stream (an undecodable chunk, not a clean single-bit sample defect) rather than proving
// the verifier's per-sample bit comparison specifically.
void writeSingleBitPerturbedVariant(const std::filesystem::path& validPath,
                                    const std::filesystem::path& outPath) {
    Imf::InputFile input(validPath.string().c_str());
    const Imf::Header header(input.header());
    const auto& dataWindow = header.dataWindow();
    const auto width = static_cast<std::size_t>(static_cast<std::int64_t>(dataWindow.max.x) -
                                                static_cast<std::int64_t>(dataWindow.min.x) + 1);
    const auto heightRows =
        static_cast<std::size_t>(static_cast<std::int64_t>(dataWindow.max.y) -
                                 static_cast<std::int64_t>(dataWindow.min.y) + 1);
    const auto pixelCount = width * heightRows;
    std::vector<float> red(pixelCount), green(pixelCount), blue(pixelCount), alpha(pixelCount);

    const auto rowStride = sizeof(float) * width;
    Imf::FrameBuffer inFrameBuffer;
    inFrameBuffer.insert(
        "R", Imf::Slice::Make(Imf::FLOAT, red.data(), dataWindow, sizeof(float), rowStride));
    inFrameBuffer.insert(
        "G", Imf::Slice::Make(Imf::FLOAT, green.data(), dataWindow, sizeof(float), rowStride));
    inFrameBuffer.insert(
        "B", Imf::Slice::Make(Imf::FLOAT, blue.data(), dataWindow, sizeof(float), rowStride));
    inFrameBuffer.insert(
        "A", Imf::Slice::Make(Imf::FLOAT, alpha.data(), dataWindow, sizeof(float), rowStride));
    input.setFrameBuffer(inFrameBuffer);
    input.readPixels(dataWindow.min.y, dataWindow.max.y);

    // Flip the least-significant bit of the last row's last pixel's red sample -- a scanline
    // near the end of the image, distinct from row 0, so the test can confirm the verifier names
    // the actual defective scanline rather than always reporting the first row.
    std::uint32_t bits = 0;
    std::memcpy(&bits, &red[pixelCount - 1], sizeof(bits));
    bits ^= 1U;
    std::memcpy(&red[pixelCount - 1], &bits, sizeof(bits));

    Imf::OutputFile output(outPath.string().c_str(), header, 1);
    Imf::FrameBuffer outFrameBuffer;
    outFrameBuffer.insert(
        "R", Imf::Slice::Make(Imf::FLOAT, red.data(), dataWindow, sizeof(float), rowStride));
    outFrameBuffer.insert(
        "G", Imf::Slice::Make(Imf::FLOAT, green.data(), dataWindow, sizeof(float), rowStride));
    outFrameBuffer.insert(
        "B", Imf::Slice::Make(Imf::FLOAT, blue.data(), dataWindow, sizeof(float), rowStride));
    outFrameBuffer.insert(
        "A", Imf::Slice::Make(Imf::FLOAT, alpha.data(), dataWindow, sizeof(float), rowStride));
    output.setFrameBuffer(outFrameBuffer);
    output.writePixels(static_cast<int>(heightRows));
}

void testPerturbedSingleSampleBit(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("adversarial-sample-bit");
    auto fixture = writeValidFixture(scratch, "base.exr");
    const auto corruptPath = scratch.file("perturbed-sample.exr");
    writeSingleBitPerturbedVariant(fixture.path, corruptPath);

    const output::FlatExrRgba32fLinRec709SceneReopenVerifierV1 verifier;
    const auto result =
        verifier.verify(corruptPath, fixture.source.processIdentity, fixture.source.report, {});
    const auto dataWindow = fixture.source.frame->processImage().descriptor()->dataWindow();
    const auto lastRow =
        dataWindow.originY() + static_cast<std::int64_t>(dataWindow.extent().height()) - 1;
    expectations.expect(
        result.status() == output::FlatExrVerifyStatusV1::Failed &&
            result.diagnostic().code == output::FlatExrVerifyErrorCodeV1::SampleMismatch &&
            result.diagnostic().scanlineY.has_value() && *result.diagnostic().scanlineY == lastRow,
        "a single perturbed sample bit is caught and the failing scanline is named exactly");
}

void testTruncatedFile(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("adversarial-truncated");
    auto fixture = writeValidFixture(scratch, "base.exr");
    const auto corruptPath = scratch.file("truncated.exr");
    std::filesystem::copy_file(fixture.path, corruptPath);

    support::ExrHeaderBytes bytes(corruptPath);
    bytes.truncateTo(bytes.headerEnd() / 2); // well inside the header, before its terminator
    bytes.save();

    const output::FlatExrRgba32fLinRec709SceneReopenVerifierV1 verifier;
    const auto result =
        verifier.verify(corruptPath, fixture.source.processIdentity, fixture.source.report, {});
    expectations.expect(result.status() == output::FlatExrVerifyStatusV1::Failed &&
                            result.diagnostic().code != output::FlatExrVerifyErrorCodeV1::None,
                        "a truncated file is a typed failure, no identity issued");
}

void testWrongVersionFieldFlags(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("adversarial-version-flags");
    auto fixture = writeValidFixture(scratch, "base.exr");
    const auto corruptPath = scratch.file("wrong-version-flags.exr");
    std::filesystem::copy_file(fixture.path, corruptPath);

    support::ExrHeaderBytes bytes(corruptPath);
    // Byte offset 5 holds bits 8-15 of the version field; bit 1 there is TILED_FLAG (0x0200).
    bytes.patchByte(5, static_cast<unsigned char>(bytes.byteAt(5) | 0x02U));
    bytes.save();

    const output::FlatExrRgba32fLinRec709SceneReopenVerifierV1 verifier;
    const auto result =
        verifier.verify(corruptPath, fixture.source.processIdentity, fixture.source.report, {});
    expectations.expect(
        result.status() == output::FlatExrVerifyStatusV1::Failed &&
            result.diagnostic().code == output::FlatExrVerifyErrorCodeV1::InvalidVersionField,
        "a set feature flag (tiled) in the version field is rejected before header parsing");
}

void testCancellationMidVerify(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("cancel-mid-verify");
    // A wide image so each scanline is a meaningful fraction of the verifier's 16 MiB streaming
    // chunk cap; at the maximum permitted width (32768) a chunk is 32 rows, so kHeight below spans
    // several chunks, giving a deterministic chunk boundary to block on.
    constexpr std::uint32_t kWidth = 32'768;
    constexpr std::uint32_t kHeight = 200;
    std::vector<render::Rgba32f> pixels(std::size_t{kWidth} * kHeight,
                                        render::Rgba32f::transparent());
    auto source =
        support::prepareSource({support::frameIdentity(support::identityPlan(
                                    kWidth, kHeight, core::PixelAspectRatio::square())),
                                support::image(support::window(0, 0, kWidth, kHeight),
                                               support::window(0, 0, kWidth, kHeight),
                                               core::PixelAspectRatio::square(), pixels)});
    const auto path = scratch.file("large.exr");
    const output::FlatExrRgba32fLinRec709SceneWriterV1 writer;
    const auto written = writer.write(*source.frame, path, {});
    if (written.status() != output::FlatExrWriteStatusV1::Written) {
        std::abort();
    }

    runtime::TaskSchedulerConfig config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    runtime::TaskScheduler scheduler(config);
    std::mutex mutex;
    std::condition_variable condition;
    bool reachedRow = false;
    bool released = false;
    std::atomic_bool cancelled = false;
    std::atomic_bool verified = false;
    const output::FlatExrRgba32fLinRec709SceneReopenVerifierV1 verifier;

    auto submission = scheduler.submit<void>(
        runtime::TaskRequest(
            "Flat EXR verify cancellation",
            {.kind = runtime::TaskOwnerKind::Composition, .id = runtime::TaskOwnerId::fromRaw(1)}),
        [&](runtime::TaskContext& context) {
            const auto result =
                verifier.verify(path, source.processIdentity, source.report, context.cancellation(),
                                [&](const output::FlatExrVerifyScanProgressV1& progress) {
                                    if (progress.completedScanlines == 0) {
                                        return;
                                    }
                                    std::unique_lock lock(mutex);
                                    reachedRow = true;
                                    condition.notify_all();
                                    condition.wait(lock, [&released] { return released; });
                                });
            cancelled.store(result.status() == output::FlatExrVerifyStatusV1::Cancelled,
                            std::memory_order_release);
            verified.store(result.status() == output::FlatExrVerifyStatusV1::Verified,
                           std::memory_order_release);
            return result.status() == output::FlatExrVerifyStatusV1::Cancelled
                       ? runtime::TaskResult<void>::cancelled()
                       : runtime::TaskResult<void>::succeeded();
        });
    {
        std::unique_lock lock(mutex);
        expectations.expect(submission.accepted() &&
                                condition.wait_for(lock, 5s, [&reachedRow] { return reachedRow; }),
                            "cancellation fixture reaches a deterministic scanline-chunk boundary");
    }
    submission.handle.cancel();
    {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::optional<runtime::TaskResult<void>> taskResult;
    while (!taskResult && std::chrono::steady_clock::now() < deadline) {
        taskResult = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expectations.expect(taskResult && taskResult->state() == runtime::TaskState::Cancelled &&
                            cancelled.load(std::memory_order_acquire) &&
                            !verified.load(std::memory_order_acquire),
                        "cancellation mid-verify yields a typed Cancelled result, never Verified "
                        "(no identity issued)");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    expectations.expect(scheduler.isQuiescent(), "cancellation fixture shuts down cleanly");
}

} // namespace

int main() {
    support::Expectations expectations;
    testRoundTrip(expectations);
    testHeaderConformanceRejectsForeignAttribute(expectations);
    testWrongCompressionEnum(expectations);
    testRenamedChannel(expectations);
    testPerturbedSingleSampleBit(expectations);
    testTruncatedFile(expectations);
    testWrongVersionFieldFlags(expectations);
    testCancellationMidVerify(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
