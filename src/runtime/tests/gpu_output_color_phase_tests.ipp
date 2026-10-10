// Actual final helper/Stage budget cleanup under one immutable admission slot. This is a
// configured envelope control, not evidence of ordinary application capacity or CPU storage
// ownership.
void testSinglePhaseReadbackRecovery(Expectations& expectations, GpuDevice& device,
                                     GpuOcioProgramPreparer& preparer, const Fixtures& fixtures) {
    using bloom::render::GpuProcessingPhaseBudget;
    using bloom::render::GpuProcessingPhaseLease;
    using bloom::runtime::GpuProcessFrameDiagnosticCode;
    using bloom::runtime::GpuProcessFrameStatus;
    if (!fixtures.aces) {
        expectations.expect(false, "max1: ACES config exists");
        return;
    }
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Cst;
    spec.fromId = "ACES2065-1";
    spec.toId = "ACEScg";
    constexpr GpuOcioCommandGeometry geometry{4, 3};
    auto prepared = preparer.prepare(*fixtures.aces, spec, geometry, fixtures.options);
    auto uploader = GpuImageUpload::create(device);
    std::unique_ptr<GpuOutputColorStage> stage(stageFor(expectations, device));
    if (!prepared || !uploader || !stage) {
        expectations.expect(false, "max1: actual command, uploader and stage exist");
        return;
    }
    const auto window = bloom::render::ImageWindow::create(0, 0, geometry.width, geometry.height);
    if (!window) {
        expectations.expect(false, "max1: geometry exists");
        return;
    }
    const auto descriptor = Rgba32fImageDescriptor::create(*window.value(), *window.value(),
                                                           bloom::core::PixelAspectRatio::square());
    if (!descriptor) {
        expectations.expect(false, "max1: descriptor exists");
        return;
    }
    const auto pixels = fixturePixels(geometry.width, geometry.height);
    bloom::render::test::FinalHostRetirementScope host;
    GpuProcessingPhaseBudget budget(1, host.owner);
    budget.setHostCapacity(1ULL << 30, true);
    budget.setDeviceCapacity(1ULL << 30, true);
    const auto admit = [&](const std::uint64_t native) {
        auto grant = budget.reserve({.fixedHostBytes = 1ULL << 20,
                                     .requestedExecutorBytes = native,
                                     .minimumExecutorBytes = native,
                                     .sourceFree = true});
        if (!grant.lease || !device.retainProcessingPhase(grant.lease))
            throw std::runtime_error("max1: exact one-slot grant must be retained");
        return std::move(grant.lease);
    };
    const auto finish = [&](std::shared_ptr<GpuProcessingPhaseLease>& phase) {
        const std::weak_ptr<GpuProcessingPhaseLease> old = phase;
        device.retireProcessingPhase(phase->id(), true);
        phase.reset();
        const auto final = budget.snapshot();
        expectations.expect(old.expired() && final.admittedPhases == 0 &&
                                final.unprovenPhases == 0 && final.ownedNativeBuffers == 0 &&
                                final.ownedNativeImages == 0 && final.totalHostClaimBytes == 0 &&
                                final.totalDeviceClaimBytes == 0,
                            "max1: transient aliases and actual backing release the old grant");
    };
    auto phase = admit(1ULL << 20);
    auto input = uploadImage(*uploader.upload, geometry.width, geometry.height, pixels);
    if (!input) {
        expectations.expect(false, "max1: real measurement image uploads");
        return;
    }
    const auto imageBytes = budget.snapshot().ownedNativeImageBytes;
    expectations.expect(imageBytes > 0 && budget.snapshot().ownedNativeImages == 1,
                        "max1: actual image allocation supplies the bounded refusal envelope");
    input.reset();
    finish(phase);
    const std::atomic_bool stopRequested{false};
    for (const bool effect : {false, true}) {
        phase = admit(imageBytes * (effect ? 2U : 1U));
        input = uploadImage(*uploader.upload, geometry.width, geometry.height, pixels);
        if (!input) {
            expectations.expect(false, "max1: source fits the declared refusal envelope");
            return;
        }
        bool retired = false;
        const auto refused = bloom::runtime::detail::runFinalCombinedReadback(
            *stage, *descriptor.value(), effect ? prepared.command : nullptr, input, kBudget,
            std::chrono::seconds(2), {}, stopRequested, retired, phase);
        const auto metadata = device.nativeBufferStorageRefusal();
        expectations.expect(refused.status == GpuProcessFrameStatus::Failed &&
                                refused.diagnosticCode ==
                                    GpuProcessFrameDiagnosticCode::ReadbackOverBudget &&
                                retired && metadata && metadata->phaseId == phase->id() &&
                                !phase->isUnproven() && budget.snapshot().ownedNativeBuffers == 0,
                            "max1: genuine synchronous/asynchronous staging refusal is retired");
        input.reset();
        finish(phase);
        // No root capacity or unknown-state mutation: only a new independently admitted envelope.
        phase = admit(1ULL << 20);
        input = uploadImage(*uploader.upload, geometry.width, geometry.height, pixels);
        if (!input) {
            expectations.expect(false, "max1: next admitted request uploads");
            return;
        }
        retired = false;
        auto recovered = bloom::runtime::detail::runFinalCombinedReadback(
            *stage, *descriptor.value(), effect ? prepared.command : nullptr, input, kBudget,
            std::chrono::seconds(2), {}, stopRequested, retired, phase);
        expectations.expect(recovered.status == GpuProcessFrameStatus::Evaluated &&
                                recovered.processImage && retired && recovered.readbacks == 1 &&
                                budget.snapshot().ownedNativeBuffers == 0,
                            "max1: same-root next request completes the actual final helper");
        recovered.processImage.reset();
        recovered.encodedEffectRgba32f.reset();
        recovered.encodedDisplayRgba8.reset();
        input.reset();
        finish(phase);
        std::cout << "FINAL_READBACK_MAX1 arm=" << (effect ? "effect" : "identity")
                  << " sourceBacking=" << imageBytes << " refusalRetired=1 recovery=1\n";
    }
}
