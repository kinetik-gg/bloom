// Real native proof for the GpuComposite host on a Vulkan device: an actual
// Solid -> TranslationOpacityBilinearV1 -> SourceOverV1 resident chain, each stage compared to the
// retained CPU primitives under the documented per-finite-component 2e-6 absolute-or-relative gate.
//
// Compiled against the FROZEN snapshot's private headers only (see proof/README.md). Full readback
// happens only for oracle assertions. Local mode pins the explicit prefix loader and requires a
// device; --require-device fails closed.

#include "gpu_composite_native_support.hpp"

// The complete GpuImageImpl so local GpuImage values can be constructed/destroyed in this TU.
#include "gpu_image_private.hpp"

#include <thread>
#include <utility>

namespace {

using namespace bloom::render::composite_proof;

// --- Translation --------------------------------------------------------------------------------

void runTranslation(Expectations& expectations, GpuComposite& composite, GpuImageUpload& uploader,
                    const TranslationCase& testCase) {
    const auto sourceWindow =
        window(testCase.sourceOriginX, testCase.sourceOriginY, testCase.width, testCase.height);
    const auto outputWindow =
        window(testCase.outputOriginX, testCase.outputOriginY, testCase.width, testCase.height);
    const auto sourcePixels = checker(testCase.width, testCase.height);
    auto sourceImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), sourcePixels);
    if (!sourceImage) {
        expectations.expect(false, testCase.name + ": source builds");
        return;
    }
    auto sourceResident =
        upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    expectations.expect(sourceResident.has_value(), testCase.name + ": source uploads");
    if (!sourceResident) {
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
    // Inputs are immutable: keep the bytes to prove the source is unchanged after the operation.
    const auto sourceBytes = bloom::render::readbackResidentImage(*source, kBudget);
    expectations.expect(sourceBytes.hasValue(), testCase.name + ": source reads back");

    const auto parameters = bloom::render::TranslationOpacity::create(
        testCase.translationX, testCase.translationY, static_cast<double>(testCase.opacity));
    expectations.expect(static_cast<bool>(parameters), testCase.name + ": parameters valid");
    if (!parameters) {
        return;
    }
    const auto begin = composite.beginTranslation(
        {source, outputWindow, testCase.translationX, testCase.translationY, testCase.opacity},
        kBudget);
    expectations.expect(begin.code == GpuCompositeDiagnosticCode::None,
                        testCase.name + ": begin accepted: " + begin.message);
    if (begin.code != GpuCompositeDiagnosticCode::None) {
        return;
    }
    GpuCompositePollResult poll = GpuCompositePollResult::Pending;
    while (poll == GpuCompositePollResult::Pending) {
        poll = composite.poll();
    }
    expectations.expect(poll == GpuCompositePollResult::Ready,
                        testCase.name + ": completes: " + composite.diagnostic().message);
    if (poll != GpuCompositePollResult::Ready) {
        return;
    }
    auto result = composite.takeImage();
    expectations.expect(result.isValid(), testCase.name + ": resident output published");
    // The output preserves the source display window and pixel aspect.
    expectations.expect(result.displayWindow() == source->displayWindow() &&
                            result.pixelAspect() == source->pixelAspect(),
                        testCase.name + ": output preserves display window and pixel aspect");

    // CPU oracle: translateOpacityBilinearRow, which works in LOCAL coordinates (origins do not
    // enter its sample math), over the output window.
    const auto oracleImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), sourcePixels);
    if (!oracleImage) {
        expectations.expect(false, testCase.name + ": oracle source image builds");
        return;
    }
    const auto view = oracleImage->view();
    if (!view) {
        expectations.expect(false, testCase.name + ": oracle source view builds");
        return;
    }
    std::vector<Rgba32f> expected(static_cast<std::size_t>(testCase.width) * testCase.height,
                                  Rgba32f::transparent());
    for (std::uint32_t y = 0; y < testCase.height; ++y) {
        auto row = std::span<Rgba32f>(expected).subspan(
            static_cast<std::size_t>(y) * testCase.width, testCase.width);
        const auto status = bloom::render::translateOpacityBilinearRow(
            *view.value(), outputWindow, outputWindow.originY() + static_cast<std::int64_t>(y),
            *parameters.value(), row);
        expectations.expect(!status.has_value(), testCase.name + ": oracle row succeeds");
    }
    const auto readback = bloom::render::readbackResidentImage(result, kBudget);
    expectations.expect(readback.hasValue(), testCase.name + ": readback for oracle");
    if (readback) {
        static_cast<void>(pixelsMatch(readback.pixels, expected, expectations, testCase.name));
    }
    // The immutable source is unchanged by the operation.
    const auto sourceAfter = bloom::render::readbackResidentImage(*source, kBudget);
    expectations.expect(sourceAfter.hasValue() && sourceBytes.hasValue() &&
                            sourceAfter.pixels == sourceBytes.pixels,
                        testCase.name + ": the immutable source is unchanged");
}

void testTranslation(Expectations& expectations, GpuDevice& device) {
    auto composite = GpuComposite::create(device);
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(composite.hasValue() && uploader.hasValue(), "translation hosts created");
    if (!composite || !uploader) {
        return;
    }
    const std::vector<TranslationCase> cases{
        {"4k-checker-dx+0.3-dy+0.1", 3840, 8, 0, 0, 0, 0, 0.3, 0.1, 1.0F},
        {"4k-checker-dx-0.3-dy-0.1", 3840, 8, 0, 0, 0, 0, -0.3, -0.1, 1.0F},
        {"4k-checker-near-integer", 3840, 8, 0, 0, 0, 0, 3.0000001, -2.9999999, 1.0F},
        {"nonzero-differing-origins", 16, 4, 100, -50, 7, 3, 0.5, -0.5, 1.0F},
        {"odd-extents-fractional", 7, 3, 0, 0, 0, 0, 0.5, -0.5, 0.75F},
        {"opacity-zero", 64, 4, 0, 0, 0, 0, 0.5, 0.5, 0.0F},
        {"opacity-half", 64, 4, 0, 0, 0, 0, 0.25, 0.0, 0.5F},
    };
    for (const auto& testCase : cases) {
        runTranslation(expectations, *composite.composite, *uploader.upload, testCase);
    }
}

// --- Source-over --------------------------------------------------------------------------------

void runSourceOver(Expectations& expectations, GpuComposite& composite, GpuImageUpload& uploader,
                   GpuSolid& solidOp, const SourceOverCase& testCase) {
    const auto destWindow =
        window(testCase.destOriginX, testCase.destOriginY, testCase.width, testCase.height);
    const auto sourceWindow =
        window(testCase.sourceOriginX, testCase.sourceOriginY, testCase.width, testCase.height);
    const auto foregroundPixels = testCase.semantic
                                      ? semanticPixels(testCase.width, testCase.height)
                                      : checker(testCase.width, testCase.height);
    auto foregroundImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), foregroundPixels);
    if (!foregroundImage) {
        expectations.expect(false, testCase.name + ": foreground image builds");
        return;
    }
    auto foregroundResident =
        upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*foregroundImage)));
    expectations.expect(foregroundResident.has_value(), testCase.name + ": foreground uploads");
    if (!foregroundResident) {
        return;
    }
    auto backdropResident = solid(solidOp, Color4d{0.2, 0.4, 0.8, 1.0}, destWindow, destWindow,
                                  PixelAspectRatio::square());
    expectations.expect(backdropResident.has_value(), testCase.name + ": solid backdrop produced");
    if (!backdropResident) {
        return;
    }
    auto foreground = std::make_shared<const GpuImage>(std::move(*foregroundResident));
    auto backdrop = std::make_shared<const GpuImage>(std::move(*backdropResident));
    const auto backdropBefore = bloom::render::readbackResidentImage(*backdrop, kBudget);
    const auto foregroundBefore = bloom::render::readbackResidentImage(*foreground, kBudget);

    const auto begin = composite.beginSourceOver({foreground, backdrop}, kBudget);
    expectations.expect(begin.code == GpuCompositeDiagnosticCode::None,
                        testCase.name + ": begin accepted: " + begin.message);
    if (begin.code != GpuCompositeDiagnosticCode::None) {
        return;
    }
    GpuCompositePollResult poll = GpuCompositePollResult::Pending;
    while (poll == GpuCompositePollResult::Pending) {
        poll = composite.poll();
    }
    expectations.expect(poll == GpuCompositePollResult::Ready,
                        testCase.name + ": completes: " + composite.diagnostic().message);
    if (poll != GpuCompositePollResult::Ready) {
        return;
    }
    auto result = composite.takeImage();

    // CPU oracle: for each destination row build an aligned source row by sampling the source
    // window at (destAbsolute - sourceOrigin) with transparent padding, then the retained
    // source-over primitive. This is the exact window mapping the kernel implements.
    const auto solidPixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.2, 0.4, 0.8, 1.0});
    std::vector<Rgba32f> destination(static_cast<std::size_t>(testCase.width) * testCase.height,
                                     *solidPixel.value());
    std::vector<Rgba32f> alignedSource(testCase.width, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < testCase.height; ++y) {
        const auto destY = destWindow.originY() + static_cast<std::int64_t>(y);
        for (std::uint32_t x = 0; x < testCase.width; ++x) {
            const auto destX = destWindow.originX() + static_cast<std::int64_t>(x);
            const auto sourceX = destX - sourceWindow.originX();
            const auto sourceY = destY - sourceWindow.originY();
            const bool inside = sourceX >= 0 && sourceY >= 0 &&
                                sourceX < static_cast<std::int64_t>(testCase.width) &&
                                sourceY < static_cast<std::int64_t>(testCase.height);
            alignedSource[x] =
                inside ? foregroundPixels[static_cast<std::size_t>(sourceY) * testCase.width +
                                          static_cast<std::size_t>(sourceX)]
                       : Rgba32f::transparent();
        }
        auto destRow = std::span<Rgba32f>(destination)
                           .subspan(static_cast<std::size_t>(y) * testCase.width, testCase.width);
        const auto status = bloom::render::sourceOverLinearRec709SceneRow(alignedSource, destRow);
        expectations.expect(!status.has_value(), testCase.name + ": oracle row succeeds");
    }
    const auto readback = bloom::render::readbackResidentImage(result, kBudget);
    expectations.expect(readback.hasValue(), testCase.name + ": readback for oracle");
    if (readback) {
        static_cast<void>(pixelsMatch(readback.pixels, destination, expectations, testCase.name));
    }
    // Neither immutable input is mutated.
    const auto backdropAfter = bloom::render::readbackResidentImage(*backdrop, kBudget);
    const auto foregroundAfter = bloom::render::readbackResidentImage(*foreground, kBudget);
    expectations.expect(backdropAfter.hasValue() && backdropBefore.hasValue() &&
                            backdropAfter.pixels == backdropBefore.pixels,
                        testCase.name + ": the immutable backdrop is unchanged");
    expectations.expect(foregroundAfter.hasValue() && foregroundBefore.hasValue() &&
                            foregroundAfter.pixels == foregroundBefore.pixels,
                        testCase.name + ": the immutable foreground is unchanged");
}

void testSourceOver(Expectations& expectations, GpuDevice& device) {
    auto composite = GpuComposite::create(device);
    auto uploader = GpuImageUpload::create(device);
    auto solidOp = GpuSolid::create(device);
    expectations.expect(composite.hasValue() && uploader.hasValue() && solidOp.hasValue(),
                        "source-over hosts created");
    if (!composite || !uploader || !solidOp) {
        return;
    }
    const std::vector<SourceOverCase> cases{
        {"checker-same-origin", 16, 4, 0, 0, 0, 0, false},
        {"checker-differing-origins", 16, 4, 7, 3, 100, -50, false},
        {"checker-source-out-of-bounds", 16, 4, 1000, 1000, 0, 0, false},
        {"semantic-all-rgba", 32, 8, 5, 5, -20, 40, true},
    };
    for (const auto& testCase : cases) {
        runSourceOver(expectations, *composite.composite, *uploader.upload, *solidOp.solid,
                      testCase);
    }
}

// --- Rejections, budget, cancel, wrong device, lifetime -----------------------------------------

void testRejections(Expectations& expectations, GpuDevice& device) {
    auto composite = GpuComposite::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!composite || !uploader) {
        expectations.expect(false, "rejection hosts created");
        return;
    }
    const auto window4 = window(0, 0, 4, 4);
    expectations.expect(
        composite.composite->beginTranslation({nullptr, window4, 0.0, 0.0, 1.0F}, kBudget).code ==
            GpuCompositeDiagnosticCode::InvalidArgument,
        "a null translation source is rejected");
    expectations.expect(composite.composite->beginSourceOver({nullptr, nullptr}, kBudget).code ==
                            GpuCompositeDiagnosticCode::InvalidArgument,
                        "null source-over inputs are rejected");

    auto image = makeImage(window4, window4, PixelAspectRatio::square(), checker(4, 4));
    if (!image) {
        expectations.expect(false, "rejection source image builds");
        return;
    }
    auto resident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*image)));
    if (!resident) {
        return;
    }
    auto shared = std::make_shared<const GpuImage>(std::move(*resident));
    expectations.expect(
        composite.composite
                ->beginTranslation(
                    {shared, window4, std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0F}, kBudget)
                .code == GpuCompositeDiagnosticCode::InvalidArgument,
        "a non-finite translation is rejected");
    expectations.expect(
        composite.composite->beginTranslation({shared, window4, 0.0, 0.0, 1.0F}, 1).code ==
            GpuCompositeDiagnosticCode::OverBudget,
        "an over-budget request is rejected");

    // Cancel: submit then cancel before polling.
    const auto started =
        composite.composite->beginTranslation({shared, window4, 0.0, 0.0, 1.0F}, kBudget);
    if (started.code == GpuCompositeDiagnosticCode::None) {
        composite.composite->cancel();
        GpuCompositePollResult poll = GpuCompositePollResult::Pending;
        while (poll == GpuCompositePollResult::Pending) {
            poll = composite.composite->poll();
        }
        expectations.expect(poll == GpuCompositePollResult::Failure &&
                                composite.composite->diagnostic().code ==
                                    GpuCompositeDiagnosticCode::Cancelled,
                            "a cancelled job reports Failure(Cancelled)");
    }

    // Wrong device.
    GpuDeviceCreationOptions options;
    auto second = GpuDevice::create(options);
    if (second) {
        expectations.expect(!composite.composite->isBoundTo(*second.device),
                            "a second device is not the bound one");
    }

    // Foreign-thread destruction must not make a Vulkan call. Retain the result of an operation and
    // destroy it from another thread; a crash here would mean a foreign Vulkan destroy.
    auto image2 = makeImage(window4, window4, PixelAspectRatio::square(), checker(4, 4));
    if (!image2) {
        expectations.expect(false, "foreign-thread source image builds");
        return;
    }
    auto resident2 =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*image2)));
    if (resident2) {
        auto foreign = std::make_shared<GpuImage>(std::move(*resident2));
        std::thread([foreign]() mutable { foreign.reset(); }).join();
        expectations.expect(true, "foreign-thread image destruction did not crash");
    }

    // Status-flag rejection: a subnormal input lane is rejected whole-frame by the kernels.
    const auto subnormal =
        Rgba32f::fromPremultiplied(std::numeric_limits<float>::denorm_min(), 0.0F, 0.0F, 1.0F);
    if (subnormal) {
        std::vector<Rgba32f> pixels(16, Rgba32f::transparent());
        pixels[0] = *subnormal.value();
        auto bad = makeImage(window4, window4, PixelAspectRatio::square(), pixels);
        if (!bad) {
            expectations.expect(false, "subnormal probe image builds");
            return;
        }
        auto badResident =
            upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*bad)));
        if (badResident) {
            auto badShared = std::make_shared<const GpuImage>(std::move(*badResident));
            const auto begin = composite.composite->beginTranslation(
                {badShared, window4, 0.0, 0.0, 1.0F}, kBudget);
            if (begin.code == GpuCompositeDiagnosticCode::None) {
                GpuCompositePollResult poll = GpuCompositePollResult::Pending;
                while (poll == GpuCompositePollResult::Pending) {
                    poll = composite.composite->poll();
                }
                expectations.expect(poll == GpuCompositePollResult::Failure &&
                                        composite.composite->diagnostic().code ==
                                            GpuCompositeDiagnosticCode::StatusFlagRejected,
                                    "a subnormal input is rejected as StatusFlagRejected");
            }
        }
    }
}

// A real foreign-device input: an image created from a DIFFERENT GpuDevice must be rejected with a
// typed diagnostic before any dispatch, and the original pipeline must remain usable afterwards.
void testForeignInputs(Expectations& expectations, GpuDevice& device) {
    auto composite = GpuComposite::create(device);
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(composite.hasValue() && uploader.hasValue(), "foreign-input hosts created");
    if (!composite || !uploader) {
        return;
    }
    const auto window4 = window(0, 0, 4, 4);

    GpuDeviceCreationOptions options;
    auto second = GpuDevice::create(options);
    if (!second) {
        std::cout
            << "NOTE: a second device is unavailable; foreign-input rejection not exercised\n";
        return;
    }
    auto foreignUploader = GpuImageUpload::create(*second.device);
    expectations.expect(foreignUploader.hasValue(), "the foreign upload host is created");
    if (!foreignUploader) {
        return;
    }
    auto image = makeImage(window4, window4, PixelAspectRatio::square(), checker(4, 4));
    if (!image) {
        expectations.expect(false, "the foreign image builds");
        return;
    }
    auto foreignResident =
        upload(*foreignUploader.upload, std::make_shared<const Rgba32fImage>(std::move(*image)));
    expectations.expect(foreignResident.has_value(), "the foreign image is uploaded");
    if (!foreignResident) {
        return;
    }
    auto foreign = std::make_shared<const GpuImage>(std::move(*foreignResident));

    // Translation with a foreign source: typed InvalidArgument, and no job started.
    const auto translation =
        composite.composite->beginTranslation({foreign, window4, 0.0, 0.0, 1.0F}, kBudget);
    expectations.expect(translation.code == GpuCompositeDiagnosticCode::InvalidArgument,
                        "a foreign translation source is rejected before dispatch");
    expectations.expect(composite.composite->state() == GpuCompositeJobState::Idle,
                        "a foreign-input rejection starts no job");

    // Source-over with the foreign image on each side.
    auto sameImage = makeImage(window4, window4, PixelAspectRatio::square(), checker(4, 4));
    if (!sameImage) {
        expectations.expect(false, "the same-device image builds");
        return;
    }
    auto sameResident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*sameImage)));
    if (!sameResident) {
        return;
    }
    auto same = std::make_shared<const GpuImage>(std::move(*sameResident));
    expectations.expect(composite.composite->beginSourceOver({foreign, same}, kBudget).code ==
                            GpuCompositeDiagnosticCode::InvalidArgument,
                        "a foreign source-over foreground is rejected");
    expectations.expect(composite.composite->beginSourceOver({same, foreign}, kBudget).code ==
                            GpuCompositeDiagnosticCode::InvalidArgument,
                        "a foreign source-over backdrop is rejected");

    // The original pipeline is still usable with a same-device input.
    const auto valid =
        composite.composite->beginTranslation({same, window4, 0.0, 0.0, 1.0F}, kBudget);
    expectations.expect(valid.code == GpuCompositeDiagnosticCode::None,
                        "the pipeline is usable after a foreign-input rejection");
    if (valid.code == GpuCompositeDiagnosticCode::None) {
        GpuCompositePollResult poll = GpuCompositePollResult::Pending;
        while (poll == GpuCompositePollResult::Pending) {
            poll = composite.composite->poll();
        }
        expectations.expect(poll == GpuCompositePollResult::Ready, "the recovery job completes");
    }
}

// The budget is enforced on the ACTUAL VMA allocation sizes, not the requested pixel bytes: a
// byte budget between the requested size and the allocator-rounded size must be refused, and a
// generous budget must still succeed.
void testActualAllocationBudget(Expectations& expectations, GpuDevice& device) {
    auto composite = GpuComposite::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!composite || !uploader) {
        expectations.expect(false, "allocation-budget hosts created");
        return;
    }
    const auto window4 = window(0, 0, 4, 4);
    auto image = makeImage(window4, window4, PixelAspectRatio::square(), checker(4, 4));
    if (!image) {
        expectations.expect(false, "allocation-budget source image builds");
        return;
    }
    auto resident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*image)));
    if (!resident) {
        expectations.expect(false, "allocation-budget source uploads");
        return;
    }
    auto shared = std::make_shared<const GpuImage>(std::move(*resident));

    // 4x4 RGBA32F requests 256 bytes; a 512-byte budget passes the requested check but the actual
    // VMA allocations (image + axis + status + staging) exceed it.
    const auto tight =
        composite.composite->beginTranslation({shared, window4, 0.0, 0.0, 1.0F}, 512);
    expectations.expect(tight.code == GpuCompositeDiagnosticCode::OverBudget,
                        "an actual-allocation over-budget request is refused");
    expectations.expect(composite.composite->state() == GpuCompositeJobState::Idle,
                        "an allocation-budget refusal starts no job");

    // The same request with a generous budget still succeeds.
    const auto ok =
        composite.composite->beginTranslation({shared, window4, 0.0, 0.0, 1.0F}, kBudget);
    expectations.expect(ok.code == GpuCompositeDiagnosticCode::None,
                        "the valid job after an allocation-budget refusal is accepted");
    if (ok.code == GpuCompositeDiagnosticCode::None) {
        GpuCompositePollResult poll = GpuCompositePollResult::Pending;
        while (poll == GpuCompositePollResult::Pending) {
            poll = composite.composite->poll();
        }
        expectations.expect(poll == GpuCompositePollResult::Ready,
                            "the valid job completes after the refusal");
    }
}

// The full Solid -> Translation -> SourceOver chain in one resident graph.
void testChain(Expectations& expectations, GpuDevice& device) {
    auto composite = GpuComposite::create(device);
    auto uploader = GpuImageUpload::create(device);
    auto solidOp = GpuSolid::create(device);
    if (!composite || !uploader || !solidOp) {
        expectations.expect(false, "chain hosts created");
        return;
    }
    const auto window16 = window(0, 0, 16, 4);
    auto backdrop = solid(*solidOp.solid, Color4d{0.2, 0.4, 0.8, 1.0}, window16, window16,
                          PixelAspectRatio::square());
    if (!backdrop) {
        expectations.expect(false, "chain backdrop produced");
        return;
    }
    auto foregroundImage =
        makeImage(window16, window16, PixelAspectRatio::square(), checker(16, 4));
    if (!foregroundImage) {
        expectations.expect(false, "chain foreground image builds");
        return;
    }
    auto foreground =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*foregroundImage)));
    if (!foreground) {
        expectations.expect(false, "chain foreground uploaded");
        return;
    }
    auto translated = std::make_shared<const GpuImage>(std::move(*foreground));
    auto backdropShared = std::make_shared<const GpuImage>(std::move(*backdrop));
    const auto begin = composite.composite->beginSourceOver({translated, backdropShared}, kBudget);
    expectations.expect(begin.code == GpuCompositeDiagnosticCode::None,
                        "the Solid->SourceOver chain begins");
    if (begin.code != GpuCompositeDiagnosticCode::None) {
        return;
    }
    GpuCompositePollResult poll = GpuCompositePollResult::Pending;
    while (poll == GpuCompositePollResult::Pending) {
        poll = composite.composite->poll();
    }
    expectations.expect(poll == GpuCompositePollResult::Ready,
                        "the Solid->SourceOver chain completes");
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
        testTranslation(expectations, *device.device);
        testSourceOver(expectations, *device.device);
        testChain(expectations, *device.device);
        testRejections(expectations, *device.device);
        testForeignInputs(expectations, *device.device);
        testActualAllocationBudget(expectations, *device.device);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " composite native expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: GPU composite native chain\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
