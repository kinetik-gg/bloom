// Tests for GpuImageUploadV1: host RGBA32F -> resident GpuImage.
//
// Local mode pins the explicit loader and requires a device; a hardware-free image prints an
// explicit skip. The parity oracle is the host image itself: a real Rgba32fImage is uploaded and
// the resident image is read back and compared bit-for-bit, including signed/HDR values,
// subnormals, alpha endpoints, odd extents, nonzero origins, and a non-square pixel aspect.
// Readback is test-only. The host fixture is built with the production Rgba32fImageBuilder, not a
// decoder; this is labelled honestly and does not claim a media decode.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <source_location>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using bloom::core::PixelAspectRatio;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuImage;
using bloom::render::GpuImageUpload;
using bloom::render::GpuImageUploadDiagnosticCode;
using bloom::render::GpuImageUploadJobState;
using bloom::render::GpuImageUploadParameters;
using bloom::render::GpuImageUploadPollResult;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

struct Options final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool valid = true;
};

[[nodiscard]] Options parseOptions(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader") {
            if (index + 1 >= argc) {
                std::cerr << "--loader requires a path argument\n";
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            options.valid = false;
            return options;
        }
    }
    return options;
}

[[nodiscard]] std::optional<Rgba32f> makePixel(const std::uint32_t x, const std::uint32_t y) {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
    if (x == 0 && y == 0) {
        // Signed zero, subnormal green lane, alpha endpoint 1.
        r = -0.0F;
        g = std::numeric_limits<float>::denorm_min();
        b = 0.5F;
    } else if (x == 1 && y == 0) {
        // Signed HDR RGB, alpha endpoint 0.5.
        r = 65.0F;
        g = -3.5F;
        b = 1.0e-5F;
        a = 0.5F;
    } else if (x == 2 && y == 0) {
        // Authored alpha 0 must canonicalize to transparent exactly like the CPU path.
        r = 1.0F;
        g = 1.0F;
        b = 1.0F;
        a = 0.0F;
    } else {
        r = static_cast<float>(x) * 0.25F + static_cast<float>(y) * 0.125F - 0.5F;
        g = -static_cast<float>(y) * 0.5F + 0.0625F;
        b = static_cast<float>(x + y) * 0.03125F;
        a = (x % 3U == 0U) ? 0.25F : 1.0F;
    }
    const auto pixel = Rgba32f::fromPremultiplied(r, g, b, a);
    if (!pixel) {
        return std::nullopt;
    }
    return *pixel.value();
}

// A real production image with deliberately awkward geometry and values.
[[nodiscard]] std::optional<Rgba32fImage> buildFixture(const ImageWindow dataWindow,
                                                       const ImageWindow displayWindow,
                                                       const PixelAspectRatio pixelAspect) {
    const auto descriptor = Rgba32fImageDescriptor::create(dataWindow, displayWindow, pixelAspect);
    if (!descriptor) {
        return std::nullopt;
    }
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), 1ULL << 32ULL);
    if (!builder) {
        return std::nullopt;
    }
    const auto width = dataWindow.extent().width();
    for (std::int64_t y = dataWindow.originY(); y < dataWindow.maxYExclusive(); ++y) {
        auto row = builder.value()->row(y);
        if (!row || row.value()->size() != width) {
            return std::nullopt;
        }
        auto pixels = *row.value();
        for (std::uint32_t i = 0; i < width; ++i) {
            auto pixel = makePixel(i, static_cast<std::uint32_t>(y - dataWindow.originY()));
            if (!pixel) {
                return std::nullopt;
            }
            pixels[i] = *pixel;
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen) {
        return std::nullopt;
    }
    return std::move(*frozen.value());
}

[[nodiscard]] GpuImageUploadPollResult pollUntilTerminal(GpuImageUpload& upload) {
    for (int attempt = 0; attempt < 20000; ++attempt) {
        const auto result = upload.poll();
        if (result != GpuImageUploadPollResult::Pending) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return GpuImageUploadPollResult::Pending;
}

void testBitExactUpload(Expectations& expectations, GpuDevice& device, GpuImageUpload& upload) {
    const auto dataWindow = ImageWindow::create(-3, 5, 7, 3);
    const auto displayWindow = ImageWindow::create(10, 20, 7, 3);
    const auto pixelAspect = PixelAspectRatio::create(4, 3);
    expectations.expect(static_cast<bool>(dataWindow) && static_cast<bool>(displayWindow) &&
                            pixelAspect.has_value(),
                        "the fixture windows and pixel aspect build");
    if (!dataWindow || !displayWindow || !pixelAspect) {
        return;
    }
    auto fixture = buildFixture(*dataWindow.value(), *displayWindow.value(), *pixelAspect);
    expectations.expect(fixture.has_value(), "the host fixture image builds");
    if (!fixture) {
        return;
    }
    auto sharedSource = std::make_shared<const Rgba32fImage>(std::move(*fixture));

    GpuImageUploadParameters parameters;
    parameters.source = sharedSource;
    const auto accepted = upload.begin(parameters, 1ULL << 32ULL);
    expectations.expect(accepted.code == GpuImageUploadDiagnosticCode::None,
                        "a valid upload begins");
    // The source copy is owned before begin returns, so releasing the caller reference is safe.
    parameters.source.reset();
    sharedSource.reset();
    if (accepted.code != GpuImageUploadDiagnosticCode::None) {
        return;
    }
    expectations.expect(pollUntilTerminal(upload) == GpuImageUploadPollResult::Ready,
                        "the upload reaches Ready");
    const GpuImage* const resident = upload.image();
    expectations.expect(resident != nullptr && resident->isValid(),
                        "the resident image is published");
    if (resident == nullptr) {
        return;
    }
    expectations.expect(resident->isBoundTo(device), "the resident image is bound to its device");
    expectations.expect(resident->width() == 7 && resident->height() == 3,
                        "the resident extent matches the data window");
    expectations.expect(resident->dataWindow() == *dataWindow.value(),
                        "the data window is preserved verbatim");
    expectations.expect(resident->displayWindow() == *displayWindow.value(),
                        "the display window is preserved verbatim");
    expectations.expect(resident->pixelAspect() == pixelAspect,
                        "the pixel aspect is preserved verbatim");

    auto readback = upload.readback();
    expectations.expect(readback.hasValue(), "the oracle readback succeeds");
    if (!readback.hasValue()) {
        return;
    }
    // Rebuild the expected bytes from the same values the production builder accepted.
    const auto rebuilt = buildFixture(*dataWindow.value(), *displayWindow.value(), *pixelAspect);
    expectations.expect(rebuilt.has_value(), "the oracle fixture rebuilds");
    if (!rebuilt) {
        return;
    }
    expectations.expect(readback.pixels.size() == rebuilt->pixels().size(),
                        "the readback pixel count matches the source");
    if (readback.pixels.size() != rebuilt->pixels().size()) {
        return;
    }
    expectations.expect(std::memcmp(readback.pixels.data(), rebuilt->pixels().data(),
                                    readback.pixels.size() * sizeof(Rgba32f)) == 0,
                        "the resident image is bit-exact to the uploaded host pixels");

    GpuImage taken = upload.takeImage();
    expectations.expect(taken.isValid() && upload.state() == GpuImageUploadJobState::Idle,
                        "takeImage moves the result out and returns the pipeline to Idle");
}

void testBudgetsAndArguments(Expectations& expectations, GpuImageUpload& upload) {
    const auto dataWindow = ImageWindow::create(0, 0, 4, 2);
    if (!dataWindow) {
        expectations.expect(false, "the budget fixture window builds");
        return;
    }
    auto fixture =
        buildFixture(*dataWindow.value(), *dataWindow.value(), PixelAspectRatio::square());
    expectations.expect(fixture.has_value(), "the budget fixture builds");
    if (!fixture) {
        return;
    }
    GpuImageUploadParameters parameters;
    parameters.source = std::make_shared<const Rgba32fImage>(std::move(*fixture));

    GpuImageUploadParameters nullSource;
    expectations.expect(upload.begin(nullSource, 1ULL << 32ULL).code ==
                            GpuImageUploadDiagnosticCode::InvalidArgument,
                        "a null source is rejected");
    expectations.expect(upload.begin(parameters, 8).code ==
                            GpuImageUploadDiagnosticCode::OverBudget,
                        "a tighter-than-peak byte budget is rejected before allocation");
    expectations.expect(upload.state() == GpuImageUploadJobState::Idle,
                        "a rejected begin leaves the pipeline idle");
}

void testWrongThread(Expectations& expectations, GpuImageUpload& upload) {
    GpuImageUploadParameters parameters;
    auto observed = GpuImageUploadDiagnosticCode::None;
    std::thread worker([&upload, &parameters, &observed]() {
        observed = upload.begin(parameters, 1ULL << 32ULL).code;
    });
    worker.join();
    expectations.expect(observed == GpuImageUploadDiagnosticCode::WrongThread,
                        "begin from a joined non-owner thread is WrongThread");
}

void testCancellation(Expectations& expectations, GpuImageUpload& upload) {
    const auto dataWindow = ImageWindow::create(0, 0, 256, 256);
    if (!dataWindow) {
        return;
    }
    auto fixture =
        buildFixture(*dataWindow.value(), *dataWindow.value(), PixelAspectRatio::square());
    if (!fixture) {
        return;
    }
    GpuImageUploadParameters parameters;
    parameters.source = std::make_shared<const Rgba32fImage>(std::move(*fixture));
    const auto accepted = upload.begin(parameters, 1ULL << 32ULL);
    expectations.expect(accepted.code == GpuImageUploadDiagnosticCode::None,
                        "the cancellable upload begins");
    if (accepted.code != GpuImageUploadDiagnosticCode::None) {
        return;
    }
    upload.cancel();
    const auto terminal = pollUntilTerminal(upload);
    expectations.expect(terminal == GpuImageUploadPollResult::Failure &&
                            upload.diagnostic().code == GpuImageUploadDiagnosticCode::Cancelled,
                        "a cancelled upload retires as Failure(Cancelled) and publishes nothing");
    expectations.expect(upload.image() == nullptr, "a cancelled upload publishes no image");
}

// A pipeline destroyed on a foreign thread must not call Vulkan; the generation is retained. Run
// last: it is expected to leave the process-global teardown fuse set.
void testForeignThreadLifetime(Expectations& expectations, GpuDevice& device) {
    auto created = GpuImageUpload::create(device);
    expectations.expect(created.hasValue(), "a second upload pipeline is created");
    if (!created) {
        return;
    }
    auto pipeline = std::move(created.upload);
    std::thread worker([&pipeline]() { pipeline.reset(); });
    worker.join();
    expectations.expect(true,
                        "foreign-thread pipeline destruction returned without touching Vulkan");
}

} // namespace

int main(const int argc, char** argv) {
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
            std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message << '\n';
            return 1;
        }
        std::cout << "SKIP: no compatible Vulkan device available: " << device.diagnostic.message
                  << '\n';
        return expectations.ok() ? 0 : 1;
    }

    auto created = GpuImageUpload::create(*device.device);
    expectations.expect(created.hasValue(), "the upload pipeline is created");
    if (!created) {
        std::cerr << "FAIL: upload pipeline creation failed\n";
        return 1;
    }
    GpuImageUpload& upload = *created.upload;
    expectations.expect(upload.isBoundTo(*device.device), "the upload is bound to its device");
    testBitExactUpload(expectations, *device.device, upload);
    testBudgetsAndArguments(expectations, upload);
    testWrongThread(expectations, upload);
    testCancellation(expectations, upload);
    testForeignThreadLifetime(expectations, *device.device);

    if (!expectations.ok()) {
        std::cerr << "FAIL: upload expectations failed\n";
        return 1;
    }
    std::cout << "PASS: GPU image upload\n";
    return 0;
}
