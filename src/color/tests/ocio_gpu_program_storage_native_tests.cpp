#include "ocio_gpu_program_resources_test_support.hpp"

#include "ocio_gpu_program_extract.hpp"
#include "ocio_gpu_program_fault.hpp"

#include <limits>

namespace bloom::color::ocio_resources_test {

void testMultiLutPrefix(Expectations& expectations, const GpuDeviceCreationOptions& options) {
    // Two independently extracted nonlinear 3D operations. Different grids and an intervening
    // matrix keep the actual OCIO program's texture array observable; no descriptor duplication.
    auto group = OCIO::GroupTransform::Create();
    for (unsigned operation = 0; operation < 2; ++operation) {
        const unsigned edge = operation == 0 ? 3U : 5U;
        auto lut = OCIO::Lut3DTransform::Create();
        lut->setGridSize(edge);
        lut->setInterpolation(OCIO::INTERP_TETRAHEDRAL);
        for (unsigned r = 0; r < edge; ++r) {
            for (unsigned g = 0; g < edge; ++g) {
                for (unsigned b = 0; b < edge; ++b) {
                    const float red = static_cast<float>(r) / static_cast<float>(edge - 1);
                    const float green = static_cast<float>(g) / static_cast<float>(edge - 1);
                    const float blue = static_cast<float>(b) / static_cast<float>(edge - 1);
                    lut->setValue(r, g, b, red * red, green * (0.8F + red * 0.2F),
                                  blue * (0.7F + green * 0.3F));
                }
            }
        }
        group->appendTransform(lut);
        if (operation == 0) {
            auto matrix = OCIO::MatrixTransform::Create();
            const double coefficients[16]{0.8, 0.1, 0.0, 0.0, 0.0, 0.9, 0.1, 0.0,
                                          0.1, 0.0, 0.8, 0.0, 0.0, 0.0, 0.0, 1.0};
            matrix->setMatrix(coefficients);
            group->appendTransform(matrix);
        }
    }
    auto config = OCIO::Config::CreateRaw();
    auto processor =
        config->getProcessor(OCIO::Context::Create(), group, OCIO::TRANSFORM_DIR_FORWARD);
    auto reflected =
        detail::extractOcioGpuProgram(processor, bloom::render::OcioGpuProgramStage::ProcessEffect,
                                      "bloom-test-multiple-lut-prefix", {});
    if (!reflected.program || reflected.program->textures.size() < 2) {
        expectations.expect(false, "the actual extracted prefix fixture requires multiple LUTs");
        return;
    }
    const auto& desc = *reflected.program;
    const auto spirv = compileGlsl(buildWrapperGlsl(desc, false));
    if (!spirv) {
        expectations.expect(false, "the actual multiple-LUT shader compiles");
        return;
    }
    bloom::render::GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(kBudget, true);
    budget.setDeviceCapacity(kBudget, true);
    auto madeDevice = GpuDevice::create(options);
    if (!madeDevice) {
        expectations.expect(false, "the required multiple-LUT native device is ready");
        return;
    }
    auto& device = *madeDevice.device;
    const auto envelope = bloom::render::GpuOcioProgramBudgets{}.maxOwnedBytes;
    auto phase = budget.reserve({.deviceWorkingBytes = envelope});
    if (!phase.lease || !device.retainProcessingPhase(phase.lease)) {
        expectations.expect(false, "the declared multiple-LUT envelope is retained once");
        return;
    }
    auto full = GpuOcioProgram::create(device, desc, *spirv, {}, {}, phase.lease);
    if (!full) {
        expectations.expect(false, "the complete actual multiple-LUT program uploads");
        return;
    }
    const auto complete = budget.snapshot();
    expectations.expect(complete.ownedNativeImages == desc.textures.size() &&
                            complete.ownedNativeImageBytes > 1,
                        "each actual extracted LUT owns its VMA backing");
    full.program.reset();
    device.retireProcessingPhase(phase.lease->id(), true);
    phase.lease.reset();
    expectations.expect(budget.snapshot().totalDeviceClaimBytes == 0 &&
                            budget.snapshot().ownedNativeImages == 0,
                        "complete provisional LUT backing frees before the next attempt");
    if (complete.ownedNativeImageBytes <= 1) {
        expectations.expect(false, "actual multiple-LUT bytes are measurable");
        return;
    }
    phase = budget.reserve({.deviceWorkingBytes = complete.ownedNativeImageBytes - 1});
    if (!phase.lease || !device.retainProcessingPhase(phase.lease)) {
        expectations.expect(false, "the actual aggregate one-byte-short phase is retained");
        return;
    }
    std::uint64_t actualPrefixImages = 0;
    auto denied = GpuOcioProgram::create(
        device, desc, *spirv, {},
        [&] {
            actualPrefixImages = std::max(actualPrefixImages, budget.snapshot().ownedNativeImages);
            return false;
        },
        phase.lease);
    const auto refusal = device.nativeImageStorageRefusal();
    if (!refusal) {
        expectations.expect(false, "later actual LUT allocation emits typed native refusal");
        return;
    }
    const auto refusalRevision = refusal->revisionAfterOwnRelease;
    const auto failed = budget.snapshot();
    expectations.expect(!denied &&
                            denied.diagnostic.code ==
                                bloom::render::GpuOcioProgramDiagnosticCode::OverBudget &&
                            actualPrefixImages > 0 && actualPrefixImages < desc.textures.size() &&
                            failed.ownedNativeImages == 0 && failed.ownedNativeImageBytes == 0 &&
                            failed.ownedNonlocalNativeHostBytes == 0 &&
                            device.ocioProgramRetirementStats().registeredPrograms == 0,
                        "later refusal destroys the actually uploaded factory prefix");
    device.retireProcessingPhase(phase.lease->id(), true);
    phase.lease.reset();
    const auto final = budget.snapshot();
    expectations.expect(
        final.admissionRevision == refusalRevision && final.admittedPhases == 0 &&
            final.unprovenPhases == 0 && final.totalHostClaimBytes == 0 &&
            final.totalDeviceClaimBytes == 0,
        "actual partial-array foldback plus own terminal release reaches exact R+1");
    phase = budget.reserve({.deviceWorkingBytes = envelope});
    if (!phase.lease || !device.retainProcessingPhase(phase.lease)) {
        expectations.expect(false, "partial publication uses the original declared envelope");
        return;
    }
    auto prefix = GpuOcioProgram::create(device, desc, *spirv, {}, {}, phase.lease);
    if (!prefix) {
        expectations.expect(false, "the partial-publication program actually creates both LUTs");
        return;
    }
    const auto publicationRevision = budget.snapshot().admissionRevision;
    bloom::render::ocio_program_detail::setPublicationFailureAfterForTest(1);
    const bool published = prefix.program->publishSampledStorage();
    bloom::render::ocio_program_detail::setPublicationFailureAfterForTest(
        std::numeric_limits<std::uint32_t>::max());
    expectations.expect(!published, "publication fails after the first real LUT promotion");
    prefix.program.reset();
    expectations.expect(budget.snapshot().ownedNativeImageBytes == 0 &&
                            budget.snapshot().ownedNativeImages == 0 &&
                            budget.snapshot().admissionRevision == publicationRevision,
                        "destroyed exclusive promoted prefix folds back without an own pulse");
    device.retireProcessingPhase(phase.lease->id(), true);
    phase.lease.reset();
    expectations.expect(budget.snapshot().admissionRevision == publicationRevision + 1 &&
                            budget.snapshot().totalDeviceClaimBytes == 0,
                        "partial publication has exactly one final phase-release revision");
    std::cout << "LUT_PUBLICATION_PREFIX promoted=1 actualTextures=" << desc.textures.size()
              << " finalR=" << budget.snapshot().admissionRevision << '\n';
    std::cout << "LUT_PREFIX textures=" << desc.textures.size()
              << " actualUploadedPrefix=" << actualPrefixImages
              << " completeNativeBytes=" << complete.ownedNativeImageBytes
              << " refusalR1=" << refusalRevision << " finalR=" << final.admissionRevision << '\n';
}

} // namespace bloom::color::ocio_resources_test
