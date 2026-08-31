// Tests for FlatExrRgba32fLinRec709SceneWriterV1 (issue #99): header conformance (independently
// decoded), conversion edges (signed-32 window bounds, pixel-aspect rounding), and cancellation
// mid-write. Round-trip and adversarial reopen-verification tests live in
// flat_exr_reopen_verifier_tests.cpp -- they need both the writer and the verifier together.

#include "flat_exr_preset_contract.hpp"
#include "flat_exr_test_support.hpp"
#include "output_analysis_numeric.hpp"
// Module-private (see flat_exr_output_adapter.cpp's comment); this test target gets
// src/output/ on its include path via CMakeLists.txt for exactly this quoted include, only to
// read the frozen kFlatExrRec709D65ChromaticitiesBitsV1 constant for independent-decode
// comparison -- it never uses this header's writer/verifier seam types.
#include "output_semantic_identity.hpp"

#include <bloom/output/flat_exr_output_adapter.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/output/output_analysis_analyzer.hpp>
#include <bloom/output/output_facet_descriptor.hpp>
#include <bloom/runtime/task_scheduler.hpp>

// Independent decode for header conformance: a real client reading this writer's output, distinct
// from the reopen verifier under test elsewhere. OpenEXR/Imath types are confined to this test
// translation unit, same boundary as the production adapter.
#include <ImfChannelList.h>
#include <ImfChromaticities.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfHeader.h>
#include <ImfInputFile.h>
#include <ImfStandardAttributes.h>
#include <ImfStringAttribute.h>

#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
namespace core = bloom::core;
namespace output = bloom::output;
namespace render = bloom::render;
namespace runtime = bloom::runtime;
namespace support = bloom_output_flat_exr_test_support;

void testHeaderConformance(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("header-conformance");
    const auto destination = scratch.file("conformance.exr");
    auto frame = support::publish(support::roundTripFixture());

    const output::FlatExrRgba32fLinRec709SceneWriterV1 writer;
    const auto written = writer.write(*frame, destination, {});
    expectations.expect(written.status() == output::FlatExrWriteStatusV1::Written,
                        "a valid fixture writes successfully");

    Imf::InputFile input(destination.string().c_str());
    const auto& header = input.header();

    std::set<std::string> names;
    for (auto it = header.begin(); it != header.end(); ++it) {
        names.insert(it.name());
    }
    const std::set<std::string> expected{
        "channels",       "compression",      "dataWindow",         "displayWindow",
        "lineOrder",      "pixelAspectRatio", "screenWindowCenter", "screenWindowWidth",
        "chromaticities", "colorInteropID"};
    expectations.expect(names == expected,
                        "the written header carries exactly the closed ten-attribute allowlist, "
                        "no more and no fewer");

    const auto& channels = header.channels();
    std::vector<std::string> channelOrder;
    for (auto it = channels.begin(); it != channels.end(); ++it) {
        channelOrder.emplace_back(it.name());
        expectations.expect(it.channel().type == Imf::FLOAT && it.channel().xSampling == 1 &&
                                it.channel().ySampling == 1 && !it.channel().pLinear,
                            "each channel is FLOAT 1x1 pLinear=0");
    }
    expectations.expect(channelOrder == std::vector<std::string>({"A", "B", "G", "R"}),
                        "channels are physically ordered A, B, G, R");

    expectations.expect(header.compression() == Imf::ZIP_COMPRESSION,
                        "compression is exactly ZIP_COMPRESSION");
    expectations.expect(header.lineOrder() == Imf::INCREASING_Y, "line order is INCREASING_Y");
    expectations.expect(std::bit_cast<std::uint32_t>(header.screenWindowCenter().x) == 0U &&
                            std::bit_cast<std::uint32_t>(header.screenWindowCenter().y) == 0U,
                        "screenWindowCenter is exactly positive-zero bits");
    expectations.expect(std::bit_cast<std::uint32_t>(header.screenWindowWidth()) == 0x3F800000U,
                        "screenWindowWidth is exactly 1.0f bits");
    expectations.expect(Imf::hasChromaticities(header), "chromaticities attribute is present");
    const auto& chroma = Imf::chromaticities(header);
    const std::array<std::uint32_t, 8> chromaBits{
        std::bit_cast<std::uint32_t>(chroma.red.x),   std::bit_cast<std::uint32_t>(chroma.red.y),
        std::bit_cast<std::uint32_t>(chroma.green.x), std::bit_cast<std::uint32_t>(chroma.green.y),
        std::bit_cast<std::uint32_t>(chroma.blue.x),  std::bit_cast<std::uint32_t>(chroma.blue.y),
        std::bit_cast<std::uint32_t>(chroma.white.x), std::bit_cast<std::uint32_t>(chroma.white.y),
    };
    expectations.expect(chromaBits == output::kFlatExrRec709D65ChromaticitiesBitsV1,
                        "chromaticities are the exact Rec.709/D65 bit patterns from the doc");
    const auto* colorInteropId = header.findTypedAttribute<Imf::StringAttribute>("colorInteropID");
    expectations.expect(colorInteropId != nullptr && colorInteropId->value() == "lin_rec709_scene",
                        "colorInteropID is exactly the UTF-8 bytes lin_rec709_scene");

    // Raw version-field bytes: version 2, all feature flags clear.
    std::ifstream rawStream(destination, std::ios::binary);
    std::array<unsigned char, 8> rawHeader{};
    rawStream.read(reinterpret_cast<char*>(rawHeader.data()), 8);
    const auto versionField = static_cast<std::uint32_t>(rawHeader[4]) |
                              (static_cast<std::uint32_t>(rawHeader[5]) << 8U) |
                              (static_cast<std::uint32_t>(rawHeader[6]) << 16U) |
                              (static_cast<std::uint32_t>(rawHeader[7]) << 24U);
    expectations.expect((versionField & 0xFFU) == 2U && (versionField & 0xFFFFFF00U) == 0U,
                        "version field is 2 with every feature flag clear");
}

void testWindowBoundsAtSigned32Extremes(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("window-bounds");
    const output::FlatExrRgba32fLinRec709SceneWriterV1 writer;

    // "Signed-32 extremes" here means the writer's real, checked domain -- which is narrower than
    // literal INT32_MIN/MAX. The qualified OpenEXR 3.4.15 library itself rejects a window whose
    // coordinate magnitude reaches INT32_MAX/2 (confirmed both empirically and against the
    // vendored OpenEXRCore source's `kLargeVal` check; see flat_exr_preset_contract.hpp's
    // kFlatExrLibraryCoordinateCeilingV1 comment -- reported as a genuine contract/library
    // discrepancy in the F1 task report, not silently narrowed without disclosure). Literal
    // INT32_MAX itself throws "Invalid display window" from Imf::OutputFile, so the accepted
    // extreme this test proves is the writer's actual documented ceiling.
    const auto ceiling = output::detail::kFlatExrLibraryCoordinateCeilingV1;

    // Accepted: a data window whose inclusive max lands exactly on the real ceiling minus one.
    {
        const auto pixelAspect = core::PixelAspectRatio::square();
        const std::array pixels{render::Rgba32f::transparent(), render::Rgba32f::transparent()};
        const auto dataWindow = render::ImageWindow::create(ceiling - 2, 0, 2, 1);
        auto frame = support::publish(
            {support::frameIdentity(support::shellPlan()),
             support::image(*dataWindow.value(), *dataWindow.value(), pixelAspect, pixels)});
        const auto result = writer.write(*frame, scratch.file("extreme-accepted.exr"), {});
        expectations.expect(
            result.status() == output::FlatExrWriteStatusV1::Written,
            "a data window whose inclusive max is exactly the writer's real coordinate ceiling "
            "minus one is accepted");
    }

    // Rejected: a data window one pixel past the real ceiling (still far inside literal int32).
    {
        const auto pixelAspect = core::PixelAspectRatio::square();
        const std::array pixels{render::Rgba32f::transparent(), render::Rgba32f::transparent()};
        const auto dataWindow = render::ImageWindow::create(ceiling - 1, 0, 2, 1);
        auto frame = support::publish(
            {support::frameIdentity(support::shellPlan()),
             support::image(*dataWindow.value(), *dataWindow.value(), pixelAspect, pixels)});
        const auto result = writer.write(*frame, scratch.file("extreme-rejected.exr"), {});
        expectations.expect(
            result.status() == output::FlatExrWriteStatusV1::Failed &&
                result.error() == output::FlatExrWriteErrorCodeV1::WindowOutOfRange &&
                result.destinationRemoved(),
            "a data window one pixel past the writer's real ceiling is rejected before staging, "
            "no partial file (even though it still fits literal signed-32)");
    }
}

void testNonPositiveOrNanPixelAspectRejected(support::Expectations& expectations) {
    // core::PixelAspectRatio's own invariant (positive uint32/uint32) makes a non-positive or NaN
    // ratio unconstructible, so this exercises the writer's shared checked-conversion boundary
    // directly: bloom::output::detail::roundOutputAnalysisPositiveRationalToBinary32V1, the exact
    // function prepareGeometry() calls before staging.
    using output::detail::roundOutputAnalysisPositiveRationalToBinary32V1;
    expectations.expect(
        !roundOutputAnalysisPositiveRationalToBinary32V1({.numerator = 0, .denominator = 4}),
        "a zero numerator (non-positive ratio) is rejected");
    expectations.expect(
        !roundOutputAnalysisPositiveRationalToBinary32V1({.numerator = 4, .denominator = 0}),
        "a zero denominator is rejected");
}

void testPixelAspectExactVersusApproximated(support::Expectations& expectations) {
    // Composes with the already-implemented analyzer facet
    // (bloom::output::analyzeFlatExrRgba32fLinRec709SceneV1,
    // src/output/output_analysis_analyzer.cpp)
    // -- this proves the writer's fixtures drive that existing facet consistently, not a
    // reimplementation of the facet's own exhaustive edge coverage.
    {
        auto prepared = support::prepareSource(support::exactPixelAspectFixture()); // 2/1
        bool foundExact = false;
        for (const auto& facet : prepared.report->view().facets) {
            if (facet.facet == output::OutputFacetIdV1::PixelAspect) {
                foundExact = facet.state == output::OutputPreservationStateV1::Exact;
            }
        }
        expectations.expect(foundExact, "2/1 pixel aspect rounds exactly through binary32");
    }
    {
        auto prepared = support::prepareSource(support::roundTripFixture()); // 4/3
        bool foundApproximated = false;
        for (const auto& facet : prepared.report->view().facets) {
            if (facet.facet == output::OutputFacetIdV1::PixelAspect) {
                foundApproximated =
                    facet.state == output::OutputPreservationStateV1::Approximated &&
                    facet.stableCode == output::OutputFacetStableCodeV1::ExrParRoundedBinary32;
            }
        }
        expectations.expect(foundApproximated,
                            "4/3 pixel aspect is Approximated with exr.par-rounded-binary32, "
                            "both values recorded in the facet descriptors");
    }
}

void testCancellationMidWrite(support::Expectations& expectations) {
    const support::ScratchDirectory scratch("cancel-mid-write");
    const auto destination = scratch.file("cancelled.exr");

    // A wide image so each scanline's byte cost is a meaningful fraction of the writer's 16 MiB
    // streaming-chunk cap (kOutputAdapterMaximumStreamingChunkBytesV1); at the maximum permitted
    // width (32768) a chunk is 32 rows, so kHeight below spans several chunks and
    // writePixels() is called multiple times, giving a deterministic chunk boundary to block on.
    constexpr std::uint32_t kWidth = 32'768;
    constexpr std::uint32_t kHeight = 200;
    std::vector<render::Rgba32f> pixels(std::size_t{kWidth} * kHeight,
                                        render::Rgba32f::transparent());
    auto frame = support::publish({support::frameIdentity(support::shellPlan()),
                                   support::image(support::window(0, 0, kWidth, kHeight),
                                                  support::window(0, 0, kWidth, kHeight),
                                                  core::PixelAspectRatio::square(), pixels)});

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
    const output::FlatExrRgba32fLinRec709SceneWriterV1 writer;

    auto submission = scheduler.submit<void>(
        runtime::TaskRequest(
            "Flat EXR write cancellation",
            {.kind = runtime::TaskOwnerKind::Composition, .id = runtime::TaskOwnerId::fromRaw(1)}),
        [&](runtime::TaskContext& context) {
            const auto result =
                writer.write(*frame, destination, context.cancellation(),
                             [&](const output::FlatExrWriteProgressV1& progress) {
                                 if (progress.completedScanlines == 0) {
                                     return;
                                 }
                                 std::unique_lock lock(mutex);
                                 reachedRow = true;
                                 condition.notify_all();
                                 condition.wait(lock, [&released] { return released; });
                             });
            cancelled.store(result.status() == output::FlatExrWriteStatusV1::Cancelled,
                            std::memory_order_release);
            written.store(result.status() == output::FlatExrWriteStatusV1::Written,
                          std::memory_order_release);
            return result.status() == output::FlatExrWriteStatusV1::Cancelled
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
    testHeaderConformance(expectations);
    testWindowBoundsAtSigned32Extremes(expectations);
    testNonPositiveOrNanPixelAspectRejected(expectations);
    testPixelAspectExactVersusApproximated(expectations);
    testCancellationMidWrite(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
