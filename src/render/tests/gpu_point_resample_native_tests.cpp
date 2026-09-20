// Real native proof for GpuPointResample (PointResampleV1) on a Vulkan device: upload a decoded
// RGBA32F source, run the point/nearest resample through the media-image-proxy geometry, and assert
// the resident result is BIT-EXACT to an independent re-implementation of the CPU oracle
// (src/runtime/image_source.cpp evaluateImageSource).
//
// Readback happens only for oracle assertions; the production path never reads back. It skips
// cleanly without a device and --require-device fails closed.

#include "gpu_composite_native_support.hpp"

#include <bloom/render/gpu_point_resample.hpp>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom::render::composite_proof;
using bloom::core::PixelAspectRatio;
using bloom::render::GpuPointResample;
using bloom::render::GpuPointResampleDiagnosticCode;
using bloom::render::GpuPointResamplePollResult;
using bloom::render::GpuPointResampleRequest;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageDescriptor;

// An independent re-implementation of the media-image-proxy CPU axis rule.
[[nodiscard]] std::int32_t oracleAxis(const std::uint32_t index, const std::uint32_t extent,
                                      const double scale) {
    const double mapped = static_cast<double>(index) / scale;
    const double maximum = static_cast<double>(extent - 1U);
    return static_cast<std::int32_t>(
        static_cast<std::uint32_t>(mapped > maximum ? maximum : mapped));
}

[[nodiscard]] std::uint32_t derivedExtent(const std::uint32_t extent, const double scale) {
    return static_cast<std::uint32_t>(
        std::max(1.0, std::ceil(static_cast<double>(extent) * scale)));
}

[[nodiscard]] std::vector<Rgba32f>
oracleResample(const std::span<const Rgba32f> source, const ImageWindow sourceWindow,
               const std::uint32_t outputWidth, const std::uint32_t outputHeight,
               const double horizontalScale, const double verticalScale) {
    const auto sourceWidth = sourceWindow.extent().width();
    const auto sourceHeight = sourceWindow.extent().height();
    std::vector<Rgba32f> expected(static_cast<std::size_t>(outputWidth) * outputHeight,
                                  Rgba32f::transparent());
    for (std::uint32_t y = 0; y < outputHeight; ++y) {
        const auto sourceY = static_cast<std::uint32_t>(oracleAxis(y, sourceHeight, verticalScale));
        for (std::uint32_t x = 0; x < outputWidth; ++x) {
            const auto sourceX =
                static_cast<std::uint32_t>(oracleAxis(x, sourceWidth, horizontalScale));
            expected[static_cast<std::size_t>(y) * outputWidth + x] =
                source[static_cast<std::size_t>(sourceY) * sourceWidth + sourceX];
        }
    }
    return expected;
}

// Dense HDR/negative/alpha fixture: RGB crosses negative and large positive values, alpha covers
// exact 0/1 and fractional values. All components are finite so Rgba32f accepts them.
[[nodiscard]] std::vector<Rgba32f> hdrAlphaPixels(const std::uint32_t width,
                                                  const std::uint32_t height) {
    std::vector<Rgba32f> pixels(static_cast<std::size_t>(width) * height, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const bool positive = ((x + y) % 2U) == 0U;
            const auto magnitude = 8.0F + static_cast<float>(x % 4U);
            const auto red = positive ? magnitude : -magnitude;
            const auto green = positive ? -3.5F : 12.0F;
            const auto blue = static_cast<float>(x) * 1.25F - static_cast<float>(y) * 0.5F;
            float alpha = 0.0F;
            switch (x % 4U) {
            case 0U:
                alpha = 0.0F;
                break;
            case 1U:
                alpha = 1.0F;
                break;
            case 2U:
                alpha = 0.25F;
                break;
            default:
                alpha = 0.5F;
                break;
            }
            pixels[static_cast<std::size_t>(y) * width + x] = pixel(red, green, blue, alpha);
        }
    }
    return pixels;
}

[[nodiscard]] std::optional<Rgba32fImageDescriptor>
outputDescriptor(const std::uint32_t width, const std::uint32_t height,
                 const ImageWindow displayWindow, const PixelAspectRatio aspect) {
    const auto dataWindow = ImageWindow::create(0, 0, width, height);
    if (!dataWindow) {
        return std::nullopt;
    }
    const auto descriptor =
        Rgba32fImageDescriptor::create(*dataWindow.value(), displayWindow, aspect);
    return descriptor ? std::optional(*descriptor.value()) : std::nullopt;
}

[[nodiscard]] bool bitsEqual(const Rgba32f& lhs, const Rgba32f& rhs) {
    for (std::size_t index = 0; index < 4; ++index) {
        if (std::bit_cast<std::uint32_t>(lhs.components()[index]) !=
            std::bit_cast<std::uint32_t>(rhs.components()[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool allBitsEqual(const std::vector<Rgba32f>& actual,
                                const std::vector<Rgba32f>& expected, Expectations& expectations,
                                const std::string& label) {
    if (actual.size() != expected.size()) {
        expectations.expect(false, label + ": pixel counts agree");
        return false;
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (!bitsEqual(actual[index], expected[index])) {
            expectations.expect(false, label + ": pixel " + std::to_string(index) + " bit-exact");
            return false;
        }
    }
    expectations.expect(true, label + ": every channel bit-exact");
    return true;
}

struct GeoCase final {
    std::string name;
    std::uint32_t sourceWidth;
    std::uint32_t sourceHeight;
    std::int64_t sourceOriginX;
    std::int64_t sourceOriginY;
    double horizontalScale;
    double verticalScale;
};

[[nodiscard]] std::vector<GeoCase> geometryCases() {
    return {
        {"half", 6, 5, 0, 0, 0.5, 0.5},
        {"quarter", 6, 5, 0, 0, 0.25, 0.25},
        {"nonpower", 7, 3, 0, 0, 0.3, 0.7},
        {"identity", 5, 4, 0, 0, 1.0, 1.0},
        {"upscale", 2, 2, 0, 0, 2.5, 2.5},
        {"tails", 10, 7, 0, 0, 1.7, 0.6},
        {"negative-origin", 6, 5, -9, -4, 0.5, 0.5},
        {"one-pixel-up", 1, 1, -1, 7, 4.0, 4.0},
        {"one-pixel-down", 1, 1, 3, -2, 0.5, 0.5},
    };
}

void testGeometryParity(Expectations& expectations, GpuDevice& device) {
    auto resampler = GpuPointResample::create(device);
    auto uploader = bloom::render::GpuImageUpload::create(device);
    expectations.expect(resampler.hasValue() && uploader.hasValue(), "geometry hosts created");
    if (!resampler || !uploader) {
        return;
    }
    const ImageWindow displayWindow = window(4, -8, 320, 240);
    const auto aspect = PixelAspectRatio::create(4, 3).value();
    for (const auto& testCase : geometryCases()) {
        const ImageWindow sourceWindow = window(testCase.sourceOriginX, testCase.sourceOriginY,
                                                testCase.sourceWidth, testCase.sourceHeight);
        const auto pixels = hdrAlphaPixels(testCase.sourceWidth, testCase.sourceHeight);
        auto sourceImage =
            makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), pixels);
        if (!sourceImage) {
            expectations.expect(false, testCase.name + ": source image builds");
            continue;
        }
        auto sourceResident =
            upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
        expectations.expect(sourceResident.has_value(), testCase.name + ": source uploads");
        if (!sourceResident) {
            continue;
        }
        auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
        const auto outputWidth = derivedExtent(testCase.sourceWidth, testCase.horizontalScale);
        const auto outputHeight = derivedExtent(testCase.sourceHeight, testCase.verticalScale);
        const auto output = outputDescriptor(outputWidth, outputHeight, displayWindow, aspect);
        expectations.expect(output.has_value(), testCase.name + ": output descriptor builds");
        if (!output) {
            continue;
        }
        const GpuPointResampleRequest request{source, *output, testCase.horizontalScale,
                                              testCase.verticalScale};
        const auto begin = resampler.resampler->begin(request, kBudget);
        expectations.expect(begin.code == GpuPointResampleDiagnosticCode::None,
                            testCase.name + ": begin accepted: " + begin.message);
        if (begin.code != GpuPointResampleDiagnosticCode::None) {
            continue;
        }
        GpuPointResamplePollResult poll = GpuPointResamplePollResult::Pending;
        while (poll == GpuPointResamplePollResult::Pending) {
            poll = resampler.resampler->poll();
        }
        expectations.expect(poll == GpuPointResamplePollResult::Ready,
                            testCase.name +
                                ": completes: " + resampler.resampler->diagnostic().message);
        if (poll != GpuPointResamplePollResult::Ready) {
            continue;
        }
        auto result = resampler.resampler->take();
        expectations.expect(result.isValid(), testCase.name + ": resident output published");
        expectations.expect(
            result.dataWindow() == std::optional<ImageWindow>(output->dataWindow()) &&
                result.displayWindow() == std::optional<ImageWindow>(displayWindow) &&
                result.pixelAspect() == aspect,
            testCase.name + ": output window/PAR preserved exactly");
        const auto readback = bloom::render::readbackResidentImage(result, kBudget);
        expectations.expect(readback.hasValue(), testCase.name + ": readback for oracle");
        if (!readback) {
            continue;
        }
        const auto expected = oracleResample(pixels, sourceWindow, outputWidth, outputHeight,
                                             testCase.horizontalScale, testCase.verticalScale);
        static_cast<void>(allBitsEqual(readback.pixels, expected, expectations, testCase.name));
    }
}

// Records the honest per-request cost of the current design: no warm native cache, so the shader
// pipeline, descriptor set, command pool/buffer, and fence are created per begin(). This is a
// measurement, not an assertion.
void testPerRequestCost(Expectations& expectations, GpuDevice& device) {
    constexpr std::uint32_t kWidth = 64;
    constexpr std::uint32_t kHeight = 32;
    constexpr int kIterations = 25;
    auto resampler = GpuPointResample::create(device);
    auto uploader = bloom::render::GpuImageUpload::create(device);
    if (!resampler || !uploader) {
        expectations.expect(false, "cost: hosts created");
        return;
    }
    const ImageWindow sourceWindow = window(0, 0, kWidth, kHeight);
    auto sourceImage = makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(),
                                 hdrAlphaPixels(kWidth, kHeight));
    if (!sourceImage) {
        expectations.expect(false, "cost: source built");
        return;
    }
    auto sourceResident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    if (!sourceResident) {
        expectations.expect(false, "cost: source uploaded");
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
    const auto aspect = PixelAspectRatio::square();
    const ImageWindow displayWindow = window(0, 0, kWidth, kHeight);
    const auto output = outputDescriptor(kWidth, kHeight, displayWindow, aspect);
    if (!output) {
        expectations.expect(false, "cost: output descriptor built");
        return;
    }
    const GpuPointResampleRequest request{source, *output, 1.0, 1.0};
    const auto start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        if (resampler.resampler->begin(request, kBudget).code !=
            GpuPointResampleDiagnosticCode::None) {
            expectations.expect(false, "cost: begin accepted");
            return;
        }
        GpuPointResamplePollResult poll = GpuPointResamplePollResult::Pending;
        while (poll == GpuPointResamplePollResult::Pending) {
            poll = resampler.resampler->poll();
        }
        if (poll != GpuPointResamplePollResult::Ready) {
            expectations.expect(false, "cost: job completed");
            return;
        }
        static_cast<void>(resampler.resampler->take());
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / kIterations;
    std::cout << "point-resample per-request begin->Ready->take (no warm native cache): " << micros
              << " us over " << kIterations << " iterations\n";
}

void testOwnershipRejectionsAndCancellation(Expectations& expectations, GpuDevice& device) {
    auto resampler = GpuPointResample::create(device);
    auto uploader = bloom::render::GpuImageUpload::create(device);
    if (!resampler || !uploader) {
        expectations.expect(false, "ownership hosts created");
        return;
    }
    const ImageWindow sourceWindow = window(0, 0, 5, 3);
    auto sourceImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), hdrAlphaPixels(5, 3));
    if (!sourceImage) {
        return;
    }
    auto sourceResident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    if (!sourceResident) {
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
    const auto aspect = PixelAspectRatio::square();
    const ImageWindow displayWindow = window(0, 0, 5, 3);
    const auto output = outputDescriptor(3, 2, displayWindow, aspect);
    if (!output) {
        return;
    }
    const GpuPointResampleRequest request{source, *output, 0.5, 0.5};

    // Wrong thread: begin must fail closed without touching the device.
    GpuPointResampleDiagnosticCode wrongThread = GpuPointResampleDiagnosticCode::None;
    std::thread worker([&] { wrongThread = resampler.resampler->begin(request, kBudget).code; });
    worker.join();
    expectations.expect(wrongThread == GpuPointResampleDiagnosticCode::WrongThread,
                        "begin from a foreign thread reports WrongThread");

    // Invalid scale domains.
    for (const double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
        const GpuPointResampleRequest invalid{source, *output, bad, 1.0};
        const auto refused = resampler.resampler->begin(invalid, kBudget);
        expectations.expect(refused.code == GpuPointResampleDiagnosticCode::InvalidArgument,
                            "an invalid scale reports InvalidArgument");
    }
    // A descriptor whose data window is not the derived proxy window must be refused.
    const auto mismatched = outputDescriptor(4, 2, displayWindow, aspect);
    if (mismatched) {
        const GpuPointResampleRequest invalid{source, *mismatched, 0.5, 0.5};
        const auto refused = resampler.resampler->begin(invalid, kBudget);
        expectations.expect(refused.code == GpuPointResampleDiagnosticCode::InvalidArgument,
                            "a mismatched output descriptor reports InvalidArgument");
    }
    expectations.expect(!resampler.resampler->hasUnretiredSubmission(),
                        "a refused request submits nothing");

    // take() before Ready must not publish a partial image.
    const auto begin = resampler.resampler->begin(request, kBudget);
    expectations.expect(begin.code == GpuPointResampleDiagnosticCode::None,
                        "a valid request begins");
    if (begin.code == GpuPointResampleDiagnosticCode::None) {
        expectations.expect(!resampler.resampler->take().isValid(),
                            "take before Ready returns no image");
        expectations.expect(resampler.resampler->hasUnretiredSubmission(),
                            "an in-flight job reports an unretired submission");
        GpuPointResamplePollResult poll = GpuPointResamplePollResult::Pending;
        while (poll == GpuPointResamplePollResult::Pending) {
            poll = resampler.resampler->poll();
        }
        expectations.expect(poll == GpuPointResamplePollResult::Ready, "the job completes");
        expectations.expect(!resampler.resampler->hasUnretiredSubmission(),
                            "a retired job reports no unretired submission");
        static_cast<void>(resampler.resampler->take());
    }

    // A poll from a foreign thread must fail closed and leave the pending job retireable on the
    // owner thread.
    const auto pollBegin = resampler.resampler->begin(request, kBudget);
    expectations.expect(pollBegin.code == GpuPointResampleDiagnosticCode::None,
                        "a poll-gate request begins");
    if (pollBegin.code == GpuPointResampleDiagnosticCode::None) {
        GpuPointResamplePollResult foreignPoll = GpuPointResamplePollResult::Ready;
        std::thread pollWorker([&] { foreignPoll = resampler.resampler->poll(); });
        pollWorker.join();
        expectations.expect(foreignPoll == GpuPointResamplePollResult::WrongThread,
                            "poll from a foreign thread reports WrongThread");
        GpuPointResamplePollResult poll = GpuPointResamplePollResult::Pending;
        while (poll == GpuPointResamplePollResult::Pending) {
            poll = resampler.resampler->poll();
        }
        expectations.expect(poll == GpuPointResamplePollResult::Ready,
                            "the owner thread still retires the poll-gate job");
        static_cast<void>(resampler.resampler->take());
    }

    // Cancellation: begin, cancel, then poll to retirement reports Cancelled.
    const auto cancelBegin = resampler.resampler->begin(request, kBudget);
    expectations.expect(cancelBegin.code == GpuPointResampleDiagnosticCode::None,
                        "a cancellable request begins");
    if (cancelBegin.code == GpuPointResampleDiagnosticCode::None) {
        resampler.resampler->cancel();
        GpuPointResamplePollResult poll = GpuPointResamplePollResult::Pending;
        while (poll == GpuPointResamplePollResult::Pending) {
            poll = resampler.resampler->poll();
        }
        expectations.expect(poll == GpuPointResamplePollResult::Failure &&
                                resampler.resampler->diagnostic().code ==
                                    GpuPointResampleDiagnosticCode::Cancelled &&
                                !resampler.resampler->hasUnretiredSubmission(),
                            "cancellation retires and reports Cancelled");
    }

    // Foreign device: an image created on a second device must be refused before any driver work.
    auto secondDevice = GpuDevice::create(GpuDeviceCreationOptions{});
    if (!secondDevice) {
        std::cout << "SKIP: second device unavailable; foreign-device gate not exercised\n";
        return;
    }
    expectations.expect(!resampler.resampler->isBoundTo(*secondDevice.device),
                        "the resampler is not bound to a second device");
    auto secondUploader = bloom::render::GpuImageUpload::create(*secondDevice.device);
    if (!secondUploader) {
        return;
    }
    auto foreignImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), hdrAlphaPixels(5, 3));
    if (!foreignImage) {
        return;
    }
    auto foreignResident = upload(*secondUploader.upload,
                                  std::make_shared<const Rgba32fImage>(std::move(*foreignImage)));
    if (!foreignResident) {
        return;
    }
    auto foreign = std::make_shared<const GpuImage>(std::move(*foreignResident));
    const GpuPointResampleRequest foreignRequest{foreign, *output, 0.5, 0.5};
    const auto refused = resampler.resampler->begin(foreignRequest, kBudget);
    expectations.expect(refused.code == GpuPointResampleDiagnosticCode::InvalidArgument,
                        "a foreign-device source is refused as InvalidArgument");
    expectations.expect(!resampler.resampler->hasUnretiredSubmission(),
                        "a foreign-device refusal submits nothing");
}

void testBudgetRecoveryAndLimits(Expectations& expectations, GpuDevice& device) {
    auto uploader = bloom::render::GpuImageUpload::create(device);
    if (!uploader) {
        expectations.expect(false, "budget uploader created");
        return;
    }
    const ImageWindow sourceWindow = window(0, 0, 1000, 64);
    auto sourceImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), hdrAlphaPixels(1000, 64));
    if (!sourceImage) {
        return;
    }
    auto sourceResident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    if (!sourceResident) {
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
    const auto aspect = PixelAspectRatio::create(2, 1).value();
    const ImageWindow displayWindow = window(-4, 2, 800, 400);

    // A budget-charged pipeline refuses a request whose image exceeds its injected maxImageBytes.
    bloom::render::GpuPointResampleBudgets tight;
    tight.maxImageBytes = 64;
    auto tightResampler = GpuPointResample::create(device, tight);
    const auto tinyOutput = outputDescriptor(4, 4, displayWindow, aspect);
    if (tightResampler && tinyOutput) {
        const GpuPointResampleRequest tooBig{source, *tinyOutput, 0.004, 0.0625};
        const auto refused = tightResampler.resampler->begin(tooBig, kBudget);
        expectations.expect(refused.code == GpuPointResampleDiagnosticCode::OverBudget &&
                                !tightResampler.resampler->hasUnretiredSubmission(),
                            "an injected tight maxImageBytes refuses as OverBudget");
    }

    auto resampler = GpuPointResample::create(device);
    if (!resampler) {
        expectations.expect(false, "budget resampler created");
        return;
    }
    const auto largeOutput = outputDescriptor(5000, 64, displayWindow, aspect);
    if (!largeOutput) {
        return;
    }
    const GpuPointResampleRequest request{source, *largeOutput, 5.0, 1.0};

    // A per-call byteBudget below the requested image is refused before any submission...
    const auto refused = resampler.resampler->begin(request, 8);
    expectations.expect(refused.code == GpuPointResampleDiagnosticCode::OverBudget &&
                            !resampler.resampler->hasUnretiredSubmission(),
                        "a sub-image byteBudget refuses as OverBudget");
    // ...and a valid budget on the same request recovers and completes, including an extent > 4K.
    const auto begin = resampler.resampler->begin(request, kBudget);
    expectations.expect(begin.code == GpuPointResampleDiagnosticCode::None,
                        "a >4K request begins after budget recovery: " + begin.message);
    if (begin.code != GpuPointResampleDiagnosticCode::None) {
        return;
    }
    GpuPointResamplePollResult poll = GpuPointResamplePollResult::Pending;
    while (poll == GpuPointResamplePollResult::Pending) {
        poll = resampler.resampler->poll();
    }
    expectations.expect(poll == GpuPointResamplePollResult::Ready,
                        "the >4K request completes: " + resampler.resampler->diagnostic().message);
    if (poll != GpuPointResamplePollResult::Ready) {
        return;
    }
    auto result = resampler.resampler->take();
    expectations.expect(result.width() == 5000 && result.height() == 64,
                        "the >4K resident output keeps its planned extent");
    const auto readback = bloom::render::readbackResidentImage(result, kBudget);
    if (readback) {
        const auto sourcePixels = hdrAlphaPixels(1000, 64);
        const auto expected = oracleResample(sourcePixels, sourceWindow, 5000, 64, 5.0, 1.0);
        static_cast<void>(allBitsEqual(readback.pixels, expected, expectations, "large >4K"));
    }

    // A plan beyond the live device image limit is refused, never silently allocated.
    const auto hugeOutput = outputDescriptor(200000, 1, displayWindow, aspect);
    if (hugeOutput) {
        const GpuPointResampleRequest huge{source, *hugeOutput, 200.0, 1.0 / 64.0};
        const auto refusedHuge = resampler.resampler->begin(huge, kBudget);
        expectations.expect(refusedHuge.code == GpuPointResampleDiagnosticCode::Unsupported ||
                                refusedHuge.code == GpuPointResampleDiagnosticCode::OverBudget,
                            "a beyond-device-limit plan is refused");
        expectations.expect(!resampler.resampler->hasUnretiredSubmission(),
                            "a device-limit refusal submits nothing");
    }
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
        testGeometryParity(expectations, *device.device);
        testPerRequestCost(expectations, *device.device);
        testOwnershipRejectionsAndCancellation(expectations, *device.device);
        testBudgetRecoveryAndLimits(expectations, *device.device);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures()
                      << " point-resample native expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: GPU point resample native producer\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
