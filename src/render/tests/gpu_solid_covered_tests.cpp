// Tests for the CoveredSolidV1 GPU-resident operation. The CPU oracle is the
// exact CPU vector arm: render::coverageSolidRow() per row followed by the
// separate Float32 opacity multiply (Rgba32f::fromPremultiplied), because the
// GPU only selects a host-built palette entry. Local mode pins the explicit
// loader and requires a device; a hardware-free CI image prints an explicit
// skip. No fake success: a real device must produce a resident image whose
// readback is bit-identical to the CPU reference.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/render/path_raster.hpp>

#include <bloom/core/sha256.hpp>

#include "shaders/solid_covered_spirv.inc"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
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

using bloom::core::Color4d;
using bloom::core::PixelAspectRatio;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuImage;
using bloom::render::GpuSolid;
using bloom::render::GpuSolidDiagnosticCode;
using bloom::render::GpuSolidParameters;
using bloom::render::GpuSolidPollResult;
using bloom::render::ImageWindow;
using bloom::render::Path;
using bloom::render::PathFillRule;
using bloom::render::PathMatrix;
using bloom::render::PathRaster;
using bloom::render::Rgba32f;

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

// The exact CPU vector arm the prepared GpuSceneCoverageSolidCommand replays:
// coverageSolidRow() over the row, then the separate Float32 opacity multiply.
void cpuCoverageReference(const Rgba32f pixel, const float opacity,
                          const std::span<const std::uint8_t> coverage, const std::uint32_t width,
                          const std::uint32_t height, const std::span<Rgba32f> output) noexcept {
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto offset = static_cast<std::size_t>(y) * width;
        const auto rowCoverage = coverage.subspan(offset, width);
        const auto rowOutput = output.subspan(offset, width);
        if (const auto status = bloom::render::coverageSolidRow(rowCoverage, pixel, rowOutput)) {
            (void)status;
            std::abort();
        }
        for (auto& value : rowOutput) {
            const auto faded =
                Rgba32f::fromPremultiplied(value.red() * opacity, value.green() * opacity,
                                           value.blue() * opacity, value.alpha() * opacity);
            value = *faded.value();
        }
    }
}

[[nodiscard]] bool pixelsEqual(const std::span<const Rgba32f> lhs,
                               const std::span<const Rgba32f> rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (lhs[index] != rhs[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] GpuSolidPollResult pollToCompletion(GpuSolid& solid) {
    GpuSolidPollResult poll = GpuSolidPollResult::Pending;
    while (poll == GpuSolidPollResult::Pending) {
        poll = solid.poll();
    }
    return poll;
}

// Runs one covered job and returns the resident readback pixels.
[[nodiscard]] bool runCovered(GpuSolid& solid, const GpuSolidParameters& base,
                              const std::span<const std::uint8_t> coverage, const float opacity,
                              std::vector<Rgba32f>& pixels) {
    const auto begin = solid.beginCovered(base, coverage, opacity, 1ULL << 32ULL);
    if (begin.code != GpuSolidDiagnosticCode::None) {
        return false;
    }
    if (pollToCompletion(solid) != GpuSolidPollResult::Ready) {
        return false;
    }
    const auto readback = solid.readback();
    if (!readback.hasValue()) {
        return false;
    }
    pixels = readback.pixels;
    return true;
}

void expectMatchesCpu(Expectations& expectations, GpuSolid& solid, const Rgba32f pixel,
                      const ImageWindow dataWindow, const ImageWindow displayWindow,
                      const PixelAspectRatio pixelAspect, const float opacity,
                      const std::span<const std::uint8_t> coverage, const std::string_view label) {
    const std::uint32_t width = dataWindow.extent().width();
    const std::uint32_t height = dataWindow.extent().height();
    std::vector<Rgba32f> reference(static_cast<std::size_t>(width) * height,
                                   Rgba32f::transparent());
    cpuCoverageReference(pixel, opacity, coverage, width, height, reference);
    const GpuSolidParameters base{pixel, dataWindow, displayWindow, pixelAspect};
    std::vector<Rgba32f> measured;
    const bool ran = runCovered(solid, base, coverage, opacity, measured);
    expectations.expect(ran, label);
    if (!ran) {
        return;
    }
    expectations.expect(pixelsEqual(measured, reference),
                        "the covered resident image is bit-identical to the CPU vector arm");
    const bloom::render::GpuImage* const image = solid.image();
    expectations.expect(image != nullptr && image->isValid(),
                        "a resident covered image is published");
    if (image != nullptr) {
        expectations.expect(image->dataWindow() == dataWindow,
                            "the covered data window is preserved");
        expectations.expect(image->displayWindow() == displayWindow,
                            "the covered display window/PAR metadata is preserved");
        expectations.expect(image->pixelAspect() == pixelAspect,
                            "the covered pixel aspect is preserved");
    }
}

// 256 entry, 256 coverage values: every possible coverage byte in one row.
[[nodiscard]] std::vector<std::uint8_t> allCoverageRow() {
    std::vector<std::uint8_t> coverage(256);
    for (std::size_t index = 0; index < coverage.size(); ++index) {
        coverage[index] = static_cast<std::uint8_t>(index);
    }
    return coverage;
}

[[nodiscard]] std::vector<std::uint8_t> allCoverageGrid() {
    std::vector<std::uint8_t> coverage(256);
    for (std::size_t index = 0; index < coverage.size(); ++index) {
        coverage[index] = static_cast<std::uint8_t>(index);
    }
    return coverage;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> pathCoverage(const ImageWindow window) {
    const std::array<Path, 1> paths{bloom::render::rectanglePath(20.0, 12.0)};
    const PathMatrix matrix{1.0, 0.0, 0.0, 1.0, 0.3, 0.3};
    const auto raster = PathRaster::transformed(paths, {}, matrix, 1.0, 1.0);
    if (!raster) {
        return std::nullopt;
    }
    const std::uint32_t width = window.extent().width();
    const std::uint32_t height = window.extent().height();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * height, 0);
    for (std::int64_t y = window.originY(); y < window.maxYExclusive(); ++y) {
        const auto offset = static_cast<std::size_t>(y - window.originY()) * width;
        if (!raster.value()->coverageRow(window.originX(), y,
                                         std::span<std::uint8_t>(bytes.data() + offset, width),
                                         PathFillRule::NonZero, false)) {
            return std::nullopt;
        }
    }
    return bytes;
}

// Pins the checked-in source -> SPIR-V -> embedded-array chain for CoveredSolidV1.
void testEmbeddedSpirvDigest(Expectations& expectations) {
    static_assert(bloom::render::vulkan_detail::kSolidCoveredSpirvWordCount * 4U ==
                      bloom::render::vulkan_detail::kSolidCoveredSpirvByteCount,
                  "SPIR-V word count must exactly cover the byte count");
    const auto* raw =
        reinterpret_cast<const std::byte*>(bloom::render::vulkan_detail::kSolidCoveredSpirvCode);
    const std::span<const std::byte> bytes(
        raw, bloom::render::vulkan_detail::kSolidCoveredSpirvByteCount);
    const auto digest = bloom::core::Sha256Hasher::hash(bytes);
    expectations.expect(digest.has_value(), "the embedded CoveredSolidV1 SPIR-V hashes");
    if (!digest.has_value()) {
        return;
    }
    const auto hex = digest->toLowercaseHex();
    const std::string_view measured(hex.data(), hex.size());
    expectations.expect(measured == BLOOM_SOLID_COVERED_SPV_SHA256,
                        "the embedded CoveredSolidV1 SPIR-V array hashes to the pinned digest");
    expectations.expect(std::string_view(bloom::render::vulkan_detail::kSolidCoveredSpirvDigest) ==
                            BLOOM_SOLID_COVERED_SPV_SHA256,
                        "the .inc digest comment matches the pinned covered SPIR-V digest");
}

void testCoverageMatrix(Expectations& expectations, GpuSolid& solid) {
    const auto square = PixelAspectRatio::square();
    const auto odd = ImageWindow::create(-3, 5, 7, 3);
    const auto oddDisplay = ImageWindow::create(-5, 3, 12, 8);
    const auto par = PixelAspectRatio::create(4, 3);
    expectations.expect(static_cast<bool>(odd) && static_cast<bool>(oddDisplay) && par.has_value(),
                        "covered windows and pixel aspect build");
    if (!odd || !oddDisplay || !par.has_value()) {
        return;
    }

    const auto row = allCoverageRow();
    const auto grid = allCoverageGrid();
    std::vector<std::uint8_t> oddCoverage(static_cast<std::size_t>(7) * 3);
    for (std::size_t index = 0; index < oddCoverage.size(); ++index) {
        oddCoverage[index] = static_cast<std::uint8_t>((index * 37U + 5U) & 0xFFU);
    }
    const auto rowWindow = ImageWindow::create(0, 0, 256, 1);
    const auto gridWindow = ImageWindow::create(4, -2, 16, 16);
    expectations.expect(static_cast<bool>(rowWindow) && static_cast<bool>(gridWindow),
                        "coverage windows build");
    if (!rowWindow || !gridWindow) {
        return;
    }

    const auto solidPixels = std::array<Color4d, 5>{
        Color4d{0.25, 0.5, 0.75, 1.0}, Color4d{1.0, 0.0, 0.5, 0.0}, Color4d{-0.25, 4.0, 0.125, 0.5},
        Color4d{static_cast<double>(std::numeric_limits<float>::denorm_min()), 0.0, 0.0, 1.0},
        Color4d{0.1, -2.0, 8.0, 0.75}};
    const auto opacities = std::array<float, 3>{0.0F, 0.3F, 1.0F};

    for (const auto color : solidPixels) {
        const auto pixel = bloom::render::solidPixelFromStraightLinearRec709Scene(color);
        expectations.expect(static_cast<bool>(pixel), "the covered solid pixel builds");
        if (!pixel) {
            continue;
        }
        for (const float opacity : opacities) {
            expectMatchesCpu(expectations, solid, *pixel.value(), *rowWindow.value(),
                             *rowWindow.value(), square, opacity, row, "all-256 coverage row");
            expectMatchesCpu(expectations, solid, *pixel.value(), *gridWindow.value(),
                             *gridWindow.value(), square, opacity, grid, "all-256 coverage grid");
            expectMatchesCpu(expectations, solid, *pixel.value(), *odd.value(), *oddDisplay.value(),
                             par.value(), opacity, oddCoverage, "odd/nonzero-origin/PAR coverage");
        }
    }
}

void testPathRasterPattern(Expectations& expectations, GpuSolid& solid) {
    const auto window = ImageWindow::create(-2, -2, 24, 16);
    const auto display = ImageWindow::create(-2, -2, 24, 16);
    expectations.expect(static_cast<bool>(window) && static_cast<bool>(display),
                        "the path coverage window builds");
    if (!window || !display) {
        return;
    }
    const auto coverage = pathCoverage(*window.value());
    expectations.expect(coverage.has_value(), "the fractional PathRaster coverage rasterizes");
    if (!coverage.has_value()) {
        return;
    }
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.2, 0.6, 0.9, 1.0});
    expectations.expect(static_cast<bool>(pixel), "the path coverage pixel builds");
    if (!pixel) {
        return;
    }
    for (const float opacity : std::array<float, 2>{0.3F, 1.0F}) {
        expectMatchesCpu(expectations, solid, *pixel.value(), *window.value(), *display.value(),
                         PixelAspectRatio::square(), opacity,
                         std::span<const std::uint8_t>(coverage->data(), coverage->size()),
                         "fractional PathRaster rectangle coverage");
    }
}

void testRejectionsAndGuards(Expectations& expectations, GpuSolid& solid, GpuDevice& device,
                             const std::filesystem::path& loaderPath) {
    const auto window = ImageWindow::create(0, 0, 8, 4);
    expectations.expect(static_cast<bool>(window), "the guard window builds");
    if (!window) {
        return;
    }
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.5, 0.5, 0.5, 1.0});
    const GpuSolidParameters base{*pixel.value(), *window.value(), *window.value(),
                                  PixelAspectRatio::square()};
    const std::vector<std::uint8_t> coverage(static_cast<std::size_t>(8) * 4, 128);

    // Wrong coverage byte count.
    const std::vector<std::uint8_t> shortCoverage(4, 128);
    const auto shortBegin = solid.beginCovered(base, shortCoverage, 0.5F, 1ULL << 32ULL);
    expectations.expect(shortBegin.code == GpuSolidDiagnosticCode::InvalidArgument,
                        "a coverage count that is not width*height is rejected");

    // Non-finite and out-of-range opacity.
    const auto nanOpacity =
        solid.beginCovered(base, coverage, std::numeric_limits<float>::quiet_NaN(), 1ULL << 32ULL);
    expectations.expect(nanOpacity.code == GpuSolidDiagnosticCode::InvalidArgument,
                        "a non-finite opacity is rejected");
    const auto highOpacity = solid.beginCovered(base, coverage, 2.0F, 1ULL << 32ULL);
    expectations.expect(highOpacity.code == GpuSolidDiagnosticCode::InvalidArgument,
                        "an opacity above one is rejected");

    // Budget refusal.
    const auto tightBudget = solid.beginCovered(base, coverage, 0.5F, 4);
    expectations.expect(tightBudget.code == GpuSolidDiagnosticCode::OverBudget,
                        "an under-budget covered request is rejected");

    // Cancellation publishes nothing and leaves the pipeline reusable.
    const auto cancelBegin = solid.beginCovered(base, coverage, 0.5F, 1ULL << 32ULL);
    expectations.expect(cancelBegin.code == GpuSolidDiagnosticCode::None,
                        "the cancellable covered begin is accepted");
    solid.cancel();
    expectations.expect(pollToCompletion(solid) == GpuSolidPollResult::Failure &&
                            solid.diagnostic().code == GpuSolidDiagnosticCode::Cancelled,
                        "a cancelled covered job reports Cancelled and publishes nothing");
    expectations.expect(solid.image() == nullptr,
                        "no covered image is published after cancellation");

    // Wrong-thread poll fails closed.
    auto foreignPoll = GpuSolidPollResult::Ready;
    std::thread worker([&solid, &foreignPoll]() { foreignPoll = solid.poll(); });
    worker.join();
    expectations.expect(foreignPoll == GpuSolidPollResult::WrongThread,
                        "a covered poll from a joined non-owner thread is WrongThread");

    // Binding checks. A second GpuDevice must not own this pipeline; if the second
    // device cannot be created, the same-device binding assertion still runs.
    expectations.expect(solid.isBoundTo(device), "the covered pipeline is bound to its device");
    GpuDeviceCreationOptions secondOptions;
    secondOptions.loader_path = loaderPath;
    auto second = GpuDevice::create(secondOptions);
    if (second) {
        expectations.expect(!solid.isBoundTo(*second.device),
                            "a covered pipeline is not bound to a different device");
    }
}

void testLifetime(Expectations& expectations, GpuDevice& device) {
    const auto window = ImageWindow::create(2, -1, 5, 3);
    expectations.expect(static_cast<bool>(window), "the lifetime window builds");
    if (!window) {
        return;
    }
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.5, 0.25, 0.125, 1.0});
    const std::vector<std::uint8_t> coverage(static_cast<std::size_t>(5) * 3, 200);
    auto created = GpuSolid::create(device);
    expectations.expect(created.hasValue(), "the lifetime covered pipeline is created");
    if (!created) {
        return;
    }
    std::vector<Rgba32f> measured;
    expectations.expect(runCovered(*created.solid,
                                   GpuSolidParameters{*pixel.value(), *window.value(),
                                                      *window.value(), PixelAspectRatio::square()},
                                   coverage, 0.75F, measured),
                        "the lifetime covered job runs");
    GpuImage taken = created.solid->takeImage();
    expectations.expect(taken.isValid() && created.solid->image() == nullptr,
                        "takeImage moves the covered image out");
    created.solid.reset();
    const auto afterDestroy =
        bloom::render::readbackResidentImage(taken, 512ULL * 1024ULL * 1024ULL);
    expectations.expect(afterDestroy.hasValue(),
                        "the taken covered image stays readable after its pipeline is destroyed");
}

void testPerformance(Expectations& expectations, GpuSolid& solid) {
    struct Size final {
        std::uint32_t width;
        std::uint32_t height;
    };
    const std::array<Size, 3> sizes{{{1280, 720}, {1920, 1080}, {3840, 2160}}};
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.2, 0.4, 0.6, 1.0});
    expectations.expect(static_cast<bool>(pixel), "the performance pixel builds");
    if (!pixel) {
        return;
    }
    std::cout << "covered-solid performance (ms; CPU full RGBA fill vs native covered "
                 "begin->ready, upload included, no readback)\n";
    for (const auto size : sizes) {
        const auto window = ImageWindow::create(0, 0, size.width, size.height);
        if (!window) {
            expectations.expect(false, "the performance window builds");
            continue;
        }
        const std::size_t pixels = static_cast<std::size_t>(size.width) * size.height;
        std::vector<std::uint8_t> coverage(pixels);
        for (std::size_t index = 0; index < pixels; ++index) {
            coverage[index] =
                static_cast<std::uint8_t>((index * 7U + (index / size.width) * 13U) & 0xFFU);
        }
        std::vector<Rgba32f> reference(pixels, Rgba32f::transparent());
        const GpuSolidParameters base{*pixel.value(), *window.value(), *window.value(),
                                      PixelAspectRatio::square()};
        const float opacity = 0.3F;

        const auto cpuRun = [&]() {
            cpuCoverageReference(*pixel.value(), opacity, coverage, size.width, size.height,
                                 reference);
        };
        const auto gpuRun = [&]() {
            if (solid.beginCovered(base, coverage, opacity, 1ULL << 32ULL).code !=
                GpuSolidDiagnosticCode::None) {
                expectations.expect(false, "the performance covered begin is accepted");
                return;
            }
            (void)pollToCompletion(solid);
        };
        for (int warmup = 0; warmup < 5; ++warmup) {
            cpuRun();
            gpuRun();
        }
        std::vector<double> cpuSamples;
        std::vector<double> gpuSamples;
        for (int sample = 0; sample < 10; ++sample) {
            const auto cpuStart = std::chrono::steady_clock::now();
            cpuRun();
            const auto cpuStop = std::chrono::steady_clock::now();
            const auto gpuStart = std::chrono::steady_clock::now();
            gpuRun();
            const auto gpuStop = std::chrono::steady_clock::now();
            cpuSamples.push_back(
                std::chrono::duration<double, std::milli>(cpuStop - cpuStart).count());
            gpuSamples.push_back(
                std::chrono::duration<double, std::milli>(gpuStop - gpuStart).count());
        }
        std::sort(cpuSamples.begin(), cpuSamples.end());
        std::sort(gpuSamples.begin(), gpuSamples.end());
        std::cout << "  " << size.width << 'x' << size.height << ": cpu " << cpuSamples[5]
                  << " ms, gpu " << gpuSamples[5] << " ms\n";
    }
    // Release the last resident image before the pipeline is destroyed.
    (void)solid.takeImage();
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
        Expectations expectations;
        testEmbeddedSpirvDigest(expectations);

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
            return expectations.ok() ? 0 : 1;
        }

        auto created = GpuSolid::create(*device.device);
        expectations.expect(created.hasValue(), "the SolidV1 pipeline is created");
        if (!created) {
            std::cerr << "FAIL: SolidV1 create failed: " << created.diagnostic.message << '\n';
            return 1;
        }
        GpuSolid& solid = *created.solid;

        testCoverageMatrix(expectations, solid);
        testPathRasterPattern(expectations, solid);
        testRejectionsAndGuards(expectations, solid, *device.device, options.loader_path);
        testLifetime(expectations, *device.device);
        testPerformance(expectations, solid);

        if (!expectations.ok()) {
            std::cerr << "FAIL: covered solid native checks failed\n";
            return 1;
        }
        std::cout << "PASS: covered solid native checks\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
