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

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
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

    const auto wrongWindow = ImageWindow::create(0, 0, 5, 2);
    const auto display = ImageWindow::create(0, 0, 8, 6);
    const auto aspect = PixelAspectRatio::create(4, 3);
    if (!wrongWindow || !display || !aspect) {
        expectations.expect(false, "descriptor override fixture builds");
        return;
    }
    const auto wrongDescriptor =
        Rgba32fImageDescriptor::create(*wrongWindow.value(), *display.value(), *aspect);
    if (!wrongDescriptor) {
        expectations.expect(false, "mismatched descriptor override builds");
        return;
    }
    parameters.descriptorOverride = *wrongDescriptor.value();
    expectations.expect(upload.begin(parameters, 1ULL << 32ULL).code ==
                            GpuImageUploadDiagnosticCode::InvalidArgument,
                        "an override with a different packed layout is rejected");
    const auto requestDescriptor =
        Rgba32fImageDescriptor::create(*dataWindow.value(), *display.value(), *aspect);
    if (!requestDescriptor) {
        expectations.expect(false, "request descriptor override builds");
        return;
    }
    parameters.descriptorOverride = *requestDescriptor.value();
    const auto accepted = upload.begin(parameters, 1ULL << 32ULL);
    expectations.expect(accepted.code == GpuImageUploadDiagnosticCode::None,
                        "a metadata-only descriptor override begins without a pixel copy");
    if (accepted.code != GpuImageUploadDiagnosticCode::None)
        return;
    expectations.expect(pollUntilTerminal(upload) == GpuImageUploadPollResult::Ready,
                        "an overridden upload reaches Ready");
    const auto* resident = upload.image();
    expectations.expect(resident != nullptr && resident->dataWindow() == *dataWindow.value() &&
                            resident->displayWindow() == *display.value() &&
                            resident->pixelAspect() == aspect,
                        "the native resident carries the request display and PAR");
    (void)upload.takeImage();
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

// #401: a large upload's staging copy runs as chunks through the caller's parallel-for. Chunks
// copied out of order on other threads must still give a bit-exact resident image.
void testChunkedStagingCopy(Expectations& expectations, GpuImageUpload& upload) {
    const auto window = ImageWindow::create(0, 0, 1280, 720); // 14.7 MB: two 8 MiB chunks
    const auto pixelAspect = PixelAspectRatio::create(1, 1);
    if (!window || !pixelAspect) {
        expectations.expect(false, "the chunked fixture windows build");
        return;
    }
    auto fixture = buildFixture(*window.value(), *window.value(), *pixelAspect);
    expectations.expect(fixture.has_value(), "the chunked fixture builds");
    if (!fixture)
        return;
    const auto source = std::make_shared<const Rgba32fImage>(std::move(*fixture));
    std::atomic<std::size_t> chunksRun{0};
    std::size_t chunksAsked = 0;
    GpuImageUploadParameters parameters;
    parameters.source = source;
    parameters.parallelFor = [&](const std::size_t count,
                                 const std::function<void(std::size_t)>& body) {
        chunksAsked = count;
        std::vector<std::thread> threads;
        for (std::size_t index = count; index-- > 0;)
            threads.emplace_back([&body, &chunksRun, index] {
                body(index);
                chunksRun.fetch_add(1);
            });
        for (auto& thread : threads)
            thread.join();
    };
    const auto accepted = upload.begin(parameters, 1ULL << 32ULL);
    expectations.expect(accepted.code == GpuImageUploadDiagnosticCode::None,
                        "a chunked upload begins");
    if (accepted.code != GpuImageUploadDiagnosticCode::None)
        return;
    // This fixture is not half-exact, so a half-staging attempt may run a pass before the float
    // copy; every pass is two chunks and every chunk runs.
    expectations.expect(chunksAsked == 2 && chunksRun.load() >= 2 && chunksRun.load() % 2 == 0,
                        "a 14.7 MB staging copy runs as two parallel chunks");
    expectations.expect(pollUntilTerminal(upload) == GpuImageUploadPollResult::Ready,
                        "the chunked upload reaches Ready");
    auto readback = upload.readback();
    expectations.expect(readback.hasValue() && readback.pixels.size() == source->pixels().size() &&
                            std::memcmp(readback.pixels.data(), source->pixels().data(),
                                        readback.pixels.size() * sizeof(Rgba32f)) == 0,
                        "chunks copied out of order give a bit-exact resident image");
    GpuImage taken = upload.takeImage();
    expectations.expect(taken.isValid(), "the chunked upload result is taken");
}

// #401: an opted-in proven-retired job's staging buffer is reused by the next opted-in upload
// instead of a fresh mapped allocation per upload; the reused buffer is fully overwritten, and
// releasing the spare (the executor does at scene end) makes the next upload allocate again. A job
// that did not opt in releases its staging at its fence and keeps no spare, so its phase and grant
// retire with it.
void testStagingReuse(Expectations& expectations, GpuImageUpload& upload) {
    const auto window = ImageWindow::create(0, 0, 640, 360);
    const auto pixelAspect = PixelAspectRatio::create(1, 1);
    if (!window || !pixelAspect) {
        expectations.expect(false, "the reuse fixture windows build");
        return;
    }
    const auto uploadOnce = [&](const bool checkPixels, const bool reuse = true) {
        auto fixture = buildFixture(*window.value(), *window.value(), *pixelAspect);
        if (!fixture)
            return false;
        const auto source = std::make_shared<const Rgba32fImage>(std::move(*fixture));
        GpuImageUploadParameters parameters;
        parameters.source = source;
        parameters.reuseStagingWithinScene = reuse;
        if (upload.begin(parameters, 1ULL << 32ULL).code != GpuImageUploadDiagnosticCode::None ||
            pollUntilTerminal(upload) != GpuImageUploadPollResult::Ready)
            return false;
        bool exact = true;
        if (checkPixels) {
            auto readback = upload.readback();
            exact = readback.hasValue() && readback.pixels.size() == source->pixels().size() &&
                    std::memcmp(readback.pixels.data(), source->pixels().data(),
                                readback.pixels.size() * sizeof(Rgba32f)) == 0;
        }
        GpuImage taken = upload.takeImage();
        return exact && taken.isValid();
    };
    upload.releaseSpareStaging();
    const auto before = upload.stagingAllocations();
    expectations.expect(uploadOnce(false) && upload.stagingAllocations() == before + 1,
                        "the first upload allocates its staging buffer");
    expectations.expect(uploadOnce(true) && upload.stagingAllocations() == before + 1,
                        "the next upload reuses the retired staging buffer bit-exactly");
    upload.releaseSpareStaging();
    expectations.expect(uploadOnce(false) && upload.stagingAllocations() == before + 2,
                        "after the spare is released the next upload allocates again");
    expectations.expect(uploadOnce(true, false) && upload.stagingAllocations() == before + 3,
                        "a job that does not opt in never reuses a spare and stays bit-exact");
    expectations.expect(uploadOnce(false, false) && upload.stagingAllocations() == before + 4,
                        "a job that does not opt in allocates its own staging");
    expectations.expect(uploadOnce(false) && upload.stagingAllocations() == before + 5,
                        "a job that does not opt in keeps no spare past its fence");
    upload.releaseSpareStaging();
}

// #401: an image whose every value is exactly representable in binary16 (zeros of both signs,
// infinities-free HDR values, a half subnormal) crosses as RGBA16F and is widened by one blit; the
// resident bytes must equal the source exactly. A source with one inexact value takes the RGBA32F
// copy, also bit-exact.
using HalfValue = std::array<float, 4>;

[[nodiscard]] std::shared_ptr<const Rgba32fImage>
buildValues(const ImageWindow window,
            const std::function<HalfValue(std::uint32_t, std::uint32_t)>& value) {
    const auto pixelAspect = PixelAspectRatio::create(1, 1);
    if (!pixelAspect)
        return nullptr;
    const auto descriptor = Rgba32fImageDescriptor::create(window, window, *pixelAspect);
    if (!descriptor)
        return nullptr;
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), 1ULL << 32ULL);
    if (!builder)
        return nullptr;
    for (std::int64_t y = window.originY(); y < window.maxYExclusive(); ++y) {
        auto row = builder.value()->row(y);
        if (!row)
            return nullptr;
        auto pixels = *row.value();
        for (std::uint32_t x = 0; x < pixels.size(); ++x) {
            const auto v = value(x, static_cast<std::uint32_t>(y - window.originY()));
            const auto pixel = Rgba32f::fromPremultiplied(v[0], v[1], v[2], v[3]);
            if (!pixel)
                return nullptr;
            pixels[x] = *pixel.value();
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return nullptr;
    return std::make_shared<const Rgba32fImage>(std::move(*frozen.value()));
}

void testHalfStagedUpload(Expectations& expectations, GpuImageUpload& upload) {
    const auto window = ImageWindow::create(0, 0, 1280, 720); // two chunks of the packing pass
    if (!window) {
        expectations.expect(false, "the half fixture window builds");
        return;
    }
    // Every value is a binary16 value: k/1024 steps scaled by powers of two, signed zero, the
    // largest half 65504 and the smallest half subnormal 2^-24.
    const auto halfExact = [](const std::uint32_t x, const std::uint32_t y) {
        const float step = static_cast<float>((x * 7U + y * 3U) % 1024U) / 1024.0F;
        if (x == 0 && y == 0)
            return HalfValue{-0.0F, std::ldexp(1.0F, -24), 65504.0F, 1.0F};
        return HalfValue{step * 0.5F, -step * 2.0F, step * 256.0F, 0.5F};
    };
    const auto uploadAndCompare = [&](const std::shared_ptr<const Rgba32fImage>& image) {
        if (image == nullptr)
            return false;
        GpuImageUploadParameters parameters;
        parameters.source = image;
        if (upload.begin(parameters, 1ULL << 32ULL).code != GpuImageUploadDiagnosticCode::None ||
            pollUntilTerminal(upload) != GpuImageUploadPollResult::Ready)
            return false;
        auto readback = upload.readback();
        const bool exact = readback.hasValue() &&
                           readback.pixels.size() == image->pixels().size() &&
                           std::memcmp(readback.pixels.data(), image->pixels().data(),
                                       readback.pixels.size() * sizeof(Rgba32f)) == 0;
        GpuImage taken = upload.takeImage();
        return exact && taken.isValid();
    };
    const auto before = upload.halfStagedUploads();
    expectations.expect(uploadAndCompare(buildValues(*window.value(), halfExact)),
                        "a half-exact upload is bit-exact");
    const bool halfTaken = upload.halfStagedUploads() == before + 1;
    // A device without a graphics-capable upload queue or blit support keeps the float path.
    std::cout << "half staging " << (halfTaken ? "used" : "unavailable on this device") << '\n';
    const auto inexact = buildValues(*window.value(), [&](const auto x, const auto y) {
        auto value = halfExact(x, y);
        if (x == 640 && y == 360)
            value[1] = 0.1F; // not a binary16 value
        return value;
    });
    expectations.expect(uploadAndCompare(inexact) &&
                            upload.halfStagedUploads() == (halfTaken ? before + 1 : before),
                        "one inexact value keeps the RGBA32F copy, still bit-exact");
}

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
    testChunkedStagingCopy(expectations, upload);
    testStagingReuse(expectations, upload);
    testHalfStagedUpload(expectations, upload);
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
