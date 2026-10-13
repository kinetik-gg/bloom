#include "ocio_gpu_program_resources_test_support.hpp"

#include "ocio_gpu_program_extract.hpp"
#include "ocio_gpu_program_fault.hpp"

#include <limits>

namespace bloom::color::ocio_resources_test {

void testRetainedBuffers(Expectations& expectations, const GpuDeviceCreationOptions& options,
                         const bloom::color::ResolvedBloomNeutralConfig& aces) {
    auto description = buildOcioGpuProgramForExposureContrast(aces, "ACEScg", 0.5, 1.0);
    if (!description || description.program()->uniformBufferSize == 0 ||
        !description.program()->textures.empty()) {
        expectations.expect(false, "the actual UBO-only OCIO program extracts");
        return;
    }
    const auto& desc = *description.program();
    const auto spirv = compileGlsl(buildWrapperGlsl(desc, false));
    auto madeDevice = GpuDevice::create(options);
    if (!spirv || !madeDevice) {
        expectations.expect(false, "the required UBO/status shader and native device are ready");
        return;
    }
    auto& device = *madeDevice.device;
    bloom::render::GpuProcessingPhaseBudget budget(1);
    budget.setHostCapacity(kBudget, true);
    budget.setDeviceCapacity(kBudget, true);
    const auto envelope = bloom::render::GpuOcioProgramBudgets{}.maxOwnedBytes;
    auto phase = budget.reserve({.deviceWorkingBytes = envelope});
    if (!phase.lease || !device.retainProcessingPhase(phase.lease)) {
        expectations.expect(false, "one declared UBO/status phase is retained");
        return;
    }
    auto made = GpuOcioProgram::create(device, desc, *spirv, {}, {}, phase.lease);
    if (!made) {
        expectations.expect(false, "the actual UBO/status program creates");
        return;
    }
    const auto provisional = budget.snapshot();
    const auto bytes = made.program->retainedAllocationBytes();
    expectations.expect(provisional.ownedNativeBuffers == 2 &&
                            provisional.ownedNativeBufferBytes == bytes && bytes > 1 &&
                            provisional.ownedNativeImages == 0,
                        "actual UBO/status VMA allocations each own their exact backing charge");
    const auto revision = budget.snapshot().admissionRevision;
    bloom::render::ocio_program_detail::setPublicationFailureAfterForTest(1);
    const bool published = made.program->publishSampledStorage();
    bloom::render::ocio_program_detail::setPublicationFailureAfterForTest(UINT32_MAX);
    expectations.expect(!published, "actual publication rejects after promoting the UBO prefix");
    made.program.reset();
    expectations.expect(
        budget.snapshot().ownedNativeBuffers == 0 &&
            budget.snapshot().admissionRevision == revision &&
            budget.snapshot().deviceReservedBytes == phase.lease->deviceBytes(),
        "failed internal UBO publication destroys backing before quiet prefix fold");
    device.retireProcessingPhase(phase.lease->id(), true);
    phase.lease.reset();
    phase = budget.reserve({.deviceWorkingBytes = bytes - 1});
    if (!phase.lease || !device.retainProcessingPhase(phase.lease)) {
        expectations.expect(false, "the one-byte-short actual UBO/status phase is retained");
        return;
    }
    auto denied = GpuOcioProgram::create(device, desc, *spirv, {}, {}, phase.lease);
    const auto refusal = device.nativeBufferStorageRefusal();
    expectations.expect(
        !denied &&
            denied.diagnostic.code == bloom::render::GpuOcioProgramDiagnosticCode::OverBudget &&
            refusal && refusal->domain == bloom::render::GpuProcessingPhaseDomain::Device &&
            refusal->phaseId == phase.lease->id() && budget.snapshot().ownedNativeBuffers == 0,
        "later status refusal destroys the real UBO prefix with typed phase retry");
    device.retireProcessingPhase(phase.lease->id(), true);
    phase.lease.reset();
    if (refusal)
        expectations.expect(budget.snapshot().admissionRevision == refusal->revisionAfterOwnRelease,
                            "failed UBO/status prefix has only its exact terminal release pulse");
    phase = budget.reserve({.deviceWorkingBytes = envelope});
    if (!phase.lease || !device.retainProcessingPhase(phase.lease)) {
        expectations.expect(false, "the original UBO/status envelope is reused");
        return;
    }
    made = GpuOcioProgram::create(device, desc, *spirv, {}, {}, phase.lease);
    if (!made || !made.program->publishSampledStorage()) {
        expectations.expect(false, "the complete UBO/status retained set publishes");
        return;
    }
    auto retained = std::shared_ptr<GpuOcioProgram>(std::move(made.program));
    const std::weak_ptr<bloom::render::GpuProcessingPhaseLease> origin = phase.lease;
    device.retireProcessingPhase(phase.lease->id(), true);
    phase.lease.reset();
    expectations.expect(origin.expired() && budget.snapshot().admittedPhases == 0 &&
                            budget.snapshot().totalDeviceClaimBytes == bytes &&
                            budget.snapshot().ownedNativeBuffers == 2,
                        "published UBO/status backing retains bytes without pinning the old phase");
    // The existing explicit rootless input helper prepares the independent CPU-oracle fixture;
    // every program dispatch below runs under a new phase of the same one-count processing root.
    auto uploader = GpuImageUpload::create(device);
    constexpr std::uint32_t width = 4, height = 2;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploader ? uploadImage(*uploader.upload, width, height, pixels) : nullptr;
    const auto cpu = exposureContrastCpu(0.5, 1.0);
    phase = budget.reserve({.deviceWorkingBytes = envelope});
    if (!input || !cpu || !phase.lease || !device.retainProcessingPhase(phase.lease)) {
        expectations.expect(false, "the retained UBO program admits a new same-root phase");
        return;
    }
    for (const char* label : {"cold retained UBO", "warm retained UBO"}) {
        expectations.expect(retained->beginEffect(input, {}, kBudget).code ==
                                bloom::render::GpuOcioProgramDiagnosticCode::None,
                            "retained UBO/status dispatch submits genuine native work");
        if (!pollOcio(expectations, *retained, label))
            return;
        auto output = retained->takeEffectOutput();
        if (!output) {
            expectations.expect(false, "retained UBO dispatch publishes actual native pixels");
            return;
        }
        const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
        expectations.expect(bool(readback), "retained UBO pixels read back at the oracle boundary");
        if (readback)
            compareEffect(
                expectations, pixels, readback.pixels,
                [&](std::array<float, 3>& rgb) { cpu->applyRGB(rgb.data()); }, label);
        expectations.expect(budget.snapshot().ownedNativeBufferBytes == bytes &&
                                budget.snapshot().ownedNativeBuffers == 2,
                            "cold/warm execution retains exactly one UBO/status set");
    }
    device.retireProcessingPhase(phase.lease->id(), true);
    phase.lease.reset();
    std::vector<std::shared_ptr<GpuOcioProgram>> aliases(256, retained);
    retained.reset();
    std::thread foreign([held = std::move(aliases)]() mutable { held.clear(); });
    foreign.join();
    expectations.expect(device.ocioProgramRetirementStats().pendingPrograms == 1 &&
                            budget.snapshot().ownedNativeBufferBytes == bytes &&
                            budget.snapshot().admittedPhases == 0,
                        "foreign final buffer-cache release retains charges until owner drain");
    device.reclaimReleasedImages();
    expectations.expect(budget.snapshot().ownedNativeBuffers == 0 &&
                            budget.snapshot().totalDeviceClaimBytes == 0 &&
                            budget.snapshot().totalHostClaimBytes == 0 &&
                            device.ocioProgramRetirementStats().registeredPrograms == 0,
                        "actual owner UBO/status destruction releases all alias-backed claims");
    std::cout << "OCIO_BUFFER_STORAGE actualBuffers=2 bytes=" << bytes
              << " prefixRollback=executed coldWarmPixels=executed foreignDrain=executed\n";
}

void testRetainedPacked(Expectations& expectations, const GpuDeviceCreationOptions& options,
                        const bloom::color::ResolvedBloomNeutralConfig& aces) {
    auto description = buildOcioGpuProgramForDisplay(
        aces, "Rec.2100-PQ - Display", "ACES 1.1 - HDR Video (1000 nits & Rec.2020 lim)");
    if (!description) {
        expectations.expect(false, "the actual retained packed display program extracts");
        return;
    }
    const auto spirv = compileGlsl(buildWrapperGlsl(*description.program(), true));
    auto madeDevice = GpuDevice::create(options);
    if (!spirv || !madeDevice) {
        expectations.expect(false, "the actual packed display shader/device are ready");
        return;
    }
    auto& device = *madeDevice.device;
    auto uploader = GpuImageUpload::create(device);
    auto smallInput = uploader ? uploadImage(*uploader.upload, 4, 3, fixturePixels(4, 3)) : nullptr;
    auto largeInput = uploader ? uploadImage(*uploader.upload, 8, 5, fixturePixels(8, 5)) : nullptr;
    if (!smallInput || !largeInput) {
        expectations.expect(false, "independent rootless packed-oracle inputs upload");
        return;
    }
    bloom::render::GpuProcessingPhaseBudget budget(1);
    budget.setHostCapacity(kBudget, true);
    budget.setDeviceCapacity(kBudget, true);
    const auto envelope = bloom::render::GpuOcioProgramBudgets{}.maxOwnedBytes;
    auto phase = budget.reserve({.deviceWorkingBytes = envelope});
    if (!phase.lease || !device.retainProcessingPhase(phase.lease)) {
        expectations.expect(false, "the retained packed program gets one creating phase");
        return;
    }
    auto made = GpuOcioProgram::create(device, *description.program(), *spirv, {}, {}, phase.lease);
    if (!made) {
        expectations.expect(false, "the actual packed program creates");
        return;
    }
    auto retained = std::shared_ptr<GpuOcioProgram>(std::move(made.program));
    if (!retained->publishSampledStorage()) {
        expectations.expect(false, "the complete packed program retained set publishes");
        return;
    }
    const auto staticBuffers = budget.snapshot().ownedNativeBuffers;
    device.retireProcessingPhase(phase.lease->id(), true);
    phase.lease.reset();
    std::uint64_t smallPackedBytes = 0;
    std::vector<bloom::render::Rgba8> coldPixels;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const auto& input = attempt < 2 ? smallInput : largeInput;
        phase = budget.reserve({.deviceWorkingBytes = envelope});
        if (!phase.lease || !device.retainProcessingPhase(phase.lease)) {
            expectations.expect(false, "capacity-one cold/warm/changed-size phase is admitted");
            return;
        }
        expectations.expect(retained->beginDisplay(input, {}, kBudget).code ==
                                bloom::render::GpuOcioProgramDiagnosticCode::None,
                            "the retained packed program submits actual native display work");
        if (!pollOcio(expectations, *retained, "retained packed display"))
            return;
        std::optional<GpuDisplayImage> output(retained->takeDisplayOutput());
        const auto readback = bloom::render::readbackResidentDisplayImage(*output, kBudget);
        expectations.expect(
            output->isValid() && bool(readback),
            "fence-ready retained packed publication yields actual readable pixels");
        if (!readback)
            return;
        if (attempt == 0)
            coldPixels = readback.pixels;
        else if (attempt == 1)
            expectations.expect(readback.pixels == coldPixels,
                                "same-size warm retained packed pixels match the cold result");
        output.reset();
        const auto live = budget.snapshot();
        expectations.expect(live.ownedNativeBuffers == staticBuffers + 1,
                            "retained packed backing has one unique charge beside static buffers");
        if (attempt == 0)
            smallPackedBytes = live.ownedNativeBufferBytes;
        else if (attempt == 1)
            expectations.expect(live.ownedNativeBufferBytes == smallPackedBytes,
                                "same-size warm packed reuse adds zero retained buffer bytes");
        else
            expectations.expect(live.ownedNativeBufferBytes > smallPackedBytes,
                                "changed geometry replaces the actual packed backing charge");
        const std::weak_ptr<bloom::render::GpuProcessingPhaseLease> origin = phase.lease;
        device.retireProcessingPhase(phase.lease->id(), true);
        phase.lease.reset();
        expectations.expect(origin.expired() && budget.snapshot().admittedPhases == 0 &&
                                budget.snapshot().totalDeviceClaimBytes ==
                                    live.ownedNativeImageBytes + live.ownedNativeBufferBytes,
                            "each retired packed publication detaches its current job phase");
    }
    retained.reset();
    expectations.expect(budget.snapshot().ownedNativeBuffers == 0 &&
                            budget.snapshot().ownedNativeImages == 0 &&
                            budget.snapshot().totalDeviceClaimBytes == 0 &&
                            budget.snapshot().totalHostClaimBytes == 0,
                        "actual changed-size packed cache destruction returns all backing charges");
    std::cout << "OCIO_PACKED_STORAGE capacity=1 coldWarmChangedSize=executed "
                 "phaseDetached=3 actualDrop=executed\n";
}

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
    std::size_t actualPrefixImages = 0;
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
