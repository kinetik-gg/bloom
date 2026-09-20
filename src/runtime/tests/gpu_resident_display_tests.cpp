// Tests for the GPU-resident Bloom Neutral v1 display operation. The resident RGBA32F input comes
// from the SolidV1 operation; the resident RGBA8 display output is compared to the independent CPU
// OCIO oracle. Local mode pins the explicit loader and requires a device; a hardware-free image
// prints an explicit skip and the CPU-unavailable stub exercises the typed-unavailable path.

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using bloom::core::Color4d;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuImage;
using bloom::render::GpuResidentDisplay;
using bloom::render::GpuResidentDisplayDiagnosticCode;
using bloom::render::GpuResidentDisplayJobState;
using bloom::render::GpuResidentDisplayPollResult;
using bloom::render::GpuSolid;
using bloom::render::GpuSolidDiagnosticCode;
using bloom::render::GpuSolidParameters;
using bloom::render::GpuSolidPollResult;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::render::Rgba8;

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

[[nodiscard]] std::optional<bloom::color::PreparedCpuDisplayProcessorHandle> buildCpuProcessor() {
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    if (!resolution.ready()) {
        return std::nullopt;
    }
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    auto built = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    if (!built) {
        return std::nullopt;
    }
    return std::move(built).takeHandle();
}

// CPU oracle: the same solid primitive into a real image, then the qualified CPU display processor.
[[nodiscard]] std::optional<std::vector<Rgba8>>
cpuDisplayOracle(const bloom::color::PreparedCpuDisplayProcessorHandle& processor,
                 const Color4d color, const ImageWindow window) {
    const auto pixel = bloom::render::solidPixelFromStraightLinearRec709Scene(color);
    if (!pixel) {
        return std::nullopt;
    }
    const auto descriptor =
        Rgba32fImageDescriptor::create(window, window, bloom::core::PixelAspectRatio::square());
    if (!descriptor) {
        return std::nullopt;
    }
    auto builderResult = Rgba32fImageBuilder::create(*descriptor.value(), 1ULL << 32ULL);
    if (!builderResult) {
        return std::nullopt;
    }
    auto builder = std::move(*builderResult.value());
    std::vector<Rgba32f> row;
    row.assign(static_cast<std::size_t>(window.extent().width()), Rgba32f::transparent());
    bloom::render::fillSolidRow(row, *pixel.value());
    for (std::int64_t y = window.originY(); y < window.maxYExclusive(); ++y) {
        auto rowResult = builder.row(y);
        if (!rowResult || rowResult.value()->size() != row.size()) {
            return std::nullopt;
        }
        std::copy(row.begin(), row.end(), rowResult.value()->begin());
    }
    auto frozen = std::move(builder).freeze();
    if (!frozen) {
        return std::nullopt;
    }
    const auto view = frozen.value()->view();
    if (!view) {
        return std::nullopt;
    }
    auto displayed = bloom::color::produceBloomNeutralDisplayFrame(
        processor, *view.value(), 65536, std::numeric_limits<std::size_t>::max());
    if (!displayed) {
        return std::nullopt;
    }
    const auto pixels = displayed.value()->pixels();
    return std::vector<Rgba8>(pixels.begin(), pixels.end());
}

[[nodiscard]] bool parityHolds(const std::span<const Rgba8> gpu, const std::span<const Rgba8> cpu) {
    if (gpu.size() != cpu.size()) {
        return false;
    }
    for (std::size_t index = 0; index < gpu.size(); ++index) {
        const int dr =
            std::abs(static_cast<int>(gpu[index].red) - static_cast<int>(cpu[index].red));
        const int dg =
            std::abs(static_cast<int>(gpu[index].green) - static_cast<int>(cpu[index].green));
        const int db =
            std::abs(static_cast<int>(gpu[index].blue) - static_cast<int>(cpu[index].blue));
        if (dr > 1 || dg > 1 || db > 1 || gpu[index].alpha != cpu[index].alpha) {
            return false;
        }
    }
    return true;
}

// Produces a resident RGBA32F input through the Solid operation and returns its shared ownership.
[[nodiscard]] std::shared_ptr<const GpuImage> makeResidentInput(GpuSolid& solid,
                                                                const Color4d color,
                                                                const ImageWindow window,
                                                                Expectations& expectations) {
    const auto pixel = bloom::render::solidPixelFromStraightLinearRec709Scene(color);
    if (!pixel) {
        expectations.expect(false, "the solid primitive builds");
        return nullptr;
    }
    const auto begin = solid.begin(
        GpuSolidParameters{*pixel.value(), window, window, bloom::core::PixelAspectRatio::square()},
        1ULL << 32ULL);
    if (begin.code != GpuSolidDiagnosticCode::None) {
        expectations.expect(false, "the solid begin is accepted");
        return nullptr;
    }
    GpuSolidPollResult poll = GpuSolidPollResult::Pending;
    while (poll == GpuSolidPollResult::Pending) {
        poll = solid.poll();
    }
    if (poll != GpuSolidPollResult::Ready) {
        expectations.expect(false, "the solid job completes");
        return nullptr;
    }
    GpuImage taken = solid.takeImage();
    if (!taken.isValid()) {
        expectations.expect(false, "the solid resident image is valid");
        return nullptr;
    }
    return std::make_shared<const GpuImage>(std::move(taken));
}

void testParity(Expectations& expectations, GpuSolid& solid, GpuResidentDisplay& display,
                const bloom::color::PreparedCpuDisplayProcessorHandle& processor,
                const Color4d color, const ImageWindow window) {
    auto input = makeResidentInput(solid, color, window, expectations);
    if (input == nullptr) {
        return;
    }
    const auto begin = display.begin(input, 1ULL << 32ULL);
    expectations.expect(begin.code == GpuResidentDisplayDiagnosticCode::None,
                        "the resident display begin is accepted");
    if (begin.code != GpuResidentDisplayDiagnosticCode::None) {
        return;
    }
    GpuResidentDisplayPollResult poll = GpuResidentDisplayPollResult::Pending;
    while (poll == GpuResidentDisplayPollResult::Pending) {
        poll = display.poll();
    }
    expectations.expect(poll == GpuResidentDisplayPollResult::Ready,
                        "the resident display job completes");
    expectations.expect(display.image() != nullptr && display.image()->isValid(),
                        "a resident RGBA8 display image is published");
    expectations.expect(input->isValid(), "the input resident image is still held and valid");
    const auto readback = display.readback();
    expectations.expect(readback.hasValue(), "the resident display image reads back for parity");
    if (readback.hasValue()) {
        const auto oracle = cpuDisplayOracle(processor, color, window);
        expectations.expect(oracle.has_value(), "the CPU display oracle builds");
        if (oracle.has_value()) {
            expectations.expect(parityHolds(readback.pixels, *oracle),
                                "GPU display matches the CPU OCIO oracle (RGB<=1, alpha exact)");
        }
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

        GpuDeviceCreationOptions missing;
        missing.loader_path = "/nonexistent/bloom-missing-vulkan-loader";
        expectations.expect(!GpuDevice::create(missing),
                            "a forced missing loader yields no device");

        auto processor = buildCpuProcessor();
        expectations.expect(processor.has_value(), "the default Bloom Neutral processor builds");
        if (!processor.has_value()) {
            return 1;
        }

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

        auto solidResult = GpuSolid::create(*device.device);
        auto displayResult = GpuResidentDisplay::create(*device.device);
        expectations.expect(solidResult.hasValue() && displayResult.hasValue(),
                            "the Solid and resident-display pipelines are created");
        if (!solidResult || !displayResult) {
            std::cerr << "FAIL: pipeline creation failed\n";
            return 1;
        }
        GpuSolid& solid = *solidResult.solid;
        GpuResidentDisplay& display = *displayResult.display;
        expectations.expect(display.isBoundTo(*device.device),
                            "the display is bound to its device");

        const auto window = ImageWindow::create(-3, 5, 7, 3);
        expectations.expect(static_cast<bool>(window), "the odd/nonzero-origin window builds");
        if (!window) {
            return 1;
        }
        testParity(expectations, solid, display, *processor, Color4d{0.25, 0.5, 0.75, 1.0},
                   *window.value());
        testParity(expectations, solid, display, *processor, Color4d{-0.25, 4.0, 0.125, 0.5},
                   *window.value());

        // Subnormal input is rejected whole-frame by the shader (flag 8); nothing is published.
        {
            auto badInput = makeResidentInput(
                solid,
                Color4d{static_cast<double>(std::numeric_limits<float>::denorm_min()), 0.0, 0.0,
                        1.0},
                *window.value(), expectations);
            if (badInput != nullptr) {
                const auto begin = display.begin(badInput, 1ULL << 32ULL);
                expectations.expect(begin.code == GpuResidentDisplayDiagnosticCode::None,
                                    "the subnormal job begins");
                GpuResidentDisplayPollResult poll = GpuResidentDisplayPollResult::Pending;
                while (poll == GpuResidentDisplayPollResult::Pending) {
                    poll = display.poll();
                }
                expectations.expect(poll == GpuResidentDisplayPollResult::Failure &&
                                        display.diagnostic().code ==
                                            GpuResidentDisplayDiagnosticCode::ShaderRejected,
                                    "a subnormal input is rejected whole-frame");
                expectations.expect(display.image() == nullptr,
                                    "no display image is published on shader rejection");
            }
        }

        // Budget rejection.
        {
            auto input = makeResidentInput(solid, Color4d{0.5, 0.5, 0.5, 1.0}, *window.value(),
                                           expectations);
            if (input != nullptr) {
                const auto begin = display.begin(input, 4);
                expectations.expect(begin.code == GpuResidentDisplayDiagnosticCode::OverBudget,
                                    "an under-budget job is rejected");
            }
        }

        // Wrong-thread begin fails closed.
        {
            auto input = makeResidentInput(solid, Color4d{0.5, 0.5, 0.5, 1.0}, *window.value(),
                                           expectations);
            if (input != nullptr) {
                const auto stateBefore = display.state();
                auto observed = GpuResidentDisplayDiagnosticCode::None;
                std::thread worker([&display, &input, &observed]() {
                    observed = display.begin(input, 1ULL << 32ULL).code;
                });
                worker.join();
                expectations.expect(observed == GpuResidentDisplayDiagnosticCode::WrongThread,
                                    "begin from a joined non-owner thread is WrongThread");
                expectations.expect(display.state() == stateBefore,
                                    "a wrong-thread begin leaves the pipeline state unchanged");
            }
        }

        // Cancellation: discard is honored and no image is published.
        {
            auto input = makeResidentInput(solid, Color4d{0.5, 0.5, 0.5, 1.0}, *window.value(),
                                           expectations);
            if (input != nullptr) {
                const auto begin = display.begin(input, 1ULL << 32ULL);
                expectations.expect(begin.code == GpuResidentDisplayDiagnosticCode::None,
                                    "the cancellable begin is accepted");
                display.cancel();
                GpuResidentDisplayPollResult poll = GpuResidentDisplayPollResult::Pending;
                while (poll == GpuResidentDisplayPollResult::Pending) {
                    poll = display.poll();
                }
                expectations.expect(poll == GpuResidentDisplayPollResult::Failure &&
                                        display.diagnostic().code ==
                                            GpuResidentDisplayDiagnosticCode::Cancelled,
                                    "a cancelled job reports Cancelled");
                expectations.expect(display.image() == nullptr,
                                    "no display image is published after cancellation");
            }
        }

        return expectations.ok() ? 0 : 1;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
