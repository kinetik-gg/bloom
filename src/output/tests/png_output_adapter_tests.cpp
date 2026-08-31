// Tests for PngRgba8SrgbWriterV1 (issue #107): chunk conformance via an independent (zlib-direct)
// decode, invalid-input/resource-limit rejection, and cancellation mid-write. Round trip,
// determinism, and adversarial reopen-verification tests live in png_reopen_verifier_tests.cpp --
// they need both the writer and the verifier together (mirrors flat_exr_output_adapter_tests.cpp's
// own split rationale).

#include "png_test_support.hpp"

#include <bloom/output/png_output_adapter.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace output = bloom::output;
namespace render = bloom::render;
namespace runtime = bloom::runtime;
namespace support = bloom_output_png_test_support;

// Opaque, translucent, zero-alpha, and byte-endpoint (0/255) extremes -- design decision 6's
// fixture list -- at a small 3x2 image.
[[nodiscard]] std::vector<render::Rgba8> fixturePixels() {
    return {
        render::Rgba8{255, 0, 0, 255}, render::Rgba8{0, 255, 0, 128},
        render::Rgba8{0, 0, 255, 0},   render::Rgba8{255, 255, 255, 255},
        render::Rgba8{0, 0, 0, 0},     render::Rgba8{17, 34, 51, 200},
    };
}

[[nodiscard]] output::PngRgba8SrgbPreparedStreamV1
preparedStream(const std::vector<render::Rgba8>& pixels, const std::uint32_t width,
               const std::uint32_t height) {
    const auto extent = render::ImageExtent::create(width, height);
    if (!extent) {
        std::abort();
    }
    return {*extent.value(), std::span<const render::Rgba8>(pixels)};
}

void testChunkConformanceAndIndependentDecode(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("chunk-conformance");
    const auto pixels = fixturePixels();
    const auto prepared = preparedStream(pixels, 3, 2);
    const auto destination = scratch.file("conformance.png");

    const output::PngRgba8SrgbWriterV1 writer;
    const auto written = writer.write(prepared, destination, {});
    expectations.expect(written.status() == output::PngWriteStatusV1::Written,
                        "a valid fixture writes successfully");

    const auto decoded = support::independentlyDecodePng(destination);
    expectations.expect(decoded.ok, "an independent (zlib-direct) reader parses the written file");
    expectations.expect(
        decoded.chunkTypesInOrder.size() >= 4 && decoded.chunkTypesInOrder.front() == "IHDR" &&
            decoded.chunkTypesInOrder[1] == "sRGB" && decoded.chunkTypesInOrder.back() == "IEND",
        "chunks appear in exactly IHDR, sRGB, IDAT(s), IEND order");
    for (std::size_t index = 2; index + 1 < decoded.chunkTypesInOrder.size(); ++index) {
        expectations.expect(decoded.chunkTypesInOrder[index] == "IDAT",
                            "every chunk between sRGB and IEND is IDAT");
    }
    expectations.expect(decoded.everyCrcValid, "every chunk's CRC-32 is valid");
    expectations.expect(decoded.width == 3 && decoded.height == 2 && decoded.bitDepth == 8 &&
                            decoded.colorType == 6 && decoded.compressionMethod == 0 &&
                            decoded.filterMethod == 0 && decoded.interlaceMethod == 0,
                        "IHDR carries exactly the required version 1 field values");
    expectations.expect(decoded.srgbIntent == 0, "sRGB rendering intent is exactly 0 (perceptual)");
    expectations.expect(decoded.everyRowFilterZero, "every row's filter byte is exactly 0 (None)");

    expectations.expect(decoded.rgba.size() == pixels.size() * 4, "decoded byte count matches");
    bool sampleMatch = decoded.rgba.size() == pixels.size() * 4;
    if (sampleMatch) {
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            const auto& pixel = pixels[index];
            sampleMatch = sampleMatch && decoded.rgba[index * 4 + 0] == pixel.red &&
                          decoded.rgba[index * 4 + 1] == pixel.green &&
                          decoded.rgba[index * 4 + 2] == pixel.blue &&
                          decoded.rgba[index * 4 + 3] == pixel.alpha;
        }
    }
    expectations.expect(sampleMatch,
                        "independently decoded RGBA8 samples exactly match the prepared stream "
                        "(opaque/translucent/zero-alpha/extreme fixture)");
}

void testInvalidPreparedStreamRejected(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("invalid-prepared-stream");
    const auto pixels = fixturePixels(); // 6 pixels, but claim 3x3 (9) below.
    const auto extent = render::ImageExtent::create(3, 3);
    if (!extent) {
        std::abort();
    }
    const output::PngRgba8SrgbPreparedStreamV1 prepared{*extent.value(),
                                                        std::span<const render::Rgba8>(pixels)};
    const auto destination = scratch.file("mismatch.png");

    const output::PngRgba8SrgbWriterV1 writer;
    const auto result = writer.write(prepared, destination, {});
    expectations.expect(result.status() == output::PngWriteStatusV1::Failed &&
                            result.error() == output::PngWriteErrorCodeV1::InvalidPreparedStream &&
                            result.destinationRemoved(),
                        "a pixel-count/dimension mismatch is a typed InvalidPreparedStream "
                        "failure, no partial file");
    expectations.expect(!std::filesystem::exists(destination), "no file was created");
}

void testResourceLimitExceededRejected(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("resource-limit");
    // Individually within the per-dimension ceiling (32768) but their product exceeds the pixel-
    // count ceiling (67108864). The claimed pixel span is never dereferenced -- write() rejects
    // before any pixel access once geometry validation fails -- so a null-backed span of the
    // right claimed size is safe and avoids allocating an impractically large real buffer.
    const auto extent = render::ImageExtent::create(32'768, 2'049);
    if (!extent) {
        std::abort();
    }
    const output::PngRgba8SrgbPreparedStreamV1 prepared{
        *extent.value(), std::span<const render::Rgba8>(static_cast<const render::Rgba8*>(nullptr),
                                                        std::size_t{32'768} * 2'049)};
    const auto destination = scratch.file("oversized.png");

    const output::PngRgba8SrgbWriterV1 writer;
    const auto result = writer.write(prepared, destination, {});
    expectations.expect(result.status() == output::PngWriteStatusV1::Failed &&
                            result.error() == output::PngWriteErrorCodeV1::ResourceLimitExceeded &&
                            result.destinationRemoved(),
                        "a pixel count over the closed version 1 ceiling is a typed "
                        "ResourceLimitExceeded failure, no partial file");
}

void testCancellationMidWrite(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("cancel-mid-write");
    const auto destination = scratch.file("cancelled.png");

    // A wide image so each row-chunk's byte cost is a meaningful fraction of the writer's 16 MiB
    // streaming-chunk cap (kOutputAdapterMaximumStreamingChunkBytesV1); at the maximum permitted
    // width (32768) a chunk is roughly 128 rows, so kHeight below spans several chunks, giving a
    // deterministic chunk boundary to block on (mirrors flat_exr_output_adapter_tests.cpp's
    // testCancellationMidWrite exactly).
    constexpr std::uint32_t kWidth = 32'768;
    constexpr std::uint32_t kHeight = 600;
    std::vector<render::Rgba8> pixels(std::size_t{kWidth} * kHeight, render::Rgba8{1, 2, 3, 255});
    const auto prepared = preparedStream(pixels, kWidth, kHeight);

    runtime::TaskSchedulerConfig config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    runtime::TaskScheduler scheduler(config);
    std::mutex mutex;
    std::condition_variable condition;
    bool reachedRow = false;
    bool released = false;
    std::atomic_bool cancelled = false;
    std::atomic_bool written = false;
    const output::PngRgba8SrgbWriterV1 writer;

    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("PNG write cancellation", {.kind = runtime::TaskOwnerKind::Composition,
                                                        .id = runtime::TaskOwnerId::fromRaw(1)}),
        [&](runtime::TaskContext& context) {
            const auto result =
                writer.write(prepared, destination, context.cancellation(),
                             [&](const output::PngWriteProgressV1& progress) {
                                 if (progress.completedRows == 0) {
                                     return;
                                 }
                                 std::unique_lock lock(mutex);
                                 reachedRow = true;
                                 condition.notify_all();
                                 condition.wait(lock, [&released] { return released; });
                             });
            cancelled.store(result.status() == output::PngWriteStatusV1::Cancelled,
                            std::memory_order_release);
            written.store(result.status() == output::PngWriteStatusV1::Written,
                          std::memory_order_release);
            return result.status() == output::PngWriteStatusV1::Cancelled
                       ? runtime::TaskResult<void>::cancelled()
                       : runtime::TaskResult<void>::succeeded();
        });
    {
        std::unique_lock lock(mutex);
        expectations.expect(submission.accepted() &&
                                condition.wait_for(lock, 5s, [&reachedRow] { return reachedRow; }),
                            "cancellation fixture reaches a deterministic chunk boundary");
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
                            !written.load(std::memory_order_acquire),
                        "cancellation mid-write yields a typed Cancelled result, never Written");
    expectations.expect(!std::filesystem::exists(destination),
                        "cancellation mid-write leaves no partial staged file behind");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    expectations.expect(scheduler.isQuiescent(), "cancellation fixture shuts down cleanly");
}

} // namespace

int main() {
    support::Expectations expectations;
    testChunkConformanceAndIndependentDecode(expectations);
    testInvalidPreparedStreamRejected(expectations);
    testResourceLimitExceededRejected(expectations);
    testCancellationMidWrite(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
