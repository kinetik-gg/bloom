// Tests for PngRgba8SrgbReopenVerifierV1 (issue #107): round trip (write, verify, issue identity;
// semantic determinism AND byte-identical artifacts across two independent runs -- design decision
// 6, claimable for this Bloom-owned encoder over pinned zlib, unlike the EXR case), an independent
// (zlib-direct) decode cross-check, adversarial byte-surgery fixtures (each a typed failure naming
// the defect, no identity issued), and cancellation mid-verify.

#include "png_test_support.hpp"

#include <bloom/output/png_output_adapter.hpp>
#include <bloom/output/png_reopen_verifier.hpp>
#include <bloom/runtime/task_scheduler.hpp>

// zlib is used directly here twice, both deliberately independent of src/output's own writer/
// verifier: (1) the round trip's "independent decode cross-check" (via
// support::independentlyDecodePng) and (2) the "re-encode route" a few adversarial fixtures below
// use (per F1's precedent: raw byte surgery on compressed IDAT bytes is unreliable -- a single
// flipped compressed byte corrupts the whole deflate stream rather than proving one clean defect --
// so those fixtures build a byte-valid replacement file with zlib directly instead).
#include <zlib.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
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
namespace support = bloom_output_png_test_support;

// Opaque, translucent, zero-alpha, and byte-endpoint (0/255) extremes -- design decision 6's
// fixture list -- at a small 3x2 image (matches png_output_adapter_tests.cpp's own copy; kept
// local rather than shared since each test binary owns its own anonymous-namespace helpers, the
// same idiom flat_exr_output_adapter_tests.cpp/flat_exr_reopen_verifier_tests.cpp already follow).
[[nodiscard]] std::vector<render::Rgba8> fixturePixels() {
    return {
        render::Rgba8{255, 0, 0, 255}, render::Rgba8{0, 255, 0, 128},
        render::Rgba8{0, 0, 255, 0},   render::Rgba8{255, 255, 255, 255},
        render::Rgba8{0, 0, 0, 0},     render::Rgba8{17, 34, 51, 200},
    };
}

constexpr std::uint32_t kFixtureWidth = 3;
constexpr std::uint32_t kFixtureHeight = 2;

[[nodiscard]] output::PngRgba8SrgbPreparedStreamV1
preparedStream(const std::vector<render::Rgba8>& pixels, const std::uint32_t width,
               const std::uint32_t height) {
    const auto extent = render::ImageExtent::create(width, height);
    if (!extent) {
        std::abort();
    }
    return {*extent.value(), std::span<const render::Rgba8>(pixels)};
}

struct WrittenFixture final {
    support::PreparedSource source;
    std::filesystem::path path;
};

[[nodiscard]] WrittenFixture writeValidFixture(const support::ScratchDirectory& scratch,
                                               const std::string& fileName,
                                               const output::PngRgba8SrgbPreparedStreamV1& prepared,
                                               const std::uint32_t width,
                                               const std::uint32_t height) {
    auto source = support::prepareSource(support::pngShapedFixture(width, height));
    const auto path = scratch.file(fileName);
    const output::PngRgba8SrgbWriterV1 writer;
    const auto written = writer.write(prepared, path, {});
    if (written.status() != output::PngWriteStatusV1::Written) {
        std::abort();
    }
    return {std::move(source), path};
}

[[nodiscard]] output::PngVerifyResultV1
verifyFixture(const WrittenFixture& fixture, const output::PngRgba8SrgbPreparedStreamV1& prepared) {
    const output::PngRgba8SrgbReopenVerifierV1 verifier;
    return verifier.verify(fixture.path, prepared, fixture.source.processIdentity,
                           fixture.source.report, fixture.source.expectedOcioRevision,
                           fixture.source.displayProcessorIdentity, {});
}

void testRoundTrip(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("round-trip");
    const auto pixels = fixturePixels();
    const auto prepared = preparedStream(pixels, kFixtureWidth, kFixtureHeight);
    auto runA = writeValidFixture(scratch, "run-a.png", prepared, kFixtureWidth, kFixtureHeight);
    auto runB = writeValidFixture(scratch, "run-b.png", prepared, kFixtureWidth, kFixtureHeight);

    const auto resultA = verifyFixture(runA, prepared);
    const auto resultB = verifyFixture(runB, prepared);
    expectations.expect(resultA.status() == output::PngVerifyStatusV1::Verified,
                        "an independent write+verify of a valid fixture is Verified");
    expectations.expect(resultB.status() == output::PngVerifyStatusV1::Verified,
                        "a second independent write+verify of the same fixture is Verified");
    expectations.expect(resultA.status() == output::PngVerifyStatusV1::Verified &&
                            resultB.status() == output::PngVerifyStatusV1::Verified &&
                            resultA.digest() == resultB.digest(),
                        "the kind-1 semantic identity digest is stable across two independent "
                        "write+verify runs (semantic determinism)");

    // Design decision 6: for this Bloom-owned encoder over the pinned qualified zlib, byte-
    // identical artifacts across two independent encode runs over the same prepared stream is
    // claimable and asserted, unlike the EXR case.
    std::ifstream streamA(runA.path, std::ios::binary);
    std::ifstream streamB(runB.path, std::ios::binary);
    const std::vector<char> contentA((std::istreambuf_iterator<char>(streamA)),
                                     std::istreambuf_iterator<char>());
    const std::vector<char> contentB((std::istreambuf_iterator<char>(streamB)),
                                     std::istreambuf_iterator<char>());
    expectations.expect(contentA == contentB,
                        "two independent encode runs over the same prepared stream are "
                        "byte-identical");

    // Independent decode cross-check: a from-scratch zlib-direct reader (never calling this
    // verifier's own internals) proves the encoder's artifact a second way.
    const auto independent = support::independentlyDecodePng(runA.path);
    expectations.expect(independent.ok && independent.everyCrcValid &&
                            independent.everyRowFilterZero,
                        "an independent zlib-direct decode also proves valid CRCs and filter-0 "
                        "rows");
    bool sampleMatch = independent.ok && independent.rgba.size() == pixels.size() * 4;
    if (sampleMatch) {
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            const auto& pixel = pixels[index];
            sampleMatch = sampleMatch && independent.rgba[index * 4 + 0] == pixel.red &&
                          independent.rgba[index * 4 + 1] == pixel.green &&
                          independent.rgba[index * 4 + 2] == pixel.blue &&
                          independent.rgba[index * 4 + 3] == pixel.alpha;
        }
    }
    expectations.expect(sampleMatch,
                        "the independent decode's samples exactly match the prepared stream");
}

void testForeignAncillaryChunk(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("adversarial-foreign-ancillary");
    const auto pixels = fixturePixels();
    const auto prepared = preparedStream(pixels, kFixtureWidth, kFixtureHeight);
    auto fixture = writeValidFixture(scratch, "base.png", prepared, kFixtureWidth, kFixtureHeight);
    const auto corruptPath = scratch.file("foreign-ancillary.png");
    std::filesystem::copy_file(fixture.path, corruptPath);

    support::PngChunkBytes bytes(corruptPath);
    const auto [idatDataOffset, idatLength] = bytes.dataRange("IDAT", 0);
    (void)idatLength;
    bytes.insertChunk(idatDataOffset - 8, "tEXt", {'C', 'o', 'm', 'm', 'e', 'n', 't', 0, 'h', 'i'});
    bytes.save();

    const output::PngRgba8SrgbReopenVerifierV1 verifier;
    const auto result = verifier.verify(corruptPath, prepared, fixture.source.processIdentity,
                                        fixture.source.report, fixture.source.expectedOcioRevision,
                                        fixture.source.displayProcessorIdentity, {});
    expectations.expect(result.status() == output::PngVerifyStatusV1::Failed &&
                            result.diagnostic().code ==
                                output::PngVerifyErrorCodeV1::UnexpectedChunkType &&
                            result.diagnostic().chunkType == "tEXt",
                        "a spliced foreign ancillary chunk (tEXt) is a typed failure naming it, no "
                        "identity issued");
}

void testForeignCriticalChunk(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("adversarial-foreign-critical");
    const auto pixels = fixturePixels();
    const auto prepared = preparedStream(pixels, kFixtureWidth, kFixtureHeight);
    auto fixture = writeValidFixture(scratch, "base.png", prepared, kFixtureWidth, kFixtureHeight);
    const auto corruptPath = scratch.file("foreign-critical.png");
    std::filesystem::copy_file(fixture.path, corruptPath);

    support::PngChunkBytes bytes(corruptPath);
    const auto [idatDataOffset, idatLength] = bytes.dataRange("IDAT", 0);
    (void)idatLength;
    // PLTE is a real critical PNG chunk this v1 profile forbids -- inserted before the first IDAT.
    bytes.insertChunk(idatDataOffset - 8, "PLTE", {0, 0, 0});
    bytes.save();

    const output::PngRgba8SrgbReopenVerifierV1 verifier;
    const auto result = verifier.verify(corruptPath, prepared, fixture.source.processIdentity,
                                        fixture.source.report, fixture.source.expectedOcioRevision,
                                        fixture.source.displayProcessorIdentity, {});
    expectations.expect(
        result.status() == output::PngVerifyStatusV1::Failed &&
            result.diagnostic().code == output::PngVerifyErrorCodeV1::UnexpectedChunkType &&
            result.diagnostic().chunkType == "PLTE",
        "a spliced foreign critical chunk (PLTE) is a typed failure naming it, no identity issued");
}

void testCorruptedCrc(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("adversarial-crc");
    const auto pixels = fixturePixels();
    const auto prepared = preparedStream(pixels, kFixtureWidth, kFixtureHeight);
    auto fixture = writeValidFixture(scratch, "base.png", prepared, kFixtureWidth, kFixtureHeight);
    const auto corruptPath = scratch.file("bad-crc.png");
    std::filesystem::copy_file(fixture.path, corruptPath);

    support::PngChunkBytes bytes(corruptPath);
    const auto [dataOffset, length] = bytes.dataRange("IHDR", 0);
    (void)length;
    // Flip a byte inside IHDR's data without fixing up the CRC.
    bytes.patchByteBreakingCrc(dataOffset,
                               static_cast<unsigned char>(bytes.byteAt(dataOffset) ^ 1U));
    bytes.save();

    const output::PngRgba8SrgbReopenVerifierV1 verifier;
    const auto result = verifier.verify(corruptPath, prepared, fixture.source.processIdentity,
                                        fixture.source.report, fixture.source.expectedOcioRevision,
                                        fixture.source.displayProcessorIdentity, {});
    expectations.expect(result.status() == output::PngVerifyStatusV1::Failed &&
                            result.diagnostic().code ==
                                output::PngVerifyErrorCodeV1::ChunkCrcMismatch &&
                            result.diagnostic().chunkType == "IHDR",
                        "a corrupted CRC is a typed ChunkCrcMismatch failure naming the chunk, no "
                        "identity issued");
}

void testIhdrFieldPerturbation(support::Expectations& expectations) {
    const struct {
        const char* label;
        std::size_t offset;
        unsigned char value;
    } cases[] = {
        {"bit depth", 8, 16},
        {"color type", 9, 2},
        {"interlace method", 12, 1},
    };
    for (const auto& testCase : cases) {
        const support::ScratchDirectory scratch(std::string("adversarial-ihdr-") + testCase.label);
        const auto pixels = fixturePixels();
        const auto prepared = preparedStream(pixels, kFixtureWidth, kFixtureHeight);
        auto fixture =
            writeValidFixture(scratch, "base.png", prepared, kFixtureWidth, kFixtureHeight);
        const auto corruptPath = scratch.file("perturbed.png");
        std::filesystem::copy_file(fixture.path, corruptPath);

        support::PngChunkBytes bytes(corruptPath);
        bytes.patchDataByteWithValidCrc("IHDR", 0, testCase.offset, testCase.value);
        bytes.save();

        const output::PngRgba8SrgbReopenVerifierV1 verifier;
        const auto result = verifier.verify(
            corruptPath, prepared, fixture.source.processIdentity, fixture.source.report,
            fixture.source.expectedOcioRevision, fixture.source.displayProcessorIdentity, {});
        expectations.expect(result.status() == output::PngVerifyStatusV1::Failed &&
                                result.diagnostic().code ==
                                    output::PngVerifyErrorCodeV1::IhdrFieldMismatch,
                            std::string("an IHDR ") + testCase.label +
                                " perturbation (CRC kept valid) is a typed "
                                "IhdrFieldMismatch failure");
    }
}

void testSrgbIntentNonZero(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("adversarial-srgb-intent");
    const auto pixels = fixturePixels();
    const auto prepared = preparedStream(pixels, kFixtureWidth, kFixtureHeight);
    auto fixture = writeValidFixture(scratch, "base.png", prepared, kFixtureWidth, kFixtureHeight);
    const auto corruptPath = scratch.file("bad-intent.png");
    std::filesystem::copy_file(fixture.path, corruptPath);

    support::PngChunkBytes bytes(corruptPath);
    bytes.patchDataByteWithValidCrc("sRGB", 0, 0, 1);
    bytes.save();

    const output::PngRgba8SrgbReopenVerifierV1 verifier;
    const auto result = verifier.verify(corruptPath, prepared, fixture.source.processIdentity,
                                        fixture.source.report, fixture.source.expectedOcioRevision,
                                        fixture.source.displayProcessorIdentity, {});
    expectations.expect(
        result.status() == output::PngVerifyStatusV1::Failed &&
            result.diagnostic().code == output::PngVerifyErrorCodeV1::SrgbIntentMismatch,
        "an sRGB rendering intent other than 0 (CRC kept valid) is a typed SrgbIntentMismatch "
        "failure");
}

void testTruncationMidChunk(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("adversarial-truncated");
    const auto pixels = fixturePixels();
    const auto prepared = preparedStream(pixels, kFixtureWidth, kFixtureHeight);
    auto fixture = writeValidFixture(scratch, "base.png", prepared, kFixtureWidth, kFixtureHeight);
    const auto corruptPath = scratch.file("truncated.png");
    std::filesystem::copy_file(fixture.path, corruptPath);

    support::PngChunkBytes bytes(corruptPath);
    const auto [idatDataOffset, idatLength] = bytes.dataRange("IDAT", 0);
    bytes.truncateTo(idatDataOffset + idatLength / 2); // well inside the IDAT data, before its CRC
    bytes.save();

    const output::PngRgba8SrgbReopenVerifierV1 verifier;
    const auto result = verifier.verify(corruptPath, prepared, fixture.source.processIdentity,
                                        fixture.source.report, fixture.source.expectedOcioRevision,
                                        fixture.source.displayProcessorIdentity, {});
    expectations.expect(result.status() == output::PngVerifyStatusV1::Failed &&
                            result.diagnostic().code == output::PngVerifyErrorCodeV1::TruncatedFile,
                        "a file truncated mid-chunk is a typed TruncatedFile failure, no identity "
                        "issued");
}

void testTrailingBytesAfterIend(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("adversarial-trailing");
    const auto pixels = fixturePixels();
    const auto prepared = preparedStream(pixels, kFixtureWidth, kFixtureHeight);
    auto fixture = writeValidFixture(scratch, "base.png", prepared, kFixtureWidth, kFixtureHeight);
    const auto corruptPath = scratch.file("trailing.png");
    std::filesystem::copy_file(fixture.path, corruptPath);

    support::PngChunkBytes bytes(corruptPath);
    bytes.appendBytes(std::array<unsigned char, 4>{0xDE, 0xAD, 0xBE, 0xEF});
    bytes.save();

    const output::PngRgba8SrgbReopenVerifierV1 verifier;
    const auto result = verifier.verify(corruptPath, prepared, fixture.source.processIdentity,
                                        fixture.source.report, fixture.source.expectedOcioRevision,
                                        fixture.source.displayProcessorIdentity, {});
    expectations.expect(
        result.status() == output::PngVerifyStatusV1::Failed &&
            result.diagnostic().code == output::PngVerifyErrorCodeV1::TrailingBytesAfterIend,
        "trailing bytes after IEND are a typed TrailingBytesAfterIend failure, no identity issued");
}

void appendU32(std::vector<unsigned char>& out, const std::uint32_t value) {
    out.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<unsigned char>(value & 0xFFU));
}

void appendChunk(std::vector<unsigned char>& out, const std::array<char, 4>& type,
                 const std::vector<unsigned char>& data) {
    appendU32(out, static_cast<std::uint32_t>(data.size()));
    for (const char character : type) {
        out.push_back(static_cast<unsigned char>(character));
    }
    out.insert(out.end(), data.begin(), data.end());
    auto crc = crc32_z(0L, reinterpret_cast<const Bytef*>(type.data()), type.size());
    if (!data.empty()) {
        crc = crc32_z(crc, data.data(), data.size());
    }
    appendU32(out, static_cast<std::uint32_t>(crc));
}

// Builds a byte-valid PNG (correct signature/IHDR/sRGB/IEND, one IDAT holding
// deflate(uncompressedPayload) under this preset's exact fixed parameters) with zlib directly --
// the "re-encode route" for adversarial cases pure byte surgery on compressed bytes cannot reach
// cleanly (per F1's ExrHeaderBytes precedent comment).
void writeCustomPng(const std::filesystem::path& path, const std::uint32_t width,
                    const std::uint32_t height,
                    const std::vector<unsigned char>& uncompressedPayload) {
    std::vector<unsigned char> bytes;
    static constexpr std::array<unsigned char, 8> kSignature{137, 80, 78, 71, 13, 10, 26, 10};
    bytes.insert(bytes.end(), kSignature.begin(), kSignature.end());

    std::vector<unsigned char> ihdr;
    appendU32(ihdr, width);
    appendU32(ihdr, height);
    ihdr.push_back(8);
    ihdr.push_back(6);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    appendChunk(bytes, {'I', 'H', 'D', 'R'}, ihdr);
    appendChunk(bytes, {'s', 'R', 'G', 'B'}, {0});

    z_stream stream{};
    if (deflateInit2(&stream, 6, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        std::abort();
    }
    std::vector<unsigned char> compressed(
        deflateBound(&stream, static_cast<uLong>(uncompressedPayload.size())));
    stream.next_in = const_cast<Bytef*>(uncompressedPayload.data());
    stream.avail_in = static_cast<uInt>(uncompressedPayload.size());
    stream.next_out = compressed.data();
    stream.avail_out = static_cast<uInt>(compressed.size());
    if (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
        std::abort();
    }
    compressed.resize(compressed.size() - stream.avail_out);
    deflateEnd(&stream);
    appendChunk(bytes, {'I', 'D', 'A', 'T'}, compressed);
    appendChunk(bytes, {'I', 'E', 'N', 'D'}, {});

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::abort();
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

void testNonzeroRowFilterByte(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("adversarial-row-filter");
    const auto pixels = fixturePixels();
    const auto prepared = preparedStream(pixels, kFixtureWidth, kFixtureHeight);
    auto source = support::prepareSource(support::pngShapedFixture(kFixtureWidth, kFixtureHeight));
    const auto path = scratch.file("nonzero-filter.png");

    std::vector<unsigned char> payload;
    for (std::uint32_t row = 0; row < kFixtureHeight; ++row) {
        // Row 1 declares filter type 1 (Sub) instead of the required 0 (None); the raw sample
        // bytes are left unchanged, which is exactly the defect under test -- the verifier only
        // requires the filter byte itself to be 0, independent of whether the row would decode
        // correctly under a different declared filter.
        payload.push_back(row == 1 ? 1 : 0);
        for (std::uint32_t x = 0; x < kFixtureWidth; ++x) {
            const auto& pixel = pixels[(row * kFixtureWidth) + x];
            payload.push_back(pixel.red);
            payload.push_back(pixel.green);
            payload.push_back(pixel.blue);
            payload.push_back(pixel.alpha);
        }
    }
    writeCustomPng(path, kFixtureWidth, kFixtureHeight, payload);

    const output::PngRgba8SrgbReopenVerifierV1 verifier;
    const auto result =
        verifier.verify(path, prepared, source.processIdentity, source.report,
                        source.expectedOcioRevision, source.displayProcessorIdentity, {});
    expectations.expect(result.status() == output::PngVerifyStatusV1::Failed &&
                            result.diagnostic().code ==
                                output::PngVerifyErrorCodeV1::RowFilterByteNonzero &&
                            result.diagnostic().row.has_value() && *result.diagnostic().row == 1,
                        "a nonzero row filter byte is caught and the failing row is named exactly");
}

void testOversizedIdatExpansion(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("adversarial-zlib-bomb");
    const auto pixels = fixturePixels();
    const auto prepared = preparedStream(pixels, kFixtureWidth, kFixtureHeight);
    auto source = support::prepareSource(support::pngShapedFixture(kFixtureWidth, kFixtureHeight));
    const auto path = scratch.file("oversized.png");

    // Expected inflate size is height*(1+width*4) = 2*(1+12) = 26 bytes. This payload's true
    // (highly compressible, so still a small IDAT chunk) uncompressed size is far larger --
    // exactly the zlib-bomb shape the checked expanded-size ceiling exists to catch.
    const std::vector<unsigned char> oversizedPayload(1U << 16U, 0);
    writeCustomPng(path, kFixtureWidth, kFixtureHeight, oversizedPayload);

    const output::PngRgba8SrgbReopenVerifierV1 verifier;
    const auto result =
        verifier.verify(path, prepared, source.processIdentity, source.report,
                        source.expectedOcioRevision, source.displayProcessorIdentity, {});
    expectations.expect(
        result.status() == output::PngVerifyStatusV1::Failed &&
            result.diagnostic().code == output::PngVerifyErrorCodeV1::IdatExpandedSizeExceeded,
        "an IDAT stream declaring far more decompressed bytes than the checked expected ceiling "
        "(a zlib-bomb shape) is a typed resource failure, no identity issued");
}

void testCancellationMidVerify(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("cancel-mid-verify");
    // A wide image so each row-chunk is a meaningful fraction of the verifier's 16 MiB streaming
    // chunk cap, giving a deterministic chunk boundary to block on during the sample-comparison
    // phase (mirrors flat_exr_reopen_verifier_tests.cpp's testCancellationMidVerify exactly).
    constexpr std::uint32_t kWidth = 32'768;
    constexpr std::uint32_t kHeight = 600;
    std::vector<render::Rgba8> pixels(std::size_t{kWidth} * kHeight, render::Rgba8{9, 8, 7, 255});
    const auto prepared = preparedStream(pixels, kWidth, kHeight);
    auto fixture = writeValidFixture(scratch, "large.png", prepared, kWidth, kHeight);

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
    const output::PngRgba8SrgbReopenVerifierV1 verifier;

    auto submission = scheduler.submit<void>(
        runtime::TaskRequest(
            "PNG verify cancellation",
            {.kind = runtime::TaskOwnerKind::Composition, .id = runtime::TaskOwnerId::fromRaw(1)}),
        [&](runtime::TaskContext& context) {
            const auto result = verifier.verify(
                fixture.path, prepared, fixture.source.processIdentity, fixture.source.report,
                fixture.source.expectedOcioRevision, fixture.source.displayProcessorIdentity,
                context.cancellation(), [&](const output::PngVerifyProgressV1& progress) {
                    if (progress.completedRows == 0) {
                        return;
                    }
                    std::unique_lock lock(mutex);
                    reachedRow = true;
                    condition.notify_all();
                    condition.wait(lock, [&released] { return released; });
                });
            cancelled.store(result.status() == output::PngVerifyStatusV1::Cancelled,
                            std::memory_order_release);
            verified.store(result.status() == output::PngVerifyStatusV1::Verified,
                           std::memory_order_release);
            return result.status() == output::PngVerifyStatusV1::Cancelled
                       ? runtime::TaskResult<void>::cancelled()
                       : runtime::TaskResult<void>::succeeded();
        });
    {
        std::unique_lock lock(mutex);
        expectations.expect(submission.accepted() &&
                                condition.wait_for(lock, 5s, [&reachedRow] { return reachedRow; }),
                            "cancellation fixture reaches a deterministic row-chunk boundary");
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
    testForeignAncillaryChunk(expectations);
    testForeignCriticalChunk(expectations);
    testCorruptedCrc(expectations);
    testIhdrFieldPerturbation(expectations);
    testSrgbIntentNonZero(expectations);
    testTruncationMidChunk(expectations);
    testTrailingBytesAfterIend(expectations);
    testNonzeroRowFilterByte(expectations);
    testOversizedIdatExpansion(expectations);
    testCancellationMidVerify(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
