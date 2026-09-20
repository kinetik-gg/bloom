// Native proof for the per-op CPU-math parity specialization on real mixed OCIO programs:
//
//   * ACEScg -> sRGB - Texture   : a MatrixOp followed by a GammaOp. OCIO's GPU `mat4 * vec4`
//     rounds differently from its SSE matrix CPU op, and the GammaOp linear segment multiplies that
//     difference by ~12.92; the matrix region is specialized to OCIO's SSE add order.
//   * ACEScct -> sRGB - Texture  : a LogOp followed by a MatrixOp and a GammaOp. OCIO's LogOp CPU
//     renderer decomposes `pow(base, x)` into `sseExp2(x * log2(base))`; the Log region is
//     specialized to that decomposition.
//
// Both are compared against the unchanged CpuColorSpaceProcessor oracle at the strict 2e-6
// absolute-or-relative RGBA32F gate with byte-exact alpha, over a fixture with cancelling
// negative/HDR values, transfer breakpoints, alpha-zero, and a deterministic grid, cold and warm.
//
// A missing device is an explicit SKIP (exit 77); --require-device turns that into a failure.

#include "ocio_gpu_program_native_helpers.hpp"

#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/color/ocio_gpu_program.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using bloom::color::ocio_gpu_native_test::compileGlsl;
using bloom::color::ocio_gpu_native_test::Expectations;
using bloom::color::ocio_gpu_native_test::kBudget;
using bloom::color::ocio_gpu_native_test::kSkipExit;
using bloom::color::ocio_gpu_native_test::makeProgram;
using bloom::color::ocio_gpu_native_test::parseRequireDevice;
using bloom::color::ocio_gpu_native_test::uploadImage;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDeviceState;
using bloom::render::GpuImageUpload;
using bloom::render::GpuOcioProgramPollResult;
using bloom::render::Rgba32f;

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 16;

// Straight (pre-premultiply) fixture. Large cancelling opposite-sign values stress the matrix add
// order; the small negative values hit the GammaOp linear segment that amplified the matrix
// rounding in the original failure; the HDR and breakpoint values cover the transfer edges.
// `magnitude` bounds the log-encoded source (an ACEScct anti-log overflows binary32 above ~1.5).
[[nodiscard]] std::vector<Rgba32f> mixedFixture(const float magnitude) {
    std::vector<Rgba32f> pixels;
    pixels.reserve(static_cast<std::size_t>(kWidth) * kHeight);
    std::mt19937 generator(0x1CEB00DAU);
    std::uniform_real_distribution<float> rgb(-magnitude, magnitude);
    std::uniform_real_distribution<float> alpha(0.0F, 1.0F);
    const float cancelling = 0.9F * magnitude;
    const std::array<std::array<float, 4>, 14> explicitCases{{
        {0.0F, 0.0F, 0.0F, 1.0F},
        {0.0031308F, 0.0031308F, 0.0031308F, 1.0F},
        {0.00303993467F, 0.00303993467F, 0.00303993467F, 1.0F},
        {0.18F, 0.18F, 0.18F, 1.0F},
        {0.5F, 0.5F, 0.5F, 0.625F},
        {1.0F, 1.0F, 1.0F, 1.0F},
        {1.75F, -0.25F, 0.5F, 0.625F},
        {2.0F, 2.0F, 2.0F, 0.25F},
        {-0.25F, 0.25F, -0.001F, 0.75F},
        // Cancelling pairs: the intermediate `col0*x + col1*y` cancels, so the SSE add order shows.
        {cancelling, -cancelling, cancelling, 1.0F},
        {-cancelling, cancelling, -cancelling, 1.0F},
        {0.5F * cancelling, -0.5F * cancelling, 0.0031308F, 0.5F},
        {0.0F, 0.0F, 0.0F, 0.0F},
        {1.0F, 0.0F, -1.0F, 0.999F},
    }};
    std::size_t explicitIndex = 0;
    for (std::size_t index = 0; index < static_cast<std::size_t>(kWidth) * kHeight; ++index) {
        std::array<float, 4> straight{};
        if (explicitIndex < explicitCases.size()) {
            straight = explicitCases[explicitIndex++];
        } else {
            straight = {rgb(generator), rgb(generator), rgb(generator), alpha(generator)};
        }
        const auto value =
            Rgba32f::fromPremultiplied(straight[0] * straight[3], straight[1] * straight[3],
                                       straight[2] * straight[3], straight[3]);
        pixels.push_back(value ? *value.value() : Rgba32f::transparent());
    }
    return pixels;
}

[[nodiscard]] bool close(const float actual, const float expected) {
    if (actual == expected) {
        return true;
    }
    if (!std::isfinite(actual) || !std::isfinite(expected)) {
        return false;
    }
    const double absolute = std::fabs(static_cast<double>(actual) - static_cast<double>(expected));
    const double magnitude =
        std::max(std::fabs(static_cast<double>(actual)), std::fabs(static_cast<double>(expected)));
    return absolute <= 2e-6 || absolute <= 2e-6 * magnitude;
}

// The unchanged CPU image-effect oracle.
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

void runDivisionProbe(Expectations& expectations, GpuDevice& device) {
    // Device-side isolation of the shared division helper: upload (a, b) pairs and a custom wrapper
    // that writes `bloom_ocio_cpu_div(a, b)` (red) and the device's raw `a / b` (green), then
    // compare both against the unchanged CPU's correctly-rounded scalar `a / b`.
    std::vector<Rgba32f> pixels;
    std::vector<std::pair<float, float>> pairs;
    pixels.reserve(static_cast<std::size_t>(kWidth) * kHeight);
    pairs.reserve(pixels.capacity());
    std::mt19937 generator(0xD1D1DE99U);
    std::uniform_real_distribution<float> numerators(-2.0F, 2.0F);
    std::uniform_real_distribution<float> denominators(0.001F, 1.0F);
    for (std::size_t index = 0; index < static_cast<std::size_t>(kWidth) * kHeight; ++index) {
        float numerator = numerators(generator);
        float denominator = denominators(generator);
        if (index == 0) {
            numerator = 0.0885742828F;
            denominator = 0.712358594F;
        } else if (index == 1) {
            numerator = 0.591227174F;
            denominator = 0.712358594F;
        } else if (index == 2) {
            numerator = 0.460722327F;
            denominator = 0.712358594F;
        } else if (index == 3) {
            // A tiny positive alpha.
            numerator = 0.5F;
            denominator = 1.0e-30F;
        }
        pairs.emplace_back(numerator, denominator);
        const auto value = Rgba32f::fromPremultiplied(numerator, 0.0F, 0.0F, denominator);
        pixels.push_back(value ? *value.value() : Rgba32f::transparent());
    }
    std::string glsl = "#version 460\n";
    glsl += "layout(local_size_x = 64) in;\n";
    glsl += "layout(set = 1, binding = 0, rgba32f) uniform readonly image2D bloom_ocio_input;\n";
    glsl += "layout(set = 1, binding = 1, rgba32f) uniform writeonly image2D bloom_ocio_output;\n";
    glsl += "layout(set = 1, binding = 2, std430) buffer BloomOcioStatus { uint flags[]; } "
            "bloom_ocio_status;\n";
    glsl += "layout(push_constant) uniform BloomOcioPush { uint pixelCount; uint width; uint "
            "height; } bloom_ocio_push;\n";
    glsl += std::string(bloom::color::ocioGpuPreciseDivisionGlsl());
    glsl += "void main() {\n";
    glsl += "  uint index = gl_GlobalInvocationID.x;\n";
    glsl += "  if (index >= bloom_ocio_push.pixelCount) { return; }\n";
    glsl += "  ivec2 c = ivec2(int(index % bloom_ocio_push.width), int(index / "
            "bloom_ocio_push.width));\n";
    glsl += "  vec4 p = imageLoad(bloom_ocio_input, c);\n";
    glsl += "  imageStore(bloom_ocio_output, c, vec4(bloom_ocio_cpu_div(p.r, p.a), p.r / p.a, p.r, "
            "p.a));\n";
    glsl += "}\n";

    const auto spirv = compileGlsl(glsl);
    expectations.expect(spirv.has_value(), "division probe: the wrapper compiles");
    if (!spirv) {
        return;
    }
    bloom::render::OcioGpuProgramDesc desc;
    desc.stage = bloom::render::OcioGpuProgramStage::ProcessEffect;
    desc.functionName = "bloom_ocio_probe";
    desc.semanticsId = "bloom.test.division-probe";
    desc.shaderText = "// division probe body replaced by the custom wrapper\n";
    auto created = bloom::render::GpuOcioProgram::create(device, std::move(desc), *spirv, {});
    expectations.expect(created.hasValue(), "division probe: the program hosts");
    if (!created) {
        std::cerr << "division probe diagnostic: " << created.diagnostic.message << '\n';
        return;
    }
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(uploader.hasValue(), "division probe: the uploader hosts");
    if (!uploader) {
        return;
    }
    const auto input = uploadImage(*uploader.upload, kWidth, kHeight, pixels);
    expectations.expect(input != nullptr, "division probe: the input uploads");
    if (input == nullptr) {
        return;
    }
    const auto accepted = created.program->beginEffect(input, {}, kBudget);
    expectations.expect(accepted.code == bloom::render::GpuOcioProgramDiagnosticCode::None,
                        "division probe: beginEffect accepts");
    if (accepted.code != bloom::render::GpuOcioProgramDiagnosticCode::None) {
        return;
    }
    auto poll = created.program->poll();
    while (poll == GpuOcioProgramPollResult::Pending) {
        poll = created.program->poll();
    }
    expectations.expect(poll == GpuOcioProgramPollResult::Ready, "division probe: dispatch runs");
    if (poll != GpuOcioProgramPollResult::Ready) {
        return;
    }
    auto output = created.program->takeEffectOutput();
    expectations.expect(output != nullptr, "division probe: output published");
    if (output == nullptr) {
        return;
    }
    const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
    expectations.expect(readback.hasValue(), "division probe: output reads back");
    if (!readback) {
        return;
    }
    std::size_t helperMismatches = 0;
    std::size_t rawMismatches = 0;
    for (std::size_t index = 0; index < pairs.size(); ++index) {
        const float cpu = pairs[index].first / pairs[index].second;
        if (readback.pixels[index].red() != cpu) {
            if (helperMismatches < 3) {
                std::cerr << "division helper differs @ " << index << " a=" << pairs[index].first
                          << " b=" << pairs[index].second << " gpu=" << readback.pixels[index].red()
                          << " cpu=" << cpu << '\n';
            }
            ++helperMismatches;
        }
        if (readback.pixels[index].green() != cpu) {
            ++rawMismatches;
        }
    }
    expectations.expect(helperMismatches == 0,
                        "the shared division helper matches CPU scalar division exactly (" +
                            std::to_string(helperMismatches) + " differ)");
    // The device's raw `a / b` is NOT required to be correctly rounded; the probe records how many
    // cases it gets wrong so the shared helper's correction is visible in the log.
    static_cast<void>(rawMismatches);
}

void runMixed(Expectations& expectations, GpuDevice& device,
              const bloom::color::ResolvedBloomNeutralConfig& config, const std::string_view from,
              const std::string_view to, const std::string_view label, const float magnitude) {
    const auto built = bloom::color::buildOcioGpuProgramForCst(config, from, to);
    expectations.expect(built.succeeded(), std::string(label) + ": the mixed CST extracts");
    if (!built.succeeded()) {
        return;
    }
    const auto* const desc = built.program();
    expectations.expect(desc != nullptr, std::string(label) + ": the program exists");
    if (desc == nullptr) {
        return;
    }
    // The specialization must be a body override, never an in-place rewrite of the descriptor.
    const std::string originalBody = desc->shaderText;
    const auto adapter = bloom::color::ocioGpuSamplingGlslFor(*desc);
    expectations.expect(!adapter.shaderBody.empty(),
                        std::string(label) + ": a per-op specialized body is emitted");
    expectations.expect(desc->shaderText == originalBody,
                        std::string(label) + ": the extracted descriptor is never modified");
    if (adapter.shaderBody.empty()) {
        return;
    }

    auto program = makeProgram(device, *desc);
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(program != nullptr && uploader.hasValue(),
                        std::string(label) + ": the program hosts");
    if (program == nullptr || !uploader) {
        return;
    }

    const std::vector<Rgba32f> pixels = mixedFixture(magnitude);
    const auto input = uploadImage(*uploader.upload, kWidth, kHeight, pixels);
    expectations.expect(input != nullptr, std::string(label) + ": the fixture uploads");
    if (input == nullptr) {
        return;
    }

    const auto cpu = bloom::color::CpuColorSpaceProcessor::prepare(config, from, to);
    expectations.expect(cpu.succeeded(), std::string(label) + ": the CPU oracle prepares");
    if (!cpu) {
        return;
    }
    const auto expected = cpuOracle(pixels, *cpu.processor());

    const auto dispatch = [&](const std::string_view phase) {
        const std::string stage = std::string(label) + " " + std::string(phase);
        const auto accepted = (*program)->beginEffect(input, {}, kBudget);
        expectations.expect(accepted.code == bloom::render::GpuOcioProgramDiagnosticCode::None,
                            stage + ": beginEffect accepts the request");
        if (accepted.code != bloom::render::GpuOcioProgramDiagnosticCode::None) {
            return;
        }
        auto poll = (*program)->poll();
        while (poll == GpuOcioProgramPollResult::Pending) {
            poll = (*program)->poll();
        }
        expectations.expect(poll == GpuOcioProgramPollResult::Ready, stage + ": the dispatch runs");
        if (poll != GpuOcioProgramPollResult::Ready) {
            return;
        }
        auto output = (*program)->takeEffectOutput();
        expectations.expect(output != nullptr, stage + ": the resident output is published");
        if (output == nullptr) {
            return;
        }
        const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
        expectations.expect(readback.hasValue(), stage + ": the output reads back");
        if (!readback) {
            return;
        }
        std::size_t mismatches = 0;
        for (std::size_t index = 0; index < readback.pixels.size(); ++index) {
            const auto& got = readback.pixels[index];
            const auto& want = expected[index];
            if (got.alpha() != want.alpha() || !close(got.red(), want.red()) ||
                !close(got.green(), want.green()) || !close(got.blue(), want.blue())) {
                if (mismatches == 0) {
                    const auto& src = pixels[index];
                    std::cerr << std::setprecision(9) << stage << ": first mismatch at " << index
                              << " a=" << got.alpha() << " got (" << got.red() << ", "
                              << got.green() << ", " << got.blue() << ") want (" << want.red()
                              << ", " << want.green() << ", " << want.blue() << ") src ("
                              << src.red() << ", " << src.green() << ", " << src.blue() << ", "
                              << src.alpha() << ")\n";
                }
                ++mismatches;
            }
        }
        expectations.expect(mismatches == 0,
                            stage + ": every pixel matches the CPU oracle at 2e-6/exact alpha (" +
                                std::to_string(mismatches) + " mismatches)");
    };

    dispatch("cold");
    dispatch("warm");
}

} // namespace

int main(int argc, char** argv) {
    const bool requireDevice = parseRequireDevice(argc, argv);
    Expectations expectations;

    auto neutralResolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    auto neutral = std::move(neutralResolution).takeResolved();
    if (!neutral.has_value()) {
        std::cerr << "FAILED: the Bloom Neutral built-in does not resolve\n";
        return 1;
    }
    const auto acesRevision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    if (!acesRevision.has_value()) {
        std::cout << "SKIP: the ACES built-in is unavailable\n";
        return kSkipExit;
    }
    auto acesResolution =
        bloom::color::resolveOcioBuiltIn(bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                                         bloom::color::kAcesCgV1ConfigUri, *acesRevision, "ACEScg");
    auto aces = std::move(acesResolution).takeResolved();
    if (!aces.has_value()) {
        std::cerr << "FAILED: the ACES built-in does not resolve\n";
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
    runDivisionProbe(expectations, *device.device);
    runMixed(expectations, *device.device, *aces, "ACEScg", "sRGB - Texture",
             "mixed GammaOp+Matrix CST", 100.0F);
    runMixed(expectations, *device.device, *aces, "ACEScct", "sRGB - Texture",
             "mixed GammaOp+LogOp+Matrix CST", 1.0F);

    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " mixed OCIO precision expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: mixed OCIO programs match the unchanged CPU oracle at 2e-6\n";
    return 0;
}
