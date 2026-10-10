// Actual native program lifetime control. Zero sampled textures isolate the registry from LUT
// accounting; the later textured witnesses must additionally prove token/backing conservation.
void testProgramOwnerDrain(Expectations& expectations, GpuDevice& device,
                           GpuOcioProgramPreparer& preparer, const Fixtures& fixtures) {
    if (!fixtures.aces) {
        expectations.expect(false, "the ACES configuration is required for program retirement");
        return;
    }
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Cst;
    spec.fromId = "ACES2065-1";
    spec.toId = "ACEScg";
    const auto prepared = preparer.prepare(*fixtures.aces, spec, {4, 3}, fixtures.options);
    if (!prepared || !prepared.command->program().textures.empty()) {
        expectations.expect(false, "the program retirement control prepares a zero-LUT program");
        return;
    }
    const auto& bytes = prepared.command->artifact().spirv;
    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    bloom::render::GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(kBudget, true);
    budget.setDeviceCapacity(kBudget, true);
    auto phase = budget.reserve({.deviceWorkingBytes = 64ULL * 1024ULL * 1024ULL});
    if (!phase.lease || !device.retainProcessingPhase(phase.lease)) {
        expectations.expect(false, "the program retirement control retains its exact phase");
        return;
    }
    const auto baseline = device.ocioProgramRetirementStats();
    auto made = bloom::render::GpuOcioProgram::create(device, prepared.command->program(), words,
                                                      {}, {}, phase.lease);
    if (!made) {
        expectations.expect(false, "the real program is registered before native creation");
        device.retireProcessingPhase(phase.lease->id(), true);
        return;
    }
    auto alias = std::shared_ptr<bloom::render::GpuOcioProgram>(std::move(made.program));
    auto retained = alias;
    device.reclaimReleasedImages();
    expectations.expect(
        device.ocioProgramRetirementStats().registeredPrograms == baseline.registeredPrograms + 1 &&
            device.ocioProgramRetirementStats().pendingPrograms == baseline.pendingPrograms,
        "an unreleased live alias remains registered across native drain");
    const auto claims = budget.snapshot();
    alias.reset();
    std::thread foreign([held = std::move(retained), &device]() mutable {
        held.reset();
        device.reclaimReleasedImages(); // A foreign tick cannot query or free native resources.
    });
    foreign.join();
    const auto pending = device.ocioProgramRetirementStats();
    expectations.expect(pending.registeredPrograms == baseline.registeredPrograms + 1 &&
                            pending.pendingPrograms == baseline.pendingPrograms + 1 &&
                            pending.closedOwnerPendingPrograms == 0 &&
                            budget.snapshot().totalDeviceClaimBytes == claims.totalDeviceClaimBytes,
                        "foreign final release retains registration and phase through owner drain");
    device.reclaimReleasedImages();
    const auto retired = device.ocioProgramRetirementStats();
    expectations.expect(retired.registeredPrograms == baseline.registeredPrograms &&
                            retired.pendingPrograms == baseline.pendingPrograms,
                        "actual native owner destruction unregisters the complete program");
    auto retry = bloom::render::GpuOcioProgram::create(device, prepared.command->program(), words,
                                                       {}, {}, phase.lease);
    expectations.expect(retry.hasValue(),
                        "recoverable foreign release does not trip the permanent quarantine fuse");
    retry.program.reset();
    device.retireProcessingPhase(phase.lease->id(), true);
    phase.lease.reset();
    expectations.expect(budget.snapshot().admittedPhases == 0 &&
                            budget.snapshot().totalDeviceClaimBytes == 0,
                        "the program lifetime control retires its root completely");
}
