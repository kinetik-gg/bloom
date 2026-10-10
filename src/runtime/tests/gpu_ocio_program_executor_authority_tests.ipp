// Private fixture implementation included after the native OCIO preparation helpers.
// Root affinity is independent of sampled-texture count; unknown authority is never reopened
// merely because a quarantine slot retires. These are authority/lifecycle proofs, not LUT-byte
// accounting or a new source-color interpretation.

void testRootAuthority(Expectations& expectations, GpuDevice& device,
                       GpuOcioProgramPreparer& preparer, const Fixtures& fixtures) {
    if (!fixtures.aces) {
        expectations.expect(false, "the ACES configuration is required for authority proof");
        return;
    }
    const auto& aces = *fixtures.aces;
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Cst;
    spec.fromId = "ACES2065-1";
    spec.toId = "ACEScg";
    constexpr GpuOcioCommandGeometry geometry{4, 3};
    const auto prepared = preparer.prepare(aces, spec, geometry, fixtures.options);
    expectations.expect(prepared.hasValue() && prepared.command->program().textures.empty(),
                        "the authority probe is a real zero-LUT matrix program");
    if (!prepared || !prepared.command->program().textures.empty()) {
        return;
    }
    bloom::render::GpuProcessingPhaseBudget first;
    bloom::render::GpuProcessingPhaseBudget second;
    for (auto* budget : {&first, &second}) {
        budget->setHostCapacity(kBudget, true);
        budget->setDeviceCapacity(kBudget, true);
    }
    const bloom::render::GpuProcessingPhaseRequest request{
        .fixedHostBytes = 1024,
        .deviceWorkingBytes = 64ULL * 1024ULL * 1024ULL,
    };
    auto a = first.reserve(request);
    auto b = second.reserve(request);
    expectations.expect(a.lease && b.lease && a.lease->id() == b.lease->id(),
                        "separate real roots supply equal numeric phase ids");
    if (!a.lease || !b.lease) {
        return;
    }
    expectations.expect(device.retainProcessingPhase(a.lease), "the exact first phase is retained");
    if (device.retainedProcessingPhase() != a.lease) {
        expectations.expect(false, "the device returns its exact retained authority");
        return;
    }
    const auto& bytes = prepared.command->artifact().spirv;
    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    auto omitted =
        bloom::render::GpuOcioProgram::create(device, prepared.command->program(), words);
    auto foreign = bloom::render::GpuOcioProgram::create(device, prepared.command->program(), words,
                                                         {}, {}, b.lease);
    expectations.expect(!omitted && !foreign &&
                            omitted.diagnostic.code ==
                                bloom::render::GpuOcioProgramDiagnosticCode::InvalidArgument &&
                            foreign.diagnostic.code ==
                                bloom::render::GpuOcioProgramDiagnosticCode::InvalidArgument,
                        "rooted creation rejects omission and equal-id foreign authority");
    auto exact = bloom::render::GpuOcioProgram::create(device, prepared.command->program(), words,
                                                       {}, {}, a.lease);
    expectations.expect(exact && exact.program->creationRootIdentity() == a.lease->rootIdentity(),
                        "zero-LUT creation records the exact neutral root");
    auto uploader = GpuImageUpload::create(device);
    auto executor = GpuOcioProgramExecutor::create(device);
    if (!uploader || !executor) {
        expectations.expect(false, "the authority execution fixture is ready");
        device.retireProcessingPhase(a.lease->id(), true);
        return;
    }
    auto input = uploadImage(*uploader.upload, geometry.width, geometry.height,
                             fixturePixels(geometry.width, geometry.height));
    if (!input) {
        expectations.expect(false, "the rooted authority input uploads");
        device.retireProcessingPhase(a.lease->id(), true);
        return;
    }
    const auto begin = executor.executor->begin(prepared.command, input, {}, kBudget);
    expectations.expect(begin.code == GpuOcioExecutorDiagnosticCode::None,
                        "the first rooted zero-LUT program dispatches");
    auto poll = executor.executor->poll();
    while (poll == GpuOcioExecutorPollResult::Pending) {
        poll = executor.executor->poll();
    }
    expectations.expect(poll == GpuOcioExecutorPollResult::Ready,
                        "the rooted zero-LUT dispatch retires");
    auto output = executor.executor->takeEffectOutput();
    output.reset();
    input.reset();
    executor.executor->cancel();
    device.retireProcessingPhase(a.lease->id(), true);
    const std::weak_ptr<bloom::render::GpuProcessingPhaseLease> weak = a.lease;
    a.lease.reset();
    expectations.expect(weak.expired() && first.snapshot().admittedPhases == 0,
                        "a retained zero-LUT cache entry does not pin its creation phase");
    expectations.expect(device.retainProcessingPhase(b.lease),
                        "the foreign root phase is retained");
    input = uploadImage(*uploader.upload, geometry.width, geometry.height,
                        fixturePixels(geometry.width, geometry.height));
    if (exact) {
        const auto direct = exact.program->beginEffect(input, {}, kBudget);
        expectations.expect(direct.code ==
                                bloom::render::GpuOcioProgramDiagnosticCode::InvalidArgument,
                            "a direct zero-LUT program rejects foreign-root execution");
    }
    const auto before = executor.executor->counters();
    const auto rejected = executor.executor->begin(prepared.command, input, {}, kBudget);
    const auto after = executor.executor->counters();
    expectations.expect(rejected.code == GpuOcioExecutorDiagnosticCode::InvalidArgument &&
                            after.programCreations == before.programCreations &&
                            after.programReuses == before.programReuses,
                        "zero-LUT cached root affinity rejects foreign reuse without creation");
    b.lease->markUnproven();
    auto unproven = bloom::render::GpuOcioProgram::create(device, prepared.command->program(),
                                                          words, {}, {}, b.lease);
    const auto closed = executor.executor->begin(prepared.command, input, {}, kBudget);
    expectations.expect(!unproven &&
                            unproven.diagnostic.code ==
                                bloom::render::GpuOcioProgramDiagnosticCode::DeviceUnavailable &&
                            closed.code == GpuOcioExecutorDiagnosticCode::OwnerDrainRequired &&
                            device.retainedProcessingPhase() == b.lease,
                        "unproven authority refuses cold and cached work without becoming legacy");
    exact.program.reset();
    executor.executor.reset();
    input.reset();
    uploader.upload.reset();
    device.retireProcessingPhase(b.lease->id(), true);
    b.lease.reset();
    expectations.expect(first.snapshot().totalDeviceClaimBytes == 0 &&
                            second.snapshot().totalDeviceClaimBytes == 0,
                        "authority probes retire both roots completely");
}

void testColdGeneration(Expectations& expectations, const GpuDeviceCreationOptions& options,
                        GpuOcioProgramPreparer& preparer, const Fixtures& fixtures) {
    if (!fixtures.aces) {
        expectations.expect(false, "the ACES configuration is required for authority proof");
        return;
    }
    const auto& aces = *fixtures.aces;
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Cst;
    spec.fromId = "ACES2065-1";
    spec.toId = "ACEScg";
    constexpr GpuOcioCommandGeometry geometry{4, 3};
    const auto prepared = preparer.prepare(aces, spec, geometry, fixtures.options);
    auto original = GpuDevice::create(options);
    auto replacement = GpuDevice::create(options);
    expectations.expect(prepared && original && replacement,
                        "the cold-generation authority fixture is ready");
    if (!prepared || !original || !replacement) {
        return;
    }
    auto executor = GpuOcioProgramExecutor::create(*original.device);
    if (!executor) {
        expectations.expect(false, "the original-generation executor is ready");
        return;
    }
    *original.device = std::move(*replacement.device);
    auto uploader = GpuImageUpload::create(*original.device);
    if (!uploader) {
        expectations.expect(false, "the replacement-generation uploader is ready");
        return;
    }
    const auto input = uploadImage(*uploader.upload, geometry.width, geometry.height,
                                   fixturePixels(geometry.width, geometry.height));
    const auto refused = executor.executor->begin(prepared.command, input, {}, kBudget);
    const auto counters = executor.executor->counters();
    expectations.expect(
        input && refused.code == GpuOcioExecutorDiagnosticCode::InvalidArgument &&
            counters.programCreations == 0 && counters.programReuses == 0,
        "a stale executor refuses cold creation after facade generation replacement");
}

void testUploadFaultRetirement(Expectations& expectations, GpuDevice& device,
                               GpuOcioProgramPreparer& preparer, const Fixtures& fixtures,
                               const GpuDeviceCreationOptions& options) {
    if (!fixtures.aces.has_value() || !fixtures.neutral.has_value()) {
        expectations.expect(false, "the fixture configs resolve for the fault arm");
        return;
    }
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Display;
    spec.display = "Rec.2100-PQ - Display";
    spec.view = "ACES 1.1 - HDR Video (1000 nits & Rec.2020 lim)";
    constexpr GpuOcioCommandGeometry geometry{4, 3};
    const auto prepared = preparer.prepare(*fixtures.aces, spec, geometry, fixtures.options);
    expectations.expect(prepared.hasValue(), "the LUT display command prepares");
    if (!prepared) {
        return;
    }
    auto executor = GpuOcioProgramExecutor::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!executor || !uploader) {
        expectations.expect(false, "the fault executor and uploader host");
        return;
    }
    const auto pixels = fixturePixels(geometry.width, geometry.height);
    auto input = uploadImage(*uploader.upload, geometry.width, geometry.height, pixels);
    if (input == nullptr) {
        expectations.expect(false, "the fault input uploads");
        return;
    }
    bloom::render::GpuProcessingPhaseBudget phases;
    phases.setHostCapacity(kBudget, true);
    phases.setDeviceCapacity(kBudget, true);
    // This control declares the existing per-program resource envelope. The former 8KiB grant
    // predated LUT accounting and cannot authorize real HDR sampled-image backing.
    const auto programEnvelope = GpuOcioExecutorBudgets{}.maxOwnedBytesPerProgram;
    auto phase = phases.reserve(
        {.fixedHostBytes = 4096, .deviceWorkingBytes = programEnvelope, .sourceFree = true});
    expectations.expect(phase.lease && device.retainProcessingPhase(phase.lease),
                        "the production native generation arms the LUT fault phase");
    if (!phase.lease) {
        return;
    }
    bloom::render::ocio_program_detail::setUploadFenceOverrideForTest(
        bloom::render::ocio_program_detail::UploadFenceOverride::CancelAfterSubmit);
    const auto beforeFactory = device.ocioProgramRetirementStats();
    const auto faulted = executor.executor->begin(prepared.command, input, {}, kBudget);
    expectations.expect(faulted.code == GpuOcioExecutorDiagnosticCode::Cancelled,
                        "a cancelled post-submit LUT upload is a typed failure");
    expectations.expect(bloom::render::ocio_program_detail::uploadQuarantineOccupiedForTest(),
                        "the unproven upload retains its resources in the quarantine");
    const auto quarantinedClaims = phases.snapshot();
    expectations.expect(quarantinedClaims.ownedNativeImages > 0 &&
                            quarantinedClaims.ownedNativeImageBytes > 0,
                        "the actual submitted LUT backing remains owned in upload quarantine");
    const auto failedFactory = device.ocioProgramRetirementStats();
    expectations.expect(failedFactory.registeredPrograms == beforeFactory.registeredPrograms &&
                            failedFactory.pendingUploads == 1,
                        "failed factory unregisters but its exact native upload still gates exit");
    device.retireProcessingPhase(phase.lease->id(), true);
    expectations.expect(device.processingPhaseOccupied() && phases.snapshot().unprovenPhases == 1,
                        "cancelled independent LUT quarantine overrides claimed executor proof");
    phase.lease.reset();
    expectations.expect(phases.snapshot().admittedPhases == 1,
                        "generation ownership retains the phase after consumer terminal disposal");
    bloom::render::ocio_program_detail::setUploadFenceOverrideForTest(
        bloom::render::ocio_program_detail::UploadFenceOverride::None);
    for (int attempt = 0;
         attempt < 200 && bloom::render::ocio_program_detail::uploadQuarantineOccupiedForTest();
         ++attempt) {
        bloom::render::ocio_program_detail::retireUploadQuarantineForTest();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expectations.expect(!bloom::render::ocio_program_detail::uploadQuarantineOccupiedForTest(),
                        "the quarantined upload retires after fence proof");
    expectations.expect(device.ocioProgramRetirementStats().pendingUploads == 0,
                        "actual upload fence retirement removes the generation pending slot");
    expectations.expect(phases.snapshot().ownedNativeImages == 0 &&
                            phases.snapshot().ownedNativeImageBytes == 0,
                        "actual quarantined LUT destruction precedes removal of its image charge");
    const auto refused = executor.executor->begin(prepared.command, input, {}, kBudget);
    expectations.expect(refused.code == GpuOcioExecutorDiagnosticCode::OwnerDrainRequired &&
                            phases.snapshot().admittedPhases == 1 &&
                            phases.snapshot().unprovenPhases == 1,
                        "slot fence retirement does not clear the generation's sticky authority");
    executor.executor.reset();
    input.reset();
    uploader.upload.reset();
    auto fresh = GpuDevice::create(options);
    expectations.expect(fresh.hasValue(), "a fresh recovery generation is created");
    if (!fresh) {
        return;
    }
    device = std::move(*fresh.device);
    expectations.expect(phases.snapshot().admittedPhases == 0 &&
                            phases.snapshot().unprovenPhases == 0,
                        "actual old generation destruction releases its sticky phase");
    executor = GpuOcioProgramExecutor::create(device);
    uploader = GpuImageUpload::create(device);
    if (!executor || !uploader) {
        expectations.expect(false, "the fresh recovery executor and uploader are ready");
        return;
    }
    input = uploadImage(*uploader.upload, geometry.width, geometry.height, pixels);
    const auto recovered = executor.executor->begin(prepared.command, input, {}, kBudget);
    expectations.expect(input && recovered.code == GpuOcioExecutorDiagnosticCode::None,
                        "a fresh generation executes after the old unknown generation retires");
    auto poll = executor.executor->poll();
    while (poll == GpuOcioExecutorPollResult::Pending) {
        poll = executor.executor->poll();
    }
    expectations.expect(poll == GpuOcioExecutorPollResult::Ready,
                        "fresh-generation recovery produces a genuine native result");
}
