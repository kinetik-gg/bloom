#include "ocio_gpu_program_resources_test_support.hpp"

using namespace bloom::color::ocio_resources_test;

namespace bloom::color::ocio_resources_test {

void testAcesHdrDisplayLut(Expectations& expectations, GpuDevice& device,
                           const bloom::color::ResolvedBloomNeutralConfig& aces) {
    const std::string_view display = "Rec.2100-PQ - Display";
    const std::string_view view = "ACES 1.1 - HDR Video (1000 nits & Rec.2020 lim)";
    auto desc = bloom::color::buildOcioGpuProgramForDisplay(aces, display, view);
    expectations.expect(desc.succeeded(), "the ACES HDR display LUT program extracts");
    if (!desc.succeeded()) {
        return;
    }
    bool hasLut = false;
    for (const auto& texture : desc.program()->textures) {
        hasLut = hasLut || texture.dimensions != OcioGpuTextureDimensions::ThreeD;
    }
    expectations.expect(hasLut, "the extracted display program carries a sampled LUT texture");
    auto program = makeProgram(device, *desc.program());
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(program != nullptr && uploader.hasValue(), "the LUT display program hosts");
    if (program == nullptr || !uploader) {
        return;
    }
    const std::uint64_t retained = program->retainedAllocationBytes();
    const std::uint64_t declaredLutBytes =
        desc.program()->textures.front().samples.size() * sizeof(float);
    expectations.expect(retained >= declaredLutBytes,
                        "retainedAllocationBytes charges the actual persistent LUT allocation");
    constexpr std::uint32_t width = 4;
    constexpr std::uint32_t height = 3;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    if (input == nullptr) {
        expectations.expect(false, "the LUT display input uploads");
        return;
    }
    const auto accepted = program->beginDisplay(input, {}, kBudget);
    expectations.expect(accepted.code == bloom::render::GpuOcioProgramDiagnosticCode::None,
                        "the LUT display begin is accepted");
    auto poll = program->poll();
    while (poll == bloom::render::GpuOcioProgramPollResult::Pending) {
        poll = program->poll();
    }
    expectations.expect(poll == bloom::render::GpuOcioProgramPollResult::Ready,
                        "the LUT display completes");
    GpuDisplayImage output = program->takeDisplayOutput();
    if (!output.isValid()) {
        expectations.expect(false, "the LUT display output is published");
        return;
    }
    const auto readback = bloom::render::readbackResidentDisplayImage(output, kBudget);
    if (!readback) {
        expectations.expect(false, "the LUT display output reads back");
        return;
    }
    const auto handle = bloom::color::buildCpuDisplayProcessorForView(aces, display, view);
    const auto* const processor = handle.handle();
    expectations.expect(processor != nullptr, "the ACES HDR CPU display processor prepares");
    if (processor == nullptr) {
        return;
    }
    std::size_t rgbMismatches = 0;
    std::size_t alphaMismatches = 0;
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto& p = pixels[index];
        std::array<double, 3> straight{0.0, 0.0, 0.0};
        if (p.alpha() != 0.0F) {
            const double alpha = static_cast<double>(p.alpha());
            straight = {static_cast<double>(p.red()) / alpha,
                        static_cast<double>(p.green()) / alpha,
                        static_cast<double>(p.blue()) / alpha};
        }
        const auto displayValue = processor->referenceToDisplayLinear(
            bloom::core::Color4d{straight[0], straight[1], straight[2], 1.0});
        if (!displayValue.has_value()) {
            ++rgbMismatches;
            continue;
        }
        const auto& gpu = readback.pixels[index];
        if (std::abs(static_cast<int>(gpu.red) - quantize(displayValue->red)) > 1 ||
            std::abs(static_cast<int>(gpu.green) - quantize(displayValue->green)) > 1 ||
            std::abs(static_cast<int>(gpu.blue) - quantize(displayValue->blue)) > 1) {
            ++rgbMismatches;
        }
        if (gpu.alpha != quantize(static_cast<double>(p.alpha()))) {
            ++alphaMismatches;
        }
    }
    expectations.expect(rgbMismatches == 0, "the LUT display is within one code of the CPU oracle");
    expectations.expect(alphaMismatches == 0, "the LUT display alpha is byte-exact");

    // Warm reuse: the retained texture/UBO/pipeline is reused with no re-upload.
    const auto warm = program->beginDisplay(input, {}, kBudget);
    expectations.expect(warm.code == bloom::render::GpuOcioProgramDiagnosticCode::None,
                        "the warm LUT display begin is accepted");
    poll = program->poll();
    while (poll == bloom::render::GpuOcioProgramPollResult::Pending) {
        poll = program->poll();
    }
    expectations.expect(poll == bloom::render::GpuOcioProgramPollResult::Ready &&
                            program->takeDisplayOutput().isValid(),
                        "the warm LUT display reuses the retained resources");
    expectations.expect(program->retainedAllocationBytes() == retained,
                        "retainedAllocationBytes is stable across warm jobs");
}

void testExposureContrastUniforms(Expectations& expectations, GpuDevice& device,
                                  const bloom::color::ResolvedBloomNeutralConfig& aces) {
    auto desc = bloom::color::buildOcioGpuProgramForExposureContrast(aces, "ACEScg", 0.5, 1.0);
    expectations.expect(desc.succeeded(), "the exposure/contrast program extracts");
    if (!desc.succeeded()) {
        expectations.expect(false, "the exposure/contrast program carries a non-zero UBO");
        return;
    }
    expectations.expect(desc.program()->uniformBufferSize > 0 &&
                            !desc.program()->uniformBufferData.empty() &&
                            desc.program()->uniforms.size() == 2,
                        "real OCIO uniform declarations and values are captured");
    auto program = makeProgram(device, *desc.program());
    auto uploader = GpuImageUpload::create(device);
    if (program == nullptr || !uploader) {
        expectations.expect(false, "the exposure/contrast program hosts");
        return;
    }
    constexpr std::uint32_t width = 4;
    constexpr std::uint32_t height = 2;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    if (input == nullptr) {
        expectations.expect(false, "the exposure/contrast input uploads");
        return;
    }
    const auto run = [&](std::span<const std::byte> uniformBytes) -> std::vector<Rgba32f> {
        const auto accepted = program->beginEffect(input, uniformBytes, kBudget);
        expectations.expect(accepted.code == bloom::render::GpuOcioProgramDiagnosticCode::None,
                            "the exposure/contrast begin is accepted");
        auto poll = program->poll();
        while (poll == bloom::render::GpuOcioProgramPollResult::Pending) {
            poll = program->poll();
        }
        expectations.expect(poll == bloom::render::GpuOcioProgramPollResult::Ready,
                            "the exposure/contrast dispatch completes");
        auto output = program->takeEffectOutput();
        if (output == nullptr) {
            return {};
        }
        const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
        return readback ? readback.pixels : std::vector<Rgba32f>{};
    };

    const auto snapshot = run({});
    const auto cpuHalf = exposureContrastCpu(0.5, 1.0);
    expectations.expect(cpuHalf != nullptr, "the exposure/contrast CPU oracle prepares");
    if (cpuHalf != nullptr) {
        compareEffect(
            expectations, pixels, snapshot,
            [&](std::array<float, 3>& rgb) { static_cast<void>(cpuHalf->applyRGB(rgb.data())); },
            "exposure 0.5 default snapshot");
    }

    std::array<std::byte, 8> overridden{};
    const float negativeExposure = -1.0F;
    const float contrast = 1.0F;
    std::memcpy(overridden.data(), &negativeExposure, sizeof(float));
    std::memcpy(overridden.data() + 4, &contrast, sizeof(float));
    const auto changed = run(overridden);
    const auto cpuNegative = exposureContrastCpu(-1.0, 1.0);
    if (cpuNegative != nullptr) {
        compareEffect(
            expectations, pixels, changed,
            [&](std::array<float, 3>& rgb) {
                static_cast<void>(cpuNegative->applyRGB(rgb.data()));
            },
            "exposure -1.0 uniform override");
    }
    bool differs = false;
    for (std::size_t index = 0; index < snapshot.size() && index < changed.size(); ++index) {
        differs = differs || snapshot[index].red() != changed[index].red() ||
                  snapshot[index].green() != changed[index].green() ||
                  snapshot[index].blue() != changed[index].blue();
    }
    expectations.expect(differs, "a changed uniform value changes the output without recompiling");
}

void testLut3d(Expectations& expectations, GpuDevice& device,
               const bloom::color::ResolvedBloomNeutralConfig& aces) {
    constexpr std::uint32_t edge = 3;
    std::vector<float> samples(edge * edge * edge * 3U);
    for (std::uint32_t r = 0; r < edge; ++r) {
        for (std::uint32_t g = 0; g < edge; ++g) {
            for (std::uint32_t b = 0; b < edge; ++b) {
                const std::size_t index =
                    ((static_cast<std::size_t>(r) * edge + g) * edge + b) * 3U;
                samples[index] = static_cast<float>(r) / (edge - 1);
                samples[index + 1] = static_cast<float>(g) / (edge - 1);
                samples[index + 2] = static_cast<float>(b) / (edge - 1);
            }
        }
    }
    auto desc = bloom::color::buildOcioGpuProgramForLut3d(
        aces, "ACEScg", edge, OcioGpuInterpolation::Tetrahedral, samples);
    expectations.expect(desc.succeeded(), "the in-memory 3D LUT program extracts");
    if (!desc.succeeded()) {
        return;
    }
    bool has3d = false;
    for (const auto& texture : desc.program()->textures) {
        has3d = has3d || (texture.dimensions == OcioGpuTextureDimensions::ThreeD &&
                          texture.edgeLength == edge &&
                          texture.interpolation != OcioGpuInterpolation::Unknown);
    }
    expectations.expect(has3d, "the extracted program carries the real 3D LUT and interpolation");
    auto program = makeProgram(device, *desc.program());
    auto uploader = GpuImageUpload::create(device);
    if (program == nullptr || !uploader) {
        expectations.expect(false, "the 3D LUT program hosts");
        return;
    }
    constexpr std::uint32_t width = 3;
    constexpr std::uint32_t height = 2;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    if (input == nullptr) {
        expectations.expect(false, "the 3D LUT input uploads");
        return;
    }
    const auto accepted = program->beginEffect(input, {}, kBudget);
    expectations.expect(accepted.code == bloom::render::GpuOcioProgramDiagnosticCode::None,
                        "the 3D LUT begin is accepted");
    auto poll = program->poll();
    while (poll == bloom::render::GpuOcioProgramPollResult::Pending) {
        poll = program->poll();
    }
    expectations.expect(poll == bloom::render::GpuOcioProgramPollResult::Ready,
                        "the 3D LUT dispatch completes");
    auto output = program->takeEffectOutput();
    if (output == nullptr) {
        expectations.expect(false, "the 3D LUT output is published");
        return;
    }
    const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
    if (!readback) {
        expectations.expect(false, "the 3D LUT output reads back");
        return;
    }
    auto config = OCIO::Config::CreateFromBuiltinConfig("cg-config-v1.0.0_aces-v1.3_ocio-v2.1");
    auto transform = OCIO::Lut3DTransform::Create();
    transform->setGridSize(edge);
    transform->setInterpolation(OCIO::INTERP_TETRAHEDRAL);
    for (std::uint32_t r = 0; r < edge; ++r) {
        for (std::uint32_t g = 0; g < edge; ++g) {
            for (std::uint32_t b = 0; b < edge; ++b) {
                const std::size_t index =
                    ((static_cast<std::size_t>(r) * edge + g) * edge + b) * 3U;
                transform->setValue(r, g, b, samples[index], samples[index + 1],
                                    samples[index + 2]);
            }
        }
    }
    auto cpu = config->getProcessor(OCIO::Context::Create(), transform, OCIO::TRANSFORM_DIR_FORWARD)
                   ->getDefaultCPUProcessor();
    compareEffect(
        expectations, pixels, readback.pixels,
        [&](std::array<float, 3>& rgb) { static_cast<void>(cpu->applyRGB(rgb.data())); }, "3D LUT");
}

} // namespace bloom::color::ocio_resources_test

int main(int argc, char** argv) {
    const bool requireDevice = parseRequireDevice(argc, argv);
    Expectations expectations;
    testDispatchPlan(expectations);
    testShaderInterfaceMalformed(expectations);
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
    testAcesHdrDisplayLut(expectations, *device.device, *aces);
    testExposureContrastUniforms(expectations, *device.device, *aces);
    testLut3d(expectations, *device.device, *aces);
    testForcedDispatchPlanning(expectations, *device.device, *aces);
    testDispatchRefusal(expectations, *device.device, *aces);
    testShaderInterfaceReflection(expectations, *device.device, *aces);

    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " resource native expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: native OCIO descriptor resources\n";
    return 0;
}
