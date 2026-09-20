// Bounded producer/executor precision proof for a nonlinear colour-space transform. This is the
// producer-level isolation of the production builder->executor CST failure: the exact same neutral
// config, source colour space and destination colour space are run through the real
// GpuOcioProgramPreparer + GpuOcioProgramExecutor and compared, at the strict 2e-6 absolute-or-
// relative RGBA32F gate with byte-exact alpha, against the unchanged CpuColorSpaceProcessor oracle.
//
// The neutral config's destination space is an ExponentWithLinear (sRGB) GammaOp. The pinned
// OpenColorIO CPU processor is built with OPTIMIZATION_FAST_LOG_EXP_POW, so on x86_64 the oracle's
// power is OCIO's `ssePower` minimax polynomial, not the GPU's hardware pow; the two differ by up
// to ~1.5e-5. The wrapper's CPU fast-power parity adapter must make the device reproduce the
// oracle.
//
// The fixture covers negative, HDR, translucent, alpha-zero, transfer-breakpoint endpoints, the
// exact failing builder value, and a deterministic pseudo-random grid. A missing device is an
// explicit SKIP (exit 77); --require-device turns that into a failure.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
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
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDeviceState;
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

constexpr int kSkipExit = 77;

[[nodiscard]] bool parseRequireDevice(const int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--require-device") {
            return true;
        }
    }
    return false;
}

#ifdef BLOOM_GPUSHADER_TOOLS_DIR
constexpr std::uint64_t kBudget = std::uint64_t{1} << 32;
constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 16;

// Every helper and test below drives real OCIO shader preparation from
// BLOOM_GPUSHADER_TOOLS_DIR, so this whole section is compiled only when the shader tools are
// packaged. Without them main() reports an honest SKIP (exit 77); there is no silent pass.

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

// RGB within the strict 2e-6 abs-or-rel gate; alpha byte-exact.
[[nodiscard]] std::size_t compare(Expectations& expectations, const std::vector<Rgba32f>& actual,
                                  const std::vector<Rgba32f>& expected,
                                  const std::string_view label) {
    if (actual.size() != expected.size()) {
        expectations.expect(false, std::string(label) + ": pixel count differs");
        return 1;
    }
    const auto close = [](const float a, const float b) {
        if (a == b) {
            return true;
        }
        const double absolute = std::fabs(static_cast<double>(a) - static_cast<double>(b));
        const double magnitude =
            std::max(std::fabs(static_cast<double>(a)), std::fabs(static_cast<double>(b)));
        return std::isfinite(a) && std::isfinite(b) &&
               (absolute <= 2e-6 || absolute <= 2e-6 * magnitude);
    };
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const auto& got = actual[index];
        const auto& want = expected[index];
        if (got.alpha() != want.alpha() || !close(got.red(), want.red()) ||
            !close(got.green(), want.green()) || !close(got.blue(), want.blue())) {
            if (mismatches == 0) {
                std::cerr << label << ": first mismatch at " << index << " got (" << got.red()
                          << ", " << got.green() << ", " << got.blue() << ") want (" << want.red()
                          << ", " << want.green() << ", " << want.blue() << ")\n";
            }
            ++mismatches;
        }
    }
    expectations.expect(mismatches == 0, std::string(label) +
                                             ": every pixel matches the CPU oracle at 2e-6/exact "
                                             "alpha (" +
                                             std::to_string(mismatches) + " mismatches)");
    return mismatches;
}

[[nodiscard]] std::shared_ptr<const GpuImage> uploadImage(GpuImageUpload& uploader,
                                                          const std::vector<Rgba32f>& pixels) {
    const auto windowResult = bloom::render::ImageWindow::create(0, 0, kWidth, kHeight);
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
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        const auto row = builder.value()->row(y);
        if (!row) {
            return nullptr;
        }
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            (*row.value())[x] = pixels[static_cast<std::size_t>(y) * kWidth + x];
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

// Straight (pre-premultiply) fixture. Explicit cases first, then a deterministic random grid. The
// alpha-zero case is uploaded as the canonical transparent value; the hidden-RGB passthrough is
// covered by the wrapper command test.
[[nodiscard]] std::vector<Rgba32f> fixturePixels() {
    struct Straight final {
        float r;
        float g;
        float b;
        float a;
    };
    const std::array<Straight, 18> explicitCases{{
        {0.0F, 0.0F, 0.0F, 1.0F},
        {0.0031308F, 0.0031308F, 0.0031308F, 1.0F},
        {0.0031307F, 0.0031309F, 0.00303993467F, 1.0F},
        {0.18F, 0.18F, 0.18F, 1.0F},
        {0.5F, 0.5F, 0.5F, 0.5F},
        {1.0F, 1.0F, 1.0F, 1.0F},
        // The exact production builder solid: straight {1.75, -0.25, 0.5} at alpha 0.625.
        {1.75F, -0.25F, 0.5F, 0.625F},
        {2.0F, 4.0F, 10.0F, 0.25F},
        {100.0F, 1000.0F, 0.000001F, 0.5F},
        {-1.0F, -0.25F, -0.001F, 0.75F},
        {-0.0031308F, 0.0F, 0.0031308F, 0.125F},
        {0.25F, 0.25F, 0.25F, 0.0F},
        {0.0F, 0.0F, 0.0F, 0.0F},
        {1.0F, 0.0F, 0.0F, 0.5F},
        {0.0F, 1.0F, 0.0F, 0.5F},
        {0.0F, 0.0F, 1.0F, 0.5F},
        {0.04045F, 0.04045F, 0.04045F, 1.0F},
        {0.9999999F, 1.0000001F, 0.0031308F, 0.999F},
    }};

    std::vector<Rgba32f> pixels;
    pixels.reserve(static_cast<std::size_t>(kWidth) * kHeight);
    // Fixed seed is deliberate: this is a deterministic parity fixture, not a security context, and
    // the CPU oracle must be reproducible.
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937 generator(0xB1005EEDU);
    std::uniform_real_distribution<float> rgb(-2.0F, 12.0F);
    std::uniform_real_distribution<float> alpha(0.0F, 1.0F);
    std::size_t explicitIndex = 0;
    for (std::size_t index = 0; index < static_cast<std::size_t>(kWidth) * kHeight; ++index) {
        Straight straight{};
        if (explicitIndex < explicitCases.size()) {
            straight = explicitCases[explicitIndex++];
        } else {
            straight = {rgb(generator), rgb(generator), rgb(generator), alpha(generator)};
        }
        const auto value = Rgba32f::fromPremultiplied(
            straight.r * straight.a, straight.g * straight.a, straight.b * straight.a, straight.a);
        pixels.push_back(value ? *value.value() : Rgba32f::transparent());
    }
    return pixels;
}

// The unchanged CPU image-effect oracle: un-premultiply, transform straight RGB, re-premultiply,
// keep the original alpha; an alpha-zero pixel is copied through unchanged.
[[nodiscard]] std::vector<Rgba32f> cpuOracle(const std::vector<Rgba32f>& input,
                                             const bloom::color::CpuColorSpaceProcessor& cpu) {
    std::vector<Rgba32f> expected;
    expected.reserve(input.size());
    for (const auto& pixel : input) {
        if (pixel.alpha() == 0.0F) {
            expected.push_back(pixel);
            continue;
        }
        const float alpha = pixel.alpha();
        std::array<float, 4> straight{pixel.red() / alpha, pixel.green() / alpha,
                                      pixel.blue() / alpha, 1.0F};
        std::span<std::array<float, 4>> span(&straight, 1);
        if (!cpu.apply(span)) {
            expected.push_back(Rgba32f::transparent());
            continue;
        }
        const auto value = Rgba32f::fromPremultiplied(straight[0] * alpha, straight[1] * alpha,
                                                      straight[2] * alpha, alpha);
        expected.push_back(value ? *value.value() : Rgba32f::transparent());
    }
    return expected;
}

[[nodiscard]] GpuOcioCompileOptions compileOptions() {
    GpuOcioCompileOptions options;
    options.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    options.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    return options;
}

void runPrecision(Expectations& expectations, GpuDevice& device,
                  const bloom::color::ResolvedBloomNeutralConfig& config,
                  const std::string_view from, const std::string_view to,
                  const std::string_view label, const std::vector<Rgba32f>& pixels) {
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Cst;
    spec.fromId = std::string(from);
    spec.toId = std::string(to);

    GpuOcioProgramPreparer preparer;
    const auto prepared =
        preparer.prepare(config, spec, GpuOcioCommandGeometry{kWidth, kHeight}, compileOptions());
    expectations.expect(prepared.hasValue(), std::string(label) + ": the CST command prepares");
    if (!prepared) {
        std::cerr << label << " prepare diagnostic: " << prepared.diagnostic << '\n';
        return;
    }
    expectations.expect(prepared.command->encoding() == GpuOcioOutputEncoding::FinalRgba32f,
                        std::string(label) + ": the CST command is a FinalRgba32f ProcessEffect");

    const auto cpu = bloom::color::CpuColorSpaceProcessor::prepare(config, from, to);
    expectations.expect(cpu.succeeded(), std::string(label) + ": the CPU oracle prepares");
    if (!cpu) {
        return;
    }

    auto executor = GpuOcioProgramExecutor::create(device);
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(executor.hasValue() && uploader.hasValue(),
                        std::string(label) + ": the executor and uploader host");
    if (!executor || !uploader) {
        return;
    }

    const auto input = uploadImage(*uploader.upload, pixels);
    expectations.expect(input != nullptr, std::string(label) + ": the fixture uploads");
    if (input == nullptr) {
        return;
    }
    const auto expected = cpuOracle(pixels, *cpu.processor());

    const auto dispatch = [&](const std::string_view phase) {
        const std::string stage = std::string(label) + " " + std::string(phase);
        const auto accepted = executor.executor->begin(prepared.command, input, {}, kBudget);
        expectations.expect(accepted.code == GpuOcioExecutorDiagnosticCode::None,
                            stage + ": begin accepts the command");
        if (accepted.code != GpuOcioExecutorDiagnosticCode::None) {
            return;
        }
        auto poll = executor.executor->poll();
        while (poll == GpuOcioExecutorPollResult::Pending) {
            poll = executor.executor->poll();
        }
        expectations.expect(poll == GpuOcioExecutorPollResult::Ready,
                            stage + ": the dispatch completes");
        if (poll != GpuOcioExecutorPollResult::Ready) {
            std::cerr << stage << " diagnostic: " << executor.executor->diagnostic().message
                      << '\n';
            return;
        }
        auto output = executor.executor->takeEffectOutput();
        expectations.expect(output != nullptr, stage + ": output is published");
        if (output == nullptr) {
            return;
        }
        const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
        expectations.expect(readback.hasValue(), stage + ": the output reads back");
        if (!readback) {
            return;
        }
        static_cast<void>(compare(expectations, readback.pixels, expected, stage));
    };

    dispatch("cold");
    const auto coldCounters = executor.executor->counters();
    expectations.expect(coldCounters.effectDispatches == 1,
                        std::string(label) +
                            ": the cold run dispatches the OCIO effect exactly once");
    expectations.expect(coldCounters.readbacks == 0,
                        std::string(label) + ": the executor never reads back a full frame");
    dispatch("warm");
    const auto warmCounters = executor.executor->counters();
    expectations.expect(warmCounters.programCreations == coldCounters.programCreations,
                        std::string(label) + ": a warm run creates zero new native programs");
    expectations.expect(warmCounters.programReuses == coldCounters.programReuses + 1,
                        std::string(label) + ": a warm run reuses the retained native program");
}

#endif // BLOOM_GPUSHADER_TOOLS_DIR

} // namespace

int main(int argc, char** argv) {
#ifndef BLOOM_GPUSHADER_TOOLS_DIR
    if (parseRequireDevice(argc, argv)) {
        std::cerr << "FAIL: --require-device requested but BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
        return 1;
    }
    std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
    return kSkipExit;
#else
    const bool requireDevice = parseRequireDevice(argc, argv);
    Expectations expectations;
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    auto neutral = std::move(resolution).takeResolved();
    if (!neutral.has_value()) {
        std::cerr << "FAILED: the Bloom Neutral built-in does not resolve\n";
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
    runPrecision(expectations, *device.device, *neutral, neutral->processColorSpaceId(),
                 neutral->sRgbTextureColorSpaceId(), "neutral GammaOp CST", fixturePixels());

    // A strict mixed GammaOp+other-program arm is intentionally not asserted here. The adapter is
    // verified (color unit test) to specialize only the GammaOp `pow(vec4, vec4)` and to forward
    // every other `pow` signature to the generated hardware pow, but a real mixed CST still carries
    // a residual divergence from its *other* op: an OCIO GPU matrix rounds a few ULP away from
    // OCIO's SSE matrix CPU op, and the GammaOp linear segment multiplies that difference by
    // ~12.92, so a single near-zero negative pixel can exceed the absolute 2e-6 gate even though
    // the GammaOp itself is bit-exact. That is a separate op-parity lane, not a GammaOp adapter
    // defect, and it is recorded in the work result rather than hidden by an easier fixture.

    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " OCIO precision expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: nonlinear CST GPU matches the unchanged CPU oracle at 2e-6\n";
    return 0;
#endif
}
