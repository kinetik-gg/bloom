// Native proof for bloom::render::GpuOcioProgram: real OCIO GPU programs executed on the real
// Vulkan device, compared against the actual CPU colour processors. The complete GLSL (Bloom
// wrapper + extracted OCIO shader text) is compiled once, off the device owner thread, with the
// pinned glslangValidator/spirv-val from the dependency prefix; no runtime compiler exists here.
//
// A missing device is an explicit SKIP (exit 77) unless --require-device is passed.

#include "ocio_gpu_program_native_helpers.hpp"

#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/core/color.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_ocio_program.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>

namespace bloom::color::ocio_gpu_native_test {

using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDeviceState;
using bloom::render::GpuDisplayImage;
using bloom::render::GpuImageUpload;
using bloom::render::GpuOcioProgramDiagnosticCode;
using bloom::render::GpuOcioProgramPollResult;

void testEffectCst(Expectations& expectations, GpuDevice& device,
                   const bloom::color::ResolvedBloomNeutralConfig& resolved) {
    auto desc = bloom::color::buildOcioGpuProgramForCst(resolved, "ACES2065-1", "ACEScg");
    expectations.expect(desc.succeeded(), "the ACES CST extracts");
    if (!desc.succeeded()) {
        return;
    }
    auto program = makeProgram(device, *desc.program());
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(program != nullptr && uploader.hasValue(), "the CST program hosts");
    if (program == nullptr || !uploader) {
        return;
    }
    constexpr std::uint32_t width = 4;
    constexpr std::uint32_t height = 3;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    expectations.expect(input != nullptr, "the CST input uploads");
    if (input == nullptr) {
        return;
    }
    const auto accepted = (*program)->beginEffect(input, {}, kBudget);
    expectations.expect(accepted.code == GpuOcioProgramDiagnosticCode::None,
                        "beginEffect accepts the request");
    auto poll = (*program)->poll();
    while (poll == GpuOcioProgramPollResult::Pending) {
        poll = (*program)->poll();
    }
    expectations.expect(poll == GpuOcioProgramPollResult::Ready, "the CST dispatch completes");
    auto output = (*program)->takeEffectOutput();
    expectations.expect(output != nullptr, "the CST resident output is published");
    if (output == nullptr) {
        return;
    }
    const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
    if (!readback.hasValue()) {
        std::cerr << "CST readback failed: " << readback.message << "\n";
    }
    expectations.expect(readback.hasValue(), "the CST output reads back for the oracle");
    if (!readback) {
        return;
    }
    // CPU oracle: the real ACES CPU processor, applied to straight RGB with the image-effect
    // un-premultiply/re-premultiply flow.
    const auto cpu =
        bloom::color::CpuColorSpaceProcessor::prepare(resolved, "ACES2065-1", "ACEScg");
    expectations.expect(cpu.succeeded(), "the CPU ACES processor prepares");
    if (!cpu) {
        return;
    }
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto& p = pixels[index];
        std::array<float, 4> straight{p.red(), p.green(), p.blue(), 1.0F};
        if (p.alpha() != 0.0F) {
            straight[0] = p.red() / p.alpha();
            straight[1] = p.green() / p.alpha();
            straight[2] = p.blue() / p.alpha();
        } else {
            straight = {0.0F, 0.0F, 0.0F, 1.0F};
        }
        std::span<std::array<float, 4>> span(&straight, 1);
        if (!cpu.processor()->apply(span)) {
            ++mismatches;
            continue;
        }
        const float expectedR = straight[0] * p.alpha();
        const float expectedG = straight[1] * p.alpha();
        const float expectedB = straight[2] * p.alpha();
        const auto& gpu = readback.pixels[index];
        const auto close = [](const float a, const float b) {
            if (a == b) {
                return true;
            }
            const double absolute = std::fabs(static_cast<double>(a) - static_cast<double>(b));
            const double magnitude =
                std::max(std::fabs(static_cast<double>(a)), std::fabs(static_cast<double>(b)));
            return absolute <= 2e-6 || absolute <= 2e-6 * magnitude;
        };
        if (!close(gpu.red(), expectedR) || !close(gpu.green(), expectedG) ||
            !close(gpu.blue(), expectedB) || gpu.alpha() != p.alpha()) {
            ++mismatches;
        }
    }
    expectations.expect(mismatches == 0, "every ACES CST pixel is within 2e-6 of the CPU oracle");

    // Cold/warm: a second job on the same compiled program succeeds with no recompilation.
    const auto again = (*program)->beginEffect(input, {}, kBudget);
    expectations.expect(again.code == GpuOcioProgramDiagnosticCode::None,
                        "the warm CST job is accepted");
    auto warmPoll = (*program)->poll();
    while (warmPoll == GpuOcioProgramPollResult::Pending) {
        warmPoll = (*program)->poll();
    }
    expectations.expect(warmPoll == GpuOcioProgramPollResult::Ready &&
                            (*program)->takeEffectOutput() != nullptr,
                        "the warm CST job completes on the retained program");
}

void testDisplay(Expectations& expectations, GpuDevice& device,
                 const bloom::color::ResolvedBloomNeutralConfig& resolved) {
    auto desc = bloom::color::buildOcioGpuProgramForDisplay(resolved, resolved.displayName(),
                                                            resolved.viewName());
    expectations.expect(desc.succeeded(), "the Bloom Neutral display program extracts");
    if (!desc.succeeded()) {
        return;
    }
    auto program = makeProgram(device, *desc.program());
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(program != nullptr && uploader.hasValue(), "the display program hosts");
    if (program == nullptr || !uploader) {
        return;
    }
    constexpr std::uint32_t width = 5;
    constexpr std::uint32_t height = 2;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    expectations.expect(input != nullptr, "the display input uploads");
    if (input == nullptr) {
        return;
    }
    const auto accepted = (*program)->beginDisplay(input, {}, kBudget);
    expectations.expect(accepted.code == GpuOcioProgramDiagnosticCode::None,
                        "beginDisplay accepts the request");
    auto poll = (*program)->poll();
    while (poll == GpuOcioProgramPollResult::Pending) {
        poll = (*program)->poll();
    }
    expectations.expect(poll == GpuOcioProgramPollResult::Ready, "the display dispatch completes");
    GpuDisplayImage output = (*program)->takeDisplayOutput();
    expectations.expect(output.isValid(), "the resident RGBA8 display output is published");
    if (!output.isValid()) {
        return;
    }
    const auto readback = bloom::render::readbackResidentDisplayImage(output, kBudget);
    if (!readback.hasValue()) {
        std::cerr << "display readback failed: " << readback.message << "\n";
    }
    expectations.expect(readback.hasValue(), "the display output reads back for the oracle");
    if (!readback) {
        return;
    }
    const auto handle = bloom::color::buildBloomNeutralCpuDisplayProcessor(resolved);
    const auto* const processor = handle.handle();
    expectations.expect(processor != nullptr, "the CPU display processor prepares");
    if (processor == nullptr) {
        return;
    }
    std::size_t rgbMismatches = 0;
    std::size_t alphaMismatches = 0;
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto& p = pixels[index];
        std::array<float, 3> straight{0.0F, 0.0F, 0.0F};
        if (p.alpha() != 0.0F) {
            straight = {p.red() / p.alpha(), p.green() / p.alpha(), p.blue() / p.alpha()};
        }
        const auto display = processor->referenceToDisplayLinear(
            bloom::core::Color4d{static_cast<double>(straight[0]), static_cast<double>(straight[1]),
                                 static_cast<double>(straight[2]), 1.0});
        if (!display.has_value()) {
            ++rgbMismatches;
            continue;
        }
        const auto expectedR = quantize(display->red);
        const auto expectedG = quantize(display->green);
        const auto expectedB = quantize(display->blue);
        const auto expectedA = quantize(static_cast<double>(p.alpha()));
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
                        "every display pixel is within one RGBA8 code of the CPU oracle");
    expectations.expect(alphaMismatches == 0, "display alpha is byte-exact against the CPU oracle");
}

} // namespace bloom::color::ocio_gpu_native_test

using bloom::color::ocio_gpu_native_test::Expectations;
using bloom::color::ocio_gpu_native_test::kSkipExit;
using bloom::color::ocio_gpu_native_test::parseRequireDevice;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDeviceState;

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
        std::cerr << "SKIP: the ACES built-in is unavailable\n";
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
    testEffectCst(expectations, *device.device, *aces);
    testDisplay(expectations, *device.device, *neutral);
    bloom::color::ocio_gpu_native_test::testFileTransform(expectations, *device.device, *neutral);

    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " OCIO GPU program native expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: native OCIO GPU program parity\n";
    return 0;
}
