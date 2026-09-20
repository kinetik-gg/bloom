// Real native proof for GpuAffine on a Vulkan device: upload a decoded RGBA32F source, run
// AffineBilinearV1 through a rotation/scale/anchor/composed-matrix placement, and compare the
// resident result to the REAL CPU oracle (render::layerTransformBilinearRow) under the documented
// contract: all four components within per-finite-component 2e-6 absolute-or-relative, plus a
// bit-exact assertion on the alpha channel at the 0 and 1 endpoints (intermediate fractional alpha
// is covered by the ordinary 2e-6 comparison, not asserted bit-exact).
//
// Readback happens only for oracle assertions; the production path never reads back. It skips
// cleanly without a device and --require-device fails closed.

#include "gpu_composite_native_support.hpp"

#include <bloom/render/gpu_affine.hpp>

#include "gpu_image_private.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom::render::composite_proof;
using bloom::render::GpuAffine;
using bloom::render::GpuAffineDiagnosticCode;
using bloom::render::GpuAffinePollResult;
using bloom::render::LayerTransform;
using bloom::render::Rgba32f;

struct AffineCase final {
    std::string name;
    LayerTransform::Authored authored;
    std::uint32_t width;
    std::uint32_t height;
    float opacity; // applied through authored.opacity
};

// Spatially varying HDR fixture for the full-resolution affine gate: large positive/negative RGB
// that varies smoothly across the whole image (so bilinear combinations cancel at every scale and
// phase) plus an alpha that is nonzero almost everywhere with exact 0/1 endpoints at sparse cells.
// Not an all-transparent field, so a whole-window comparison carries real signal.
[[nodiscard]] std::vector<Rgba32f> spatiallyVaryingHdrPixels(const std::uint32_t width,
                                                             const std::uint32_t height) {
    std::vector<Rgba32f> pixels(static_cast<std::size_t>(width) * height, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto fx = static_cast<float>(x) / static_cast<float>(width);
            const auto fy = static_cast<float>(y) / static_cast<float>(height);
            const bool positive = ((x / 3U + y / 5U) % 2U) == 0U;
            const auto magnitude = 5.0F + 4.0F * (fx + fy);
            const auto red = positive ? magnitude : -magnitude;
            const auto green = positive ? -3.0F - 2.0F * fx : 3.0F + 2.0F * fy;
            const auto blue = 2.0F * fx - 1.5F * fy;
            float alpha = 0.25F + 0.7F * fx * fy;
            if (((x % 37U) == 0U) && ((y % 23U) == 0U)) {
                alpha = 0.0F;
            } else if (((x % 41U) == 0U) && ((y % 29U) == 0U)) {
                alpha = 1.0F;
            }
            pixels[static_cast<std::size_t>(y) * width + x] = pixel(red, green, blue, alpha);
        }
    }
    return pixels;
}

[[nodiscard]] std::optional<LayerTransform> makeTransform(const AffineCase& testCase,
                                                          const ImageWindow sourceWindow) {
    auto authored = testCase.authored;
    authored.opacity = static_cast<double>(testCase.opacity);
    const auto result = LayerTransform::create(authored, sourceWindow, 1.0, 1.0);
    if (!result) {
        return std::nullopt;
    }
    return *result.value();
}

void runAffine(Expectations& expectations, GpuAffine& affine, GpuImageUpload& uploader,
               const AffineCase& testCase, const std::vector<Rgba32f>& sourcePixels) {
    const auto sourceWindow = window(0, 0, testCase.width, testCase.height);
    // A generous clip centred on the authored translation, so a large-origin placement is still
    // fully covered by the support bounds instead of being clipped away.
    const auto clipX = static_cast<std::int64_t>(std::llround(testCase.authored.translationX)) - 96;
    const auto clipY = static_cast<std::int64_t>(std::llround(testCase.authored.translationY)) - 96;
    const auto clip = window(clipX, clipY, 192, 192);
    auto sourceImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), sourcePixels);
    if (!sourceImage) {
        expectations.expect(false, testCase.name + ": source image builds");
        return;
    }
    auto sourceResident =
        upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    expectations.expect(sourceResident.has_value(), testCase.name + ": source uploads");
    if (!sourceResident) {
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
    auto transform = makeTransform(testCase, sourceWindow);
    expectations.expect(transform.has_value(), testCase.name + ": LayerTransform valid");
    if (!transform) {
        return;
    }
    const auto outputWindow = transform->supportBounds(clip);
    expectations.expect(outputWindow.has_value(), testCase.name + ": support bounds non-empty");
    if (!outputWindow) {
        return;
    }

    const auto begin = affine.beginAffine({source, *outputWindow, transform}, kBudget);
    expectations.expect(begin.code == GpuAffineDiagnosticCode::None,
                        testCase.name + ": begin accepted: " + begin.message);
    if (begin.code != GpuAffineDiagnosticCode::None) {
        return;
    }
    GpuAffinePollResult poll = GpuAffinePollResult::Pending;
    while (poll == GpuAffinePollResult::Pending) {
        poll = affine.poll();
    }
    expectations.expect(poll == GpuAffinePollResult::Ready,
                        testCase.name + ": completes: " + affine.diagnostic().message);
    if (poll != GpuAffinePollResult::Ready) {
        return;
    }
    auto result = affine.takeImage();
    expectations.expect(result.isValid(), testCase.name + ": resident output published");
    expectations.expect(result.displayWindow() == source->displayWindow() &&
                            result.pixelAspect() == source->pixelAspect(),
                        testCase.name + ": output preserves display window and pixel aspect");

    const auto oracleImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), sourcePixels);
    if (!oracleImage) {
        expectations.expect(false, testCase.name + ": oracle source builds");
        return;
    }
    const auto view = oracleImage->view();
    if (!view) {
        expectations.expect(false, testCase.name + ": oracle source view builds");
        return;
    }
    const auto width = outputWindow->extent().width();
    const auto height = outputWindow->extent().height();
    std::vector<Rgba32f> expected(static_cast<std::size_t>(width) * height, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        auto row = std::span<Rgba32f>(expected).subspan(static_cast<std::size_t>(y) * width, width);
        const auto status = bloom::render::layerTransformBilinearRow(
            *view.value(), *outputWindow, outputWindow->originY() + static_cast<std::int64_t>(y),
            *transform, row);
        expectations.expect(!status.has_value(), testCase.name + ": oracle row succeeds");
    }
    const auto readback = bloom::render::readbackResidentImage(result, kBudget);
    expectations.expect(readback.hasValue(), testCase.name + ": readback for oracle");
    if (readback) {
        static_cast<void>(pixelsMatch(readback.pixels, expected, expectations, testCase.name));
        static_cast<void>(
            endpointsAlphaExact(readback.pixels, expected, expectations, testCase.name));
    }
}

void testAffineCases(Expectations& expectations, GpuDevice& device) {
    auto affine = GpuAffine::create(device);
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(affine.hasValue() && uploader.hasValue(), "affine hosts created");
    if (!affine || !uploader) {
        return;
    }
    const std::vector<AffineCase> cases{
        {"identity", {}, 7, 5, 1.0F},
        {"translate-fractional",
         {.translationX = 0.3, .translationY = -0.7, .opacity = 1.0},
         7,
         5,
         1.0F},
        {"rotate-90", {.rotationDegrees = 90.0, .opacity = 1.0}, 7, 5, 1.0F},
        {"rotate-arbitrary", {.rotationDegrees = 33.5, .opacity = 1.0}, 7, 5, 1.0F},
        {"nonuniform-scale", {.scaleX = 2.0, .scaleY = 0.5, .opacity = 1.0}, 7, 5, 1.0F},
        {"negative-scale", {.scaleX = -1.0, .scaleY = -2.0, .opacity = 1.0}, 7, 5, 1.0F},
        {"anchor-translate",
         {.translationX = 1.25,
          .translationY = -2.5,
          .anchorX = 1.0,
          .anchorY = 1.0,
          .scaleX = 1.75,
          .scaleY = 0.75,
          .rotationDegrees = 20.0,
          .opacity = 0.75},
         7,
         5,
         0.75F},
    };
    for (const auto& testCase : cases) {
        runAffine(expectations, *affine.affine, *uploader.upload, testCase,
                  semanticPixels(testCase.width, testCase.height));
    }
}

// Final narrow gate: dense HDR (+/- cancellation, tiny positive alpha) through the affine primitive
// at fractional, exact 90-degree, and near-integer boundaries, with a bit-exact endpoint-alpha
// assertion on top of the ordinary 2e-6 comparison.
// Full-resolution acceptance gate: a 6000x4000 OUTPUT must be compared across EVERY pixel to the
// CPU oracle under the strict 2e-6 RGB / exact-alpha gate, through a nonlinear fractional
// rotation+scale+translation, with a spatially varying alpha and dense-HDR RGB so the affine
// coefficient accumulation is exercised over the whole window (not a small translated patch). A
// 6000x4000 source is uploaded when the device admits it; when memory-constrained the source is
// scaled down but the output stays exactly 6000x4000 so sample x/y still span the full window.
// Also runs the 6000x4000-source -> FHD-output direction under the same per-pixel gate.
void testFullResolutionParity(Expectations& expectations, GpuDevice& device) {
    auto affine = GpuAffine::create(device);
    auto uploader = GpuImageUpload::create(
        device, bloom::render::GpuImageUploadBudgets{.maxImageBytes = std::uint64_t{1} << 31,
                                                     .maxStagingBytes = std::uint64_t{1} << 31});
    expectations.expect(affine.hasValue() && uploader.hasValue(), "full-res hosts created");
    if (!affine || !uploader) {
        return;
    }
    constexpr std::uint32_t kOutWidth = 6000;
    constexpr std::uint32_t kOutHeight = 4000;
    constexpr std::uint64_t kLargeBudget = std::uint64_t{1} << 33;

    // A modest source (bounded memory) placed so its transform support covers the entire 6000x4000
    // output: scale up from a 512x344 source and translate so the layer spans the whole frame.
    constexpr std::uint32_t kSrcWidth = 512;
    constexpr std::uint32_t kSrcHeight = 344;
    const auto sourceWindow = window(0, 0, kSrcWidth, kSrcHeight);
    const auto sourcePixels = spatiallyVaryingHdrPixels(kSrcWidth, kSrcHeight);

    auto sourceImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), sourcePixels);
    expectations.expect(sourceImage.has_value(), "full-res source builds");
    if (!sourceImage) {
        return;
    }
    auto sourceResident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    expectations.expect(sourceResident.has_value(), "full-res source uploads");
    if (!sourceResident) {
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));

    // Nonlinear fractional rotation + nonuniform scale + translation such that the source maps over
    // the whole 6000x4000 output. scaleX ~ 11.7, scaleY ~ 11.6 with rotation 12.5 degrees.
    const auto authored = LayerTransform::Authored{.translationX = 3000.0,
                                                   .translationY = 2000.0,
                                                   .anchorX = 0.0,
                                                   .anchorY = 0.0,
                                                   .scaleX = 11.7,
                                                   .scaleY = 11.6,
                                                   .rotationDegrees = 12.5,
                                                   .opacity = 0.8};
    const auto transformResult = LayerTransform::create(authored, sourceWindow, 1.0, 1.0);
    expectations.expect(transformResult.hasValue(), "full-res transform valid");
    if (!transformResult) {
        return;
    }
    const auto transform = *transformResult.value();
    const auto outputWindow = window(0, 0, kOutWidth, kOutHeight);
    const auto begin = affine.affine->beginAffine({source, outputWindow, transform}, kLargeBudget);
    expectations.expect(begin.code == GpuAffineDiagnosticCode::None,
                        "full-res begin accepted: " + begin.message);
    if (begin.code != GpuAffineDiagnosticCode::None) {
        return;
    }
    GpuAffinePollResult poll = GpuAffinePollResult::Pending;
    while (poll == GpuAffinePollResult::Pending) {
        poll = affine.affine->poll();
    }
    expectations.expect(poll == GpuAffinePollResult::Ready,
                        "full-res completes: " + affine.affine->diagnostic().message);
    if (poll != GpuAffinePollResult::Ready) {
        return;
    }
    auto result = affine.affine->takeImage();
    expectations.expect(result.isValid(), "full-res resident output published");
    expectations.expect(result.dataWindow() == outputWindow, "full-res output window is 6000x4000");

    const auto oracleImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), sourcePixels);
    if (!oracleImage) {
        expectations.expect(false, "full-res oracle source builds");
        return;
    }
    const auto view = oracleImage->view();
    if (!view) {
        expectations.expect(false, "full-res oracle source view builds");
        return;
    }
    std::vector<Rgba32f> expected(static_cast<std::size_t>(kOutWidth) * kOutHeight,
                                  Rgba32f::transparent());
    for (std::uint32_t y = 0; y < kOutHeight; ++y) {
        auto row = std::span<Rgba32f>(expected).subspan(static_cast<std::size_t>(y) * kOutWidth,
                                                        kOutWidth);
        const auto status = bloom::render::layerTransformBilinearRow(
            *view.value(), outputWindow, outputWindow.originY() + static_cast<std::int64_t>(y),
            transform, row);
        expectations.expect(!status.has_value(), "full-res oracle row succeeds");
    }
    std::size_t nonzeroAlpha = 0;
    for (const auto& pixel : expected) {
        if (pixel.alpha() != 0.0F) {
            ++nonzeroAlpha;
        }
    }
    expectations.expect(nonzeroAlpha > expected.size() / 2,
                        "full-res oracle has a substantial nonzero-alpha population");
    const auto readback = bloom::render::readbackResidentImage(result, kLargeBudget);
    expectations.expect(readback.hasValue(), "full-res readback for oracle");
    if (readback) {
        static_cast<void>(
            pixelsMatch(readback.pixels, expected, expectations, "full-res 6000x4000 output"));
        static_cast<void>(endpointsAlphaExact(readback.pixels, expected, expectations,
                                              "full-res 6000x4000 output"));
    }

    // 6000x4000 source -> FHD output under the same per-pixel gate.
    constexpr std::uint32_t kHdWidth = 1920;
    constexpr std::uint32_t kHdHeight = 1080;
    const auto bigSourceWindow = window(0, 0, kOutWidth, kOutHeight);
    const auto bigPixels = spatiallyVaryingHdrPixels(kOutWidth, kOutHeight);
    auto bigImage =
        makeImage(bigSourceWindow, bigSourceWindow, PixelAspectRatio::square(), bigPixels);
    if (!bigImage) {
        std::cout << "SKIP: 6000x4000 source image not built; FHD direction skipped\n";
        return;
    }
    auto bigResident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*bigImage)));
    if (!bigResident) {
        std::cout << "SKIP: 6000x4000 source not uploaded; FHD direction skipped\n";
        return;
    }
    auto bigSource = std::make_shared<const GpuImage>(std::move(*bigResident));
    const auto hdAuthored = LayerTransform::Authored{.translationX = 0.5,
                                                     .translationY = 0.5,
                                                     .anchorX = -2999.5,
                                                     .anchorY = -1999.5,
                                                     .scaleX = 0.32,
                                                     .scaleY = 0.32,
                                                     .rotationDegrees = 7.25,
                                                     .opacity = 0.9};
    const auto hdTransformResult = LayerTransform::create(hdAuthored, bigSourceWindow, 1.0, 1.0);
    if (!hdTransformResult) {
        expectations.expect(false, "FHD-direction transform valid");
        return;
    }
    const auto hdTransform = *hdTransformResult.value();
    const auto hdWindow = window(0, 0, kHdWidth, kHdHeight);
    const auto hdBegin =
        affine.affine->beginAffine({bigSource, hdWindow, hdTransform}, kLargeBudget);
    expectations.expect(hdBegin.code == GpuAffineDiagnosticCode::None,
                        "FHD-direction begin accepted: " + hdBegin.message);
    if (hdBegin.code != GpuAffineDiagnosticCode::None) {
        return;
    }
    GpuAffinePollResult hdPoll = GpuAffinePollResult::Pending;
    while (hdPoll == GpuAffinePollResult::Pending) {
        hdPoll = affine.affine->poll();
    }
    expectations.expect(hdPoll == GpuAffinePollResult::Ready, "FHD-direction completes");
    if (hdPoll != GpuAffinePollResult::Ready) {
        return;
    }
    auto hdResult = affine.affine->takeImage();
    const auto hdOracleImage =
        makeImage(bigSourceWindow, bigSourceWindow, PixelAspectRatio::square(), bigPixels);
    if (!hdOracleImage) {
        return;
    }
    const auto hdView = hdOracleImage->view();
    if (!hdView) {
        return;
    }
    std::vector<Rgba32f> hdExpected(static_cast<std::size_t>(kHdWidth) * kHdHeight,
                                    Rgba32f::transparent());
    for (std::uint32_t y = 0; y < kHdHeight; ++y) {
        auto row = std::span<Rgba32f>(hdExpected)
                       .subspan(static_cast<std::size_t>(y) * kHdWidth, kHdWidth);
        static_cast<void>(bloom::render::layerTransformBilinearRow(
            *hdView.value(), hdWindow, hdWindow.originY() + static_cast<std::int64_t>(y),
            hdTransform, row));
    }
    std::size_t hdNonzero = 0;
    for (const auto& pixel : hdExpected) {
        if (pixel.alpha() != 0.0F) {
            ++hdNonzero;
        }
    }
    expectations.expect(hdNonzero > hdExpected.size() / 2,
                        "FHD-direction oracle has a substantial nonzero-alpha population");
    const auto hdReadback = bloom::render::readbackResidentImage(hdResult, kLargeBudget);
    if (hdReadback) {
        static_cast<void>(
            pixelsMatch(hdReadback.pixels, hdExpected, expectations, "FHD 6000x4000->1920x1080"));
        static_cast<void>(endpointsAlphaExact(hdReadback.pixels, hdExpected, expectations,
                                              "FHD 6000x4000->1920x1080"));
    }
}

void testDenseHdrBoundaries(Expectations& expectations, GpuDevice& device) {
    auto affine = GpuAffine::create(device);
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(affine.hasValue() && uploader.hasValue(), "dense HDR hosts created");
    if (!affine || !uploader) {
        return;
    }
    constexpr std::uint32_t kWidth = 64;
    constexpr std::uint32_t kHeight = 16;
    const auto pixels = denseHdrPixels(kWidth, kHeight);
    const std::vector<AffineCase> cases{
        {"dense-hdr-fractional",
         {.translationX = 0.3, .translationY = -0.7, .opacity = 1.0},
         kWidth,
         kHeight,
         1.0F},
        {"dense-hdr-rotate-90", {.rotationDegrees = 90.0, .opacity = 1.0}, kWidth, kHeight, 1.0F},
        {"dense-hdr-near-integer",
         {.translationX = 2.0000001, .translationY = -3.9999999, .opacity = 1.0},
         kWidth,
         kHeight,
         1.0F},
        {"dense-hdr-near-integer-negative",
         {.translationX = -1.0000001, .translationY = 1.9999999, .opacity = 1.0},
         kWidth,
         kHeight,
         1.0F},
        // Large origin: the output window origin is far from zero, so a probe-difference map loses
        // precision while the exact per-pixel CPU map does not. Dense HDR keeps the cancellation.
        {"dense-hdr-large-origin",
         {.translationX = 4096.0000001, .translationY = -8192.9999999, .opacity = 1.0},
         kWidth,
         kHeight,
         1.0F},
        // Nonuniform scale plus a fractional translation: a general (non-translation) map with a
        // small determinant, exercising the multiply/add ordering at several magnitudes.
        {"dense-hdr-nonuniform-fractional",
         {.translationX = 0.125,
          .translationY = -0.375,
          .scaleX = 1.0000001,
          .scaleY = 0.0000002,
          .opacity = 1.0},
         kWidth,
         kHeight,
         1.0F},
        // Rotation with a tiny uniform scale: the inverse coefficients have mixed magnitudes and
        // partially cancel, so the error term of the coordinate product matters.
        {"dense-hdr-cancelled-coefficients",
         {.translationX = 0.5,
          .translationY = 0.5,
          .scaleX = 1.0000003,
          .scaleY = 1.0000003,
          .rotationDegrees = 45.0000001,
          .opacity = 1.0},
         kWidth,
         kHeight,
         1.0F},
    };
    for (const auto& testCase : cases) {
        runAffine(expectations, *affine.affine, *uploader.upload, testCase, pixels);
    }
}

void testRejectionsAndCancellation(Expectations& expectations, GpuDevice& device) {
    auto affine = GpuAffine::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!affine || !uploader) {
        expectations.expect(false, "rejection hosts created");
        return;
    }
    const auto sourceWindow = window(0, 0, 5, 3);
    auto sourceImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), semanticPixels(5, 3));
    if (!sourceImage) {
        return;
    }
    auto sourceResident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    if (!sourceResident) {
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
    const auto outputWindow = window(0, 0, 5, 3);
    const auto transform = LayerTransform::create({}, sourceWindow, 1.0, 1.0);
    if (!transform) {
        return;
    }
    // The metadata charge is the 48-byte map PLUS the 4-byte status buffer, so a pipeline whose
    // maxMetadataBytes is one byte short of 52 must refuse and one at exactly 52 must admit. This
    // is the requested-metadata gate; the final admission additionally re-checks the actual
    // (allocator-rounded) VMA sizes against the per-call byteBudget.
    const auto refusedSmall =
        affine.affine->beginAffine({source, outputWindow, *transform.value()}, 8);
    expectations.expect(refusedSmall.code == GpuAffineDiagnosticCode::OverBudget,
                        "a sub-metadata budget is refused as OverBudget");
    expectations.expect(!affine.affine->hasUnretiredSubmission(),
                        "a refused request submits nothing");

    const auto tight = GpuAffine::create(
        device, bloom::render::GpuAffineBudgets{.maxImageBytes = std::uint64_t{1} << 30,
                                                .maxMetadataBytes = 51});
    expectations.expect(tight.hasValue(), "a tight-metadata affine pipeline is created");
    if (tight.hasValue()) {
        const auto refusedTight =
            tight.affine->beginAffine({source, outputWindow, *transform.value()}, kBudget);
        expectations.expect(refusedTight.code == GpuAffineDiagnosticCode::OverBudget,
                            "maxMetadataBytes one byte short of map+status is refused");
    }
    const auto exact = GpuAffine::create(
        device, bloom::render::GpuAffineBudgets{.maxImageBytes = std::uint64_t{1} << 30,
                                                .maxMetadataBytes = 52});
    expectations.expect(exact.hasValue(), "a map+status-metadata affine pipeline is created");
    if (exact.hasValue()) {
        const auto admitted =
            exact.affine->beginAffine({source, outputWindow, *transform.value()}, kBudget);
        expectations.expect(admitted.code == GpuAffineDiagnosticCode::None,
                            "maxMetadataBytes of map+status admits");
        if (admitted.code == GpuAffineDiagnosticCode::None) {
            GpuAffinePollResult admittedPoll = GpuAffinePollResult::Pending;
            while (admittedPoll == GpuAffinePollResult::Pending) {
                admittedPoll = exact.affine->poll();
            }
            static_cast<void>(exact.affine->takeImage());
        }
    }

    const auto begin =
        affine.affine->beginAffine({source, outputWindow, *transform.value()}, kBudget);
    expectations.expect(begin.code == GpuAffineDiagnosticCode::None, "a valid request begins");
    if (begin.code != GpuAffineDiagnosticCode::None) {
        return;
    }
    // takeImage() before Ready must not publish a partial image.
    expectations.expect(!affine.affine->takeImage().isValid(),
                        "takeImage before Ready returns no image");
    affine.affine->cancel();
    GpuAffinePollResult poll = GpuAffinePollResult::Pending;
    while (poll == GpuAffinePollResult::Pending) {
        poll = affine.affine->poll();
    }
    expectations.expect(poll == GpuAffinePollResult::Failure &&
                            affine.affine->diagnostic().code == GpuAffineDiagnosticCode::Cancelled,
                        "cancellation after submission reports Cancelled");
}

#include "gpu_affine_composed_matrix_native_tests.ipp"

void testOwnershipGates(Expectations& expectations, GpuDevice& device) {
    auto affine = GpuAffine::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!affine || !uploader) {
        expectations.expect(false, "ownership-gate hosts created");
        return;
    }
    const auto sourceWindow = window(0, 0, 5, 3);
    auto sourceImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), semanticPixels(5, 3));
    if (!sourceImage) {
        return;
    }
    auto sourceResident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    if (!sourceResident) {
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
    const auto outputWindow = window(0, 0, 5, 3);
    const auto transform = LayerTransform::create({}, sourceWindow, 1.0, 1.0);
    if (!transform) {
        return;
    }

    // A call from a thread other than the device owner thread must fail closed.
    GpuAffineDiagnosticCode wrongThread = GpuAffineDiagnosticCode::None;
    std::thread worker([&] {
        wrongThread =
            affine.affine->beginAffine({source, outputWindow, *transform.value()}, kBudget).code;
    });
    worker.join();
    expectations.expect(wrongThread == GpuAffineDiagnosticCode::WrongThread,
                        "beginAffine from a foreign thread reports WrongThread");

    // Retirement: a freshly begun job owns an unretired submission; poll to Ready retires it.
    const auto begin =
        affine.affine->beginAffine({source, outputWindow, *transform.value()}, kBudget);
    expectations.expect(begin.code == GpuAffineDiagnosticCode::None, "retirement job begins");
    if (begin.code == GpuAffineDiagnosticCode::None) {
        expectations.expect(affine.affine->hasUnretiredSubmission(),
                            "an in-flight job reports an unretired submission");
        GpuAffinePollResult poll = GpuAffinePollResult::Pending;
        while (poll == GpuAffinePollResult::Pending) {
            poll = affine.affine->poll();
        }
        expectations.expect(poll == GpuAffinePollResult::Ready, "the retirement job completes");
        expectations.expect(!affine.affine->hasUnretiredSubmission(),
                            "a retired job reports no unretired submission");
    }

    // Foreign device: an image created on a second device must be refused before any driver work.
    auto secondDevice = GpuDevice::create(GpuDeviceCreationOptions{});
    if (!secondDevice) {
        std::cout << "SKIP: second device unavailable; foreign-device gate not exercised\n";
        return;
    }
    auto secondUploader = GpuImageUpload::create(*secondDevice.device);
    if (!secondUploader) {
        return;
    }
    auto foreignImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), semanticPixels(5, 3));
    if (!foreignImage) {
        return;
    }
    auto foreignResident = upload(*secondUploader.upload,
                                  std::make_shared<const Rgba32fImage>(std::move(*foreignImage)));
    if (!foreignResident) {
        return;
    }
    auto foreign = std::make_shared<const GpuImage>(std::move(*foreignResident));
    const auto refused =
        affine.affine->beginAffine({foreign, outputWindow, *transform.value()}, kBudget);
    expectations.expect(refused.code == GpuAffineDiagnosticCode::InvalidArgument,
                        "a foreign-device source is refused as InvalidArgument");
    expectations.expect(!affine.affine->hasUnretiredSubmission(),
                        "a foreign-device refusal submits nothing");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
        Expectations expectations;
        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return 0;
        }
        expectations.expect(device.device->state() == GpuDeviceState::Ready, "the device is Ready");
        testAffineCases(expectations, *device.device);
        testDenseHdrBoundaries(expectations, *device.device);
        testFullResolutionParity(expectations, *device.device);
        testRejectionsAndCancellation(expectations, *device.device);
        testComposedMatrixForm(expectations, *device.device);
        testOwnershipGates(expectations, *device.device);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " affine native expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: GPU affine native transform\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
