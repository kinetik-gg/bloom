// Solid, CoveredSolid and upload/translation/source-over parity for the resident-preview
// qualification. See the public header for the contract; this translation unit is Vulkan-free.

#include "gpu_resident_preview_qualification_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime::resident_preview_detail {

using core::PixelAspectRatio;
using render::GpuCompositeDiagnosticCode;
using render::GpuDisplayImageReadbackCode;
using render::GpuImage;
using render::GpuImageUploadDiagnosticCode;
using render::GpuResidentDisplayDiagnosticCode;
using render::GpuSolidDiagnosticCode;
using render::GpuSolidParameters;
using render::ImageWindow;
using render::Rgba32f;
using render::Rgba32fImage;
using render::Rgba8;

ProbeResult verifyShaderPins(const core::Sha256Hasher*) noexcept {
    for (const GpuResidentPreviewShaderPin& pin : kGpuResidentPreviewShaderPins) {
        if (pin.name.empty() || pin.spv_sha256.size() != core::kSha256HexCharacters ||
            pin.source_sha256.size() != core::kSha256HexCharacters) {
            return fail(GpuResidentPreviewDiagnosticCode::InternalInvariant,
                        "a pinned shader digest is not a complete SHA-256 hex string");
        }
    }
    return ok();
}

ProbeResult verifySolidParity(ProbeContext& context) noexcept {
    const auto nonSquare = makePixelAspect(4, 3);
    const auto dataWindow = makeWindow(2, -1, 5, 3);
    const auto displayWindow = makeWindow(0, 0, 9, 5);
    if (!nonSquare || !dataWindow || !displayWindow) {
        return fail(GpuResidentPreviewDiagnosticCode::InternalInvariant,
                    "the solid fixture geometry did not build");
    }
    const std::array<core::Color4d, 3> colors{{
        core::Color4d{0.25, 0.5, 0.75, 1.0},
        core::Color4d{-0.25, 4.0, 0.125, 0.5},
        core::Color4d{0.1, -2.0, 8.0, 0.75},
    }};
    const std::uint32_t width = dataWindow->extent().width();
    const std::uint32_t height = dataWindow->extent().height();
    for (const core::Color4d color : colors) {
        const auto pixel = render::solidPixelFromStraightLinearRec709Scene(color);
        if (!pixel) {
            return fail(GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                        "the CPU solid primitive rejected a fixture colour");
        }
        const GpuSolidParameters params{*pixel.value(), *dataWindow, *displayWindow, *nonSquare};
        std::vector<Rgba32f> measured;
        std::shared_ptr<const GpuImage> taken;
        std::string reason;
        const auto ran = runSolidReadback(context, params, measured, taken, reason);
        if (ran.code != GpuResidentPreviewDiagnosticCode::None) {
            return ran;
        }
        std::vector<Rgba32f> expected(static_cast<std::size_t>(width) * height,
                                      Rgba32f::transparent());
        render::fillSolidRow(expected, *pixel.value());
        if (!exactEqual(measured, expected)) {
            return fail(GpuResidentPreviewDiagnosticCode::ParityFailure,
                        "the resident SolidV1 image is not bit-identical to fillSolidRow");
        }
        if (taken->dataWindow() != *dataWindow || taken->displayWindow() != *displayWindow ||
            taken->pixelAspect() != *nonSquare) {
            return fail(GpuResidentPreviewDiagnosticCode::ParityFailure,
                        "the resident SolidV1 geometry/PAR was not preserved");
        }
        hashPixels(context, width, height, measured);
    }
    return ok();
}

ProbeResult verifyCoveredParity(ProbeContext& context) noexcept {
    std::vector<std::uint8_t> coverage(256);
    for (std::size_t index = 0; index < coverage.size(); ++index) {
        coverage[index] = static_cast<std::uint8_t>(index);
    }
    const auto rowWindow = makeWindow(0, 0, 256, 1);
    const auto oddWindow = makeWindow(-3, 5, 7, 3);
    const auto par = makePixelAspect(4, 3);
    if (!rowWindow || !oddWindow || !par) {
        return fail(GpuResidentPreviewDiagnosticCode::InternalInvariant,
                    "the covered fixture geometry did not build");
    }
    std::vector<std::uint8_t> oddCoverage(static_cast<std::size_t>(7) * 3);
    for (std::size_t index = 0; index < oddCoverage.size(); ++index) {
        oddCoverage[index] = static_cast<std::uint8_t>((index * 37U + 5U) & 0xFFU);
    }
    const std::array<core::Color4d, 3> colors{{
        core::Color4d{0.25, 0.5, 0.75, 1.0},
        core::Color4d{-0.25, 4.0, 0.125, 0.5},
        core::Color4d{0.1, -2.0, 8.0, 0.75},
    }};
    const std::array<float, 3> opacities{{0.0F, 0.3F, 1.0F}};
    const auto runCovered = [&](const GpuSolidParameters& base,
                                const std::span<const std::uint8_t> bytes, const float opacity,
                                std::vector<Rgba32f>& out, std::string& reason) -> ProbeResult {
        const auto begin = context.solid.beginCovered(base, bytes, opacity, context.imageBudget);
        if (begin.code != GpuSolidDiagnosticCode::None) {
            if (begin.code == GpuSolidDiagnosticCode::OverBudget) {
                return fail(GpuResidentPreviewDiagnosticCode::OverBudget, begin.message);
            }
            return fail(GpuResidentPreviewDiagnosticCode::NativeFailure, begin.message);
        }
        const auto status =
            waitFor(context.solid, context.isCancelled, context.deadlineNanoseconds, reason);
        if (status != NativeStatus::Ok) {
            return waitFailure(status, reason);
        }
        const auto readback = context.solid.readback();
        if (!readback) {
            return fail(GpuResidentPreviewDiagnosticCode::NativeFailure, readback.message);
        }
        out = readback.pixels;
        return ok();
    };
    for (const core::Color4d color : colors) {
        const auto pixel = render::solidPixelFromStraightLinearRec709Scene(color);
        if (!pixel) {
            return fail(GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                        "the CPU covered fixture did not build");
        }
        for (const float opacity : opacities) {
            std::vector<Rgba32f> measured;
            std::string reason;
            const GpuSolidParameters base{*pixel.value(), *rowWindow, *rowWindow, *par};
            const auto ran = runCovered(base, coverage, opacity, measured, reason);
            if (ran.code != GpuResidentPreviewDiagnosticCode::None) {
                return ran;
            }
            std::vector<Rgba32f> expected(256, Rgba32f::transparent());
            cpuCoverageReference(*pixel.value(), opacity, coverage, 256, 1, expected);
            if (!exactEqual(measured, expected)) {
                return fail(
                    GpuResidentPreviewDiagnosticCode::ParityFailure,
                    "the covered resident row is not bit-identical to the CPU coverage arm");
            }
            hashPixels(context, 256, 1, measured);
        }
        std::vector<Rgba32f> oddMeasured;
        std::string oddReason;
        const GpuSolidParameters oddBase{*pixel.value(), *oddWindow, *oddWindow, *par};
        const auto oddRan = runCovered(oddBase, oddCoverage, 0.75F, oddMeasured, oddReason);
        if (oddRan.code != GpuResidentPreviewDiagnosticCode::None) {
            return oddRan;
        }
        std::vector<Rgba32f> oddExpected(static_cast<std::size_t>(7) * 3, Rgba32f::transparent());
        cpuCoverageReference(*pixel.value(), 0.75F, oddCoverage, 7, 3, oddExpected);
        if (!exactEqual(oddMeasured, oddExpected)) {
            return fail(GpuResidentPreviewDiagnosticCode::ParityFailure,
                        "the odd covered resident frame differs from the CPU coverage arm");
        }
        const render::GpuImage* image = context.solid.image();
        if (image == nullptr || image->dataWindow() != *oddWindow || image->pixelAspect() != *par) {
            return fail(GpuResidentPreviewDiagnosticCode::ParityFailure,
                        "the odd covered geometry/PAR was not preserved");
        }
        hashPixels(context, 7, 3, oddMeasured);
        static_cast<void>(context.solid.takeImage());
    }
    return ok();
}

ProbeResult verifyUploadAndCompositeParity(ProbeContext& context) noexcept {
    constexpr std::uint32_t kWidth = 16;
    constexpr std::uint32_t kHeight = 4;
    const auto sourceWindow = makeWindow(100, -50, kWidth, kHeight);
    const auto sourceDisplay = makeWindow(7, 3, 20, 10);
    const auto par = makePixelAspect(4, 3);
    const auto checker = detail::makeResidentPreviewCheckerPixels(kWidth, kHeight);
    if (!sourceWindow || !sourceDisplay || !par || !checker) {
        return fail(GpuResidentPreviewDiagnosticCode::InternalInvariant,
                    "the upload/composite fixtures did not build");
    }
    auto sourceImage = buildImage(*sourceWindow, *sourceDisplay, *par, *checker);
    if (!sourceImage) {
        return fail(GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                    "the source CPU image did not build");
    }
    std::shared_ptr<const GpuImage> sourceResident;
    {
        std::string reason;
        auto source = std::make_shared<const Rgba32fImage>(std::move(*sourceImage));
        const auto uploaded = uploadToResident(context, std::move(source), sourceResident, reason);
        if (uploaded.code != GpuResidentPreviewDiagnosticCode::None) {
            return uploaded;
        }
    }
    if (sourceResident->dataWindow() != *sourceWindow ||
        sourceResident->displayWindow() != *sourceDisplay ||
        sourceResident->pixelAspect() != *par) {
        return fail(GpuResidentPreviewDiagnosticCode::ParityFailure,
                    "the uploaded resident image lost its descriptor");
    }
    const auto sharedSource = std::move(sourceResident);
    hashPixels(context, kWidth, kHeight, *checker);

    struct TranslationCase final {
        double dx;
        double dy;
        float opacity;
        std::int64_t outputOriginX;
        std::int64_t outputOriginY;
    };
    // The source origin is (100, -50); two cases use a different output origin so the window
    // mapping, not just the local sample math, is exercised.
    const std::array<TranslationCase, 5> translations{{
        {0.0, 0.0, 1.0F, 100, -50},
        {0.3, 0.1, 1.0F, 100, -50},
        {-0.3, -0.1, 0.75F, 12, -7},
        {3.0000001, -2.9999999, 1.0F, 100, -50},
        {0.5, -0.5, 0.5F, 0, 0},
    }};
    const auto oracleImage = buildImage(*sourceWindow, *sourceDisplay, *par, *checker);
    if (!oracleImage) {
        return fail(GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                    "the translation CPU oracle image did not build");
    }
    const auto view = oracleImage->view();
    if (!view) {
        return fail(GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                    "the translation CPU oracle view did not build");
    }
    for (const TranslationCase& testCase : translations) {
        const auto outputWindow =
            makeWindow(testCase.outputOriginX, testCase.outputOriginY, kWidth, kHeight);
        if (!outputWindow) {
            return fail(GpuResidentPreviewDiagnosticCode::InternalInvariant,
                        "the translation output window did not build");
        }
        std::vector<Rgba32f> measured;
        std::shared_ptr<const GpuImage> output;
        std::string reason;
        const render::GpuTranslationParameters params{sharedSource, *outputWindow, testCase.dx,
                                                      testCase.dy, testCase.opacity};
        const auto ran = runTranslationReadback(context, params, measured, output, reason);
        if (ran.code != GpuResidentPreviewDiagnosticCode::None) {
            return ran;
        }
        if (output->dataWindow() != *outputWindow || output->displayWindow() != *sourceDisplay ||
            output->pixelAspect() != *par) {
            return fail(GpuResidentPreviewDiagnosticCode::ParityFailure,
                        "translation did not preserve the data/display window and pixel aspect");
        }
        const auto axis = render::TranslationOpacity::create(testCase.dx, testCase.dy,
                                                             static_cast<double>(testCase.opacity));
        if (!axis) {
            return fail(GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                        "the CPU translation parameters were rejected");
        }
        std::vector<Rgba32f> expected(static_cast<std::size_t>(kWidth) * kHeight,
                                      Rgba32f::transparent());
        for (std::uint32_t y = 0; y < kHeight; ++y) {
            auto row =
                std::span<Rgba32f>(expected).subspan(static_cast<std::size_t>(y) * kWidth, kWidth);
            if (render::translateOpacityBilinearRow(
                    *view.value(), *outputWindow,
                    outputWindow->originY() + static_cast<std::int64_t>(y), *axis.value(), row)) {
                return fail(GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                            "the CPU translation oracle row failed");
            }
        }
        if (!closeEqual(measured, expected, reason)) {
            return fail(GpuResidentPreviewDiagnosticCode::ParityFailure,
                        "translation parity failed: " + reason);
        }
        hashPixels(context, kWidth, kHeight, measured);
    }

    // A nonzero subnormal translation input must be rejected whole-frame, never published.
    {
        std::vector<Rgba32f> badPixels(kWidth * kHeight, Rgba32f::transparent());
        const auto subnormal =
            Rgba32f::fromPremultiplied(std::numeric_limits<float>::denorm_min(), 0.0F, 0.0F, 1.0F);
        if (subnormal) {
            badPixels[0] = *subnormal.value();
            auto badImage = buildImage(*sourceWindow, *sourceDisplay, *par, badPixels);
            if (!badImage) {
                return fail(GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                            "the subnormal translation CPU image did not build");
            }
            std::shared_ptr<const GpuImage> badResident;
            {
                std::string reason;
                auto source = std::make_shared<const Rgba32fImage>(std::move(*badImage));
                const auto uploaded =
                    uploadToResident(context, std::move(source), badResident, reason);
                if (uploaded.code != GpuResidentPreviewDiagnosticCode::None) {
                    return uploaded;
                }
            }
            const auto badShared = std::move(badResident);
            const auto begin = context.composite.beginTranslation(
                render::GpuTranslationParameters{badShared, *sourceWindow, 0.0, 0.0, 1.0F},
                context.imageBudget);
            if (begin.code != GpuCompositeDiagnosticCode::None) {
                return fail(GpuResidentPreviewDiagnosticCode::ParityFailure,
                            "the subnormal translation was accepted at begin");
            }
            std::string reason;
            const auto status = waitFor(context.composite, context.isCancelled,
                                        context.deadlineNanoseconds, reason);
            if (status == NativeStatus::Ok) {
                static_cast<void>(context.composite.takeImage());
                return fail(GpuResidentPreviewDiagnosticCode::ParityFailure,
                            "the translation published a frame containing a subnormal value");
            }
            if (status != NativeStatus::Failed ||
                context.composite.diagnostic().code !=
                    GpuCompositeDiagnosticCode::StatusFlagRejected) {
                return waitFailure(status, reason);
            }
            static_cast<void>(context.composite.takeImage());
        }
    }

    // Source-over ordered mix with a non-square-PAR destination, so the destination pixel-aspect
    // preservation is exercised, not assumed.
    constexpr std::uint32_t kDestWidth = 16;
    constexpr std::uint32_t kDestHeight = 4;
    const auto destWindow = makeWindow(5, 5, kDestWidth, kDestHeight);
    const auto destDisplay = makeWindow(-2, 8, 24, 12);
    const auto foreground = detail::makeResidentPreviewSemanticPixels(kDestWidth, kDestHeight);
    const auto backdropPixel =
        render::solidPixelFromStraightLinearRec709Scene(core::Color4d{0.2, 0.4, 0.8, 1.0});
    if (!destWindow || !destDisplay || !foreground || !backdropPixel) {
        return fail(GpuResidentPreviewDiagnosticCode::InternalInvariant,
                    "the source-over fixtures did not build");
    }
    std::shared_ptr<const GpuImage> backdropResident;
    std::vector<Rgba32f> ignoredBackdrop;
    {
        std::string reason;
        const GpuSolidParameters solidParams{*backdropPixel.value(), *destWindow, *destDisplay,
                                             *par};
        const auto ran =
            runSolidReadback(context, solidParams, ignoredBackdrop, backdropResident, reason);
        if (ran.code != GpuResidentPreviewDiagnosticCode::None) {
            return ran;
        }
    }
    if (!backdropResident->isValid() || backdropResident->pixelAspect() != *par ||
        backdropResident->displayWindow() != *destDisplay) {
        return fail(GpuResidentPreviewDiagnosticCode::ParityFailure,
                    "the source-over backdrop lost its non-square geometry");
    }
    std::shared_ptr<const GpuImage> foregroundResident;
    {
        auto foregroundImage = buildImage(*destWindow, *destDisplay, *par, *foreground);
        if (!foregroundImage) {
            return fail(GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                        "the source-over foreground CPU image did not build");
        }
        std::string reason;
        auto source = std::make_shared<const Rgba32fImage>(std::move(*foregroundImage));
        const auto uploaded =
            uploadToResident(context, std::move(source), foregroundResident, reason);
        if (uploaded.code != GpuResidentPreviewDiagnosticCode::None) {
            return uploaded;
        }
    }
    const auto foregroundShared = std::move(foregroundResident);
    const auto backdropShared = std::move(backdropResident);
    std::vector<Rgba32f> soMeasured;
    std::shared_ptr<const GpuImage> soOutput;
    std::string soReason;
    const auto soRan = runSourceOverReadback(
        context, render::GpuSourceOverParameters{foregroundShared, backdropShared}, soMeasured,
        soOutput, soReason);
    if (soRan.code != GpuResidentPreviewDiagnosticCode::None) {
        return soRan;
    }
    if (soOutput->dataWindow() != *destWindow || soOutput->displayWindow() != *destDisplay ||
        soOutput->pixelAspect() != *par) {
        return fail(GpuResidentPreviewDiagnosticCode::ParityFailure,
                    "source-over did not preserve the destination data/display window and PAR");
    }
    std::vector<Rgba32f> expectedDestination(static_cast<std::size_t>(kDestWidth) * kDestHeight,
                                             *backdropPixel.value());
    std::vector<Rgba32f> alignedSource(kDestWidth, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < kDestHeight; ++y) {
        for (std::uint32_t x = 0; x < kDestWidth; ++x) {
            alignedSource[x] = (*foreground)[static_cast<std::size_t>(y) * kDestWidth + x];
        }
        auto destRow = std::span<Rgba32f>(expectedDestination)
                           .subspan(static_cast<std::size_t>(y) * kDestWidth, kDestWidth);
        if (render::sourceOverLinearRec709SceneRow(alignedSource, destRow)) {
            return fail(GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                        "the CPU source-over oracle row failed");
        }
    }
    if (!closeEqual(soMeasured, expectedDestination, soReason)) {
        return fail(GpuResidentPreviewDiagnosticCode::ParityFailure,
                    "source-over parity failed: " + soReason);
    }
    hashPixels(context, kDestWidth, kDestHeight, soMeasured);
    return ok();
}

} // namespace bloom::runtime::resident_preview_detail
