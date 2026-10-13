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
    constexpr std::uint64_t sourceBytes =
        static_cast<std::uint64_t>(geometry.width) * geometry.height * sizeof(Rgba32f);
    const auto admit = [&](const std::uint64_t native) {
        // Preserve the original total host envelope F+3L while declaring the actual upload U.
        auto grant = budget.reserve({.fixedHostBytes = (1ULL << 20) - 3 * sourceBytes,
                                     .requestedSourceBytes = sourceBytes,
                                     .minimumSourceBytes = sourceBytes,
                                     .requestedExecutorBytes = native,
                                     .minimumExecutorBytes = native});
        if (!grant.lease || !device.retainProcessingPhase(grant.lease))
            throw std::runtime_error("max1: exact one-slot grant must be retained");
        return std::move(grant.lease);
    };
    const auto finish = [&](std::shared_ptr<GpuProcessingPhaseLease>& phase,
                            const std::uint64_t cachedInputBytes) {
        const std::weak_ptr<GpuProcessingPhaseLease> old = phase;
        device.retireProcessingPhase(phase->id(), true);
        phase.reset();
        // Host payload destruction is performed by the real retirement worker.
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while ((!old.expired() || budget.snapshot().ownedFinalHostAllocations != 0) &&
               std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        const auto final = budget.snapshot();
        expectations.expect(old.expired() && final.admittedPhases == 0 &&
                                final.unprovenPhases == 0 && final.ownedNativeBuffers == 0 &&
                                final.ownedNativeImages == 1 &&
                                final.ownedNativeImageBytes == cachedInputBytes &&
                                final.totalHostClaimBytes == final.ownedNonlocalNativeHostBytes &&
                                final.totalDeviceClaimBytes == cachedInputBytes,
                            "max1: actual transient destruction releases the grant; cached input "
                            "remains charged without retaining its creation phase");
    };
    auto phase = admit(1ULL << 20);
    auto input = uploadImage(*uploader.upload, geometry.width, geometry.height, pixels, phase);
    if (!input) {
        expectations.expect(false, "max1: real measurement image uploads");
        return;
    }
    const auto imageBytes = budget.snapshot().ownedNativeImageBytes;
    expectations.expect(imageBytes > 0 && budget.snapshot().ownedNativeImages == 1,
                        "max1: actual image allocation supplies the bounded refusal envelope");
    expectations.expect(input->promoteStorageOwnership(),
                        "max1: verified uploaded input becomes same-root cached storage");
    finish(phase, imageBytes);
    const std::atomic_bool stopRequested{false};
    for (const bool effect : {false, true}) {
        // The already-published same-root source needs no second upload. A one-byte executor
        // allowance cannot fit actual processing/readback staging, including allocator rounding.
        phase = admit(1);
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
                            "max1: genuine processing/readback buffer refusal is retired");
        stage.reset();
        finish(phase, imageBytes);
        // No root capacity or unknown-state mutation: only a new independently admitted envelope.
        phase = admit(1ULL << 20);
        stage.reset(stageFor(expectations, device));
        if (!stage) {
            expectations.expect(false, "max1: next admitted request has an actual stage");
            return;
        }
        retired = false;
        auto recovered = bloom::runtime::detail::runFinalCombinedReadback(
            *stage, *descriptor.value(), effect ? prepared.command : nullptr, input, kBudget,
            std::chrono::seconds(2), {}, stopRequested, retired, phase);
        const bool recoveryComplete =
            recovered.status == GpuProcessFrameStatus::Evaluated && recovered.processImage &&
            retired && recovered.readbacks == 1 && !stage->hasUnretiredSubmission() &&
            !device.outputColorReadbackPending();
        expectations.expect(recoveryComplete,
                            "max1: same-root next request completes the actual final helper");
        const auto cached = budget.snapshot();
        expectations.expect(effect ? cached.ownedNativeBuffers > 0 : cached.ownedNativeBuffers == 0,
                            "max1: only the effect arm retains charged OCIO program buffers");
        if (!recoveryComplete)
            std::cerr << "max1 recovery arm=" << (effect ? "effect" : "identity")
                      << " status=" << static_cast<unsigned>(recovered.status)
                      << " diagnostic=" << static_cast<unsigned>(recovered.diagnosticCode)
                      << " retired=" << retired << " readbacks=" << recovered.readbacks << ' '
                      << recovered.diagnosticMessage << '\n';
        recovered.processImage.reset();
        recovered.encodedEffectRgba32f.reset();
        recovered.encodedDisplayRgba8.reset();
        stage.reset();
        finish(phase, imageBytes);
        if (recoveryComplete)
            std::cout << "FINAL_READBACK_MAX1 arm=" << (effect ? "effect" : "identity")
                      << " sourceBacking=" << imageBytes << " refusalRetired=1 recovery=1\n";
        if (!effect)
            stage.reset(stageFor(expectations, device));
    }
    input.reset();
    const auto final = budget.snapshot();
    expectations.expect(final.ownedNativeImages == 0 && final.ownedNativeBuffers == 0 &&
                            final.totalHostClaimBytes == 0 && final.totalDeviceClaimBytes == 0,
                        "max1: last cached input release destroys backing before credit");
}
