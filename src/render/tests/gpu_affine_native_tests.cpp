// Real native proof for GpuAffine on a Vulkan device: upload a decoded RGBA32F source, run
// AffineBilinearV1 through a rotation/scale/anchor/composed-matrix placement, and compare the
// resident result to the REAL CPU oracle (render::layerTransformBilinearRow) under the documented
// per-finite-component 2e-6 absolute-or-relative gate.
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
    const auto clip = window(-96, -96, 192, 192);
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

    const auto begin = affine.beginAffine({source, *outputWindow, *transform}, kBudget);
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
    // A budget below the actual sample metadata must be refused before any device work.
    const auto refused = affine.affine->beginAffine({source, outputWindow, *transform.value()}, 8);
    expectations.expect(refused.code == GpuAffineDiagnosticCode::OverBudget,
                        "a sub-metadata budget is refused as OverBudget");
    expectations.expect(!affine.affine->hasUnretiredSubmission(),
                        "a refused request submits nothing");

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

void testComposedMatrixForm(Expectations& expectations, GpuDevice& device) {
    auto affine = GpuAffine::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!affine || !uploader) {
        expectations.expect(false, "composed-matrix hosts created");
        return;
    }
    const auto sourceWindow = window(0, 0, 6, 4);
    const auto outputWindow = window(-4, -4, 16, 14);
    const auto sourcePixels = semanticPixels(6, 4);
    auto sourceImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), sourcePixels);
    if (!sourceImage) {
        return;
    }
    auto sourceResident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    if (!sourceResident) {
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
    // A composed rotation+nonuniform-scale+translation matrix (with the anchor folded in), in
    // absolute output coordinates. This is the "precomposed parent matrix" input shape.
    const float opacity = 0.6F;
    const double radians = 25.0 * 3.14159265358979323846 / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const bloom::render::GpuAffineMatrix matrix{.a = cosine * 1.5,
                                                .b = -sine * 0.8,
                                                .tx = 0.75,
                                                .c = sine * 1.5,
                                                .d = cosine * 0.8,
                                                .ty = -1.25};
    const auto begin =
        affine.affine->beginAffineMatrix({source, outputWindow, matrix, opacity}, kBudget);
    expectations.expect(begin.code == GpuAffineDiagnosticCode::None,
                        "the composed-matrix request begins: " + begin.message);
    if (begin.code != GpuAffineDiagnosticCode::None) {
        return;
    }
    GpuAffinePollResult poll = GpuAffinePollResult::Pending;
    while (poll == GpuAffinePollResult::Pending) {
        poll = affine.affine->poll();
    }
    expectations.expect(poll == GpuAffinePollResult::Ready,
                        "the composed-matrix request completes");
}

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
