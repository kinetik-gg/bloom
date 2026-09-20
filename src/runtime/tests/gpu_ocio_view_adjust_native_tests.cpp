// Native acceptance for the exact runtime::ViewAdjust post-display exposure/gamma in the production
// OCIO DisplayPacking wrapper. Real display programs are extracted for at least two display/view
// pairs, wrapped with a non-neutral adjustment (and a neutral control), compiled off-device, and
// executed on the device owner thread. The unchanged CPU reference is the oracle: the CPU display
// processor's unclamped display RGB, then ViewAdjust::fromEncoded, then ViewAdjust::quantize.
//
// The fixture covers negative, HDR, alpha-zero, and translucent pixels. Display RGBA8 must match
// within one code with exact alpha. A changed adjustment changes the display command identity
// without touching the process (CST) command, and a warm identical command reuses the retained
// native program. No standalone shader-only proof: the real executor runs on a device.
//
// A missing device is an explicit SKIP unless --require-device is passed.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_ocio_program_executor.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bloom::core::Color4d;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDeviceState;
using bloom::render::GpuDisplayImage;
using bloom::render::GpuImage;
using bloom::render::GpuImageUpload;
using bloom::render::GpuImageUploadDiagnosticCode;
using bloom::render::GpuImageUploadPollResult;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::runtime::GpuOcioCommandGeometry;
using bloom::runtime::GpuOcioCompileOptions;
using bloom::runtime::GpuOcioExecutorDiagnosticCode;
using bloom::runtime::GpuOcioExecutorPollResult;
using bloom::runtime::GpuOcioOutputEncoding;
using bloom::runtime::GpuOcioProgramExecutor;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuOcioTransformKind;
using bloom::runtime::GpuOcioTransformSpec;
using bloom::runtime::ViewAdjust;

constexpr std::uint64_t kBudget = std::uint64_t{1} << 32;
constexpr int kSkipExit = 77;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] bool parseRequireDevice(const int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--require-device") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] GpuOcioCompileOptions compileOptions() {
    GpuOcioCompileOptions options;
    options.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    options.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    return options;
}

[[nodiscard]] std::shared_ptr<const GpuImage> uploadImage(GpuImageUpload& uploader,
                                                          const std::uint32_t width,
                                                          const std::uint32_t height,
                                                          const std::vector<Rgba32f>& pixels) {
    const auto windowResult = bloom::render::ImageWindow::create(0, 0, width, height);
    if (!windowResult) {
        return nullptr;
    }
    const auto descriptor = Rgba32fImageDescriptor::create(
        *windowResult.value(), *windowResult.value(), bloom::core::PixelAspectRatio::square());
    if (!descriptor || descriptor.value()->layout().pixelCount != pixels.size()) {
        return nullptr;
    }
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), kBudget);
    if (!builder) {
        return nullptr;
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto row = builder.value()->row(y);
        if (!row) {
            return nullptr;
        }
        for (std::uint32_t x = 0; x < width; ++x) {
            (*row.value())[x] = pixels[static_cast<std::size_t>(y) * width + x];
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen) {
        return nullptr;
    }
    auto source = std::make_shared<const Rgba32fImage>(std::move(*frozen.value()));
    if (uploader.begin({source}, kBudget).code != GpuImageUploadDiagnosticCode::None) {
        return nullptr;
    }
    auto poll = GpuImageUploadPollResult::Pending;
    while (poll == GpuImageUploadPollResult::Pending) {
        poll = uploader.poll();
    }
    if (poll != GpuImageUploadPollResult::Ready) {
        return nullptr;
    }
    return std::make_shared<GpuImage>(uploader.takeImage());
}

// Premultiplied fixture: HDR, a negative channel, translucent alpha, and exact zero-alpha pixels.
[[nodiscard]] std::vector<Rgba32f> fixturePixels(const std::uint32_t width,
                                                 const std::uint32_t height) {
    std::vector<Rgba32f> pixels(static_cast<std::size_t>(width) * height, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(width);
            const float fy = static_cast<float>(y) / static_cast<float>(height);
            const float alpha = ((x + y) % 3 == 0) ? 0.0F : 0.25F + 0.5F * fx;
            const auto value = Rgba32f::fromPremultiplied((1.6F + fx) * alpha, (-0.2F + fy) * alpha,
                                                          (0.125F + 2.0F * fx) * alpha, alpha);
            if (value) {
                pixels[static_cast<std::size_t>(y) * width + x] = *value.value();
            }
        }
    }
    return pixels;
}

struct Pair final {
    std::string display;
    std::string view;
};

struct AdjustCase final {
    const char* name;
    ViewAdjust adjust;
};

constexpr std::array<AdjustCase, 6> kAdjustCases{{
    {"neutral", ViewAdjust{}},
    {"exposure-negative", ViewAdjust{.exposure = -1.0, .gamma = 1.0}},
    {"exposure-hdr", ViewAdjust{.exposure = 2.0, .gamma = 1.0}},
    {"gamma-lift", ViewAdjust{.exposure = 0.0, .gamma = 2.2}},
    {"gamma-crush", ViewAdjust{.exposure = 0.0, .gamma = 0.5}},
    {"exposure-and-gamma", ViewAdjust{.exposure = 1.0, .gamma = 0.8}},
}};

void testPair(Expectations& expectations, GpuDevice& device, GpuOcioProgramPreparer& preparer,
              const bloom::color::ResolvedBloomNeutralConfig& resolved, const Pair& pair,
              const GpuOcioCompileOptions& options) {
    auto cpuResult =
        bloom::color::buildCpuDisplayProcessorForView(resolved, pair.display, pair.view);
    const auto* const processor = cpuResult.handle();
    expectations.expect(processor != nullptr,
                        "the CPU display oracle prepares for " + pair.display + " / " + pair.view);
    if (processor == nullptr) {
        return;
    }
    auto executor = GpuOcioProgramExecutor::create(device);
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(executor.hasValue() && uploader.hasValue(),
                        "the ViewAdjust executor and uploader host");
    if (!executor || !uploader) {
        return;
    }
    constexpr std::uint32_t width = 5;
    constexpr std::uint32_t height = 3;
    const GpuOcioCommandGeometry geometry{width, height};
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    expectations.expect(input != nullptr, "the ViewAdjust input uploads");
    if (input == nullptr) {
        return;
    }

    std::vector<bloom::core::Sha256Digest> identities;
    for (const auto& adjustCase : kAdjustCases) {
        GpuOcioTransformSpec spec;
        spec.kind = GpuOcioTransformKind::Display;
        spec.display = pair.display;
        spec.view = pair.view;
        spec.viewAdjust = adjustCase.adjust;
        const auto prepared = preparer.prepare(resolved, spec, geometry, options);
        expectations.expect(prepared.hasValue(),
                            std::string("the display command prepares: ") + adjustCase.name);
        if (!prepared) {
            continue;
        }
        expectations.expect(prepared.command->encoding() == GpuOcioOutputEncoding::DisplayRgba8,
                            "the display command is DisplayRgba8");
        expectations.expect(prepared.command->viewAdjust() == adjustCase.adjust,
                            "the command exposes its exact bound adjustment");
        identities.push_back(prepared.command->identity());

        const auto accepted = executor.executor->begin(prepared.command, input, {}, kBudget);
        expectations.expect(accepted.code == GpuOcioExecutorDiagnosticCode::None,
                            std::string("begin accepts: ") + adjustCase.name);
        auto poll = executor.executor->poll();
        while (poll == GpuOcioExecutorPollResult::Pending) {
            poll = executor.executor->poll();
        }
        expectations.expect(poll == GpuOcioExecutorPollResult::Ready,
                            std::string("the display dispatch completes: ") + adjustCase.name);
        auto output = executor.executor->takeDisplayOutput();
        expectations.expect(output.has_value() && output->isValid(),
                            std::string("the display output publishes: ") + adjustCase.name);
        if (!output.has_value() || !output->isValid()) {
            continue;
        }
        const auto readback = bloom::render::readbackResidentDisplayImage(*output, kBudget);
        expectations.expect(readback.hasValue(),
                            std::string("the display output reads back: ") + adjustCase.name);
        if (!readback) {
            continue;
        }
        std::size_t rgbMismatches = 0;
        std::size_t alphaMismatches = 0;
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            const auto& p = pixels[index];
            std::array<double, 3> straight{0.0, 0.0, 0.0};
            if (p.alpha() != 0.0F) {
                straight = {static_cast<double>(p.red() / p.alpha()),
                            static_cast<double>(p.green() / p.alpha()),
                            static_cast<double>(p.blue() / p.alpha())};
            }
            const auto display = processor->referenceToDisplayLinear(
                Color4d{straight[0], straight[1], straight[2], 1.0});
            if (!display.has_value()) {
                ++rgbMismatches;
                continue;
            }
            const auto expectedR =
                ViewAdjust::quantize(adjustCase.adjust.fromEncoded(display->red));
            const auto expectedG =
                ViewAdjust::quantize(adjustCase.adjust.fromEncoded(display->green));
            const auto expectedB =
                ViewAdjust::quantize(adjustCase.adjust.fromEncoded(display->blue));
            const auto expectedA = ViewAdjust::quantize(static_cast<double>(p.alpha()));
            const auto& gpu = readback.pixels[index];
            if (std::abs(static_cast<int>(gpu.red) - expectedR) > 1 ||
                std::abs(static_cast<int>(gpu.green) - expectedG) > 1 ||
                std::abs(static_cast<int>(gpu.blue) - expectedB) > 1) {
                ++rgbMismatches;
            }
            if (gpu.alpha != expectedA) {
                ++alphaMismatches;
            }
        }
        expectations.expect(rgbMismatches == 0,
                            std::string("every adjusted display pixel is within one RGBA8 code: ") +
                                adjustCase.name);
        expectations.expect(alphaMismatches == 0,
                            std::string("adjusted display alpha is exact: ") + adjustCase.name);

        // Warm identical command: the retained native program is reused, not recreated.
        const auto before = executor.executor->counters();
        const auto warm = executor.executor->begin(prepared.command, input, {}, kBudget);
        expectations.expect(warm.code == GpuOcioExecutorDiagnosticCode::None,
                            "the warm adjusted job is accepted");
        auto warmPoll = executor.executor->poll();
        while (warmPoll == GpuOcioExecutorPollResult::Pending) {
            warmPoll = executor.executor->poll();
        }
        expectations.expect(warmPoll == GpuOcioExecutorPollResult::Ready &&
                                executor.executor->takeDisplayOutput().has_value(),
                            "the warm adjusted job completes");
        const auto after = executor.executor->counters();
        expectations.expect(after.programCreations == before.programCreations &&
                                after.programReuses == before.programReuses + 1,
                            "a warm adjusted job reuses the retained native program");
    }
    // A changed adjustment is a different display command identity.
    for (std::size_t i = 1; i < identities.size(); ++i) {
        expectations.expect(identities[i] != identities[0],
                            "a changed adjustment changes the display command identity");
    }
}

} // namespace

int main(int argc, char** argv) {
    const bool requireDevice = parseRequireDevice(argc, argv);
    Expectations expectations;
#ifndef BLOOM_GPUSHADER_TOOLS_DIR
    std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
    return kSkipExit;
#else
    const auto revision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    if (!revision.has_value()) {
        std::cout << "SKIP: the ACES built-in is unavailable\n";
        return kSkipExit;
    }
    auto acesResolution =
        bloom::color::resolveOcioBuiltIn(bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                                         bloom::color::kAcesCgV1ConfigUri, *revision, "ACEScg");
    auto aces = std::move(acesResolution).takeResolved();
    if (!aces.has_value()) {
        std::cerr << "FAILED: the ACES built-in does not resolve\n";
        return 1;
    }
    // At least two real display/view pairs with a CPU display processor.
    std::vector<Pair> pairs;
    for (const auto& entry : aces->displays()) {
        auto handle =
            bloom::color::buildCpuDisplayProcessorForView(*aces, entry.display, entry.view);
        if (handle.handle() != nullptr) {
            pairs.push_back(Pair{std::string(entry.display), std::string(entry.view)});
        }
        if (pairs.size() >= 2) {
            break;
        }
    }
    expectations.expect(pairs.size() >= 2, "at least two display/view pairs are available");
    if (pairs.size() < 2) {
        return 1;
    }
    GpuDeviceCreationOptions options;
    auto device = GpuDevice::create(options);
    if (!device) {
        if (requireDevice) {
            std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message << '\n';
            return 1;
        }
        std::cout << "SKIP: no compatible Vulkan device available: " << device.diagnostic.message
                  << '\n';
        return kSkipExit;
    }
    expectations.expect(device.device->state() == GpuDeviceState::Ready, "the device is Ready");
    GpuOcioProgramPreparer preparer;
    const auto compile = compileOptions();

    // A process (CST) command is unaffected by any display adjustment.
    GpuOcioTransformSpec cst;
    cst.kind = GpuOcioTransformKind::Cst;
    cst.fromId = "ACES2065-1";
    cst.toId = "ACEScg";
    constexpr GpuOcioCommandGeometry cstGeometry{4, 3};
    const auto cstBefore = preparer.prepare(*aces, cst, cstGeometry, compile);
    expectations.expect(cstBefore.hasValue(), "the process CST command prepares");

    for (const auto& pair : pairs) {
        testPair(expectations, *device.device, preparer, *aces, pair, compile);
    }

    const auto cstAfter = preparer.prepare(*aces, cst, cstGeometry, compile);
    expectations.expect(cstBefore.hasValue() && cstAfter.hasValue() &&
                            cstBefore.command == cstAfter.command,
                        "display adjustments never re-evaluate or re-key the process command");

    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " OCIO ViewAdjust expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: runtime OCIO ViewAdjust native acceptance\n";
    return 0;
#endif
}
