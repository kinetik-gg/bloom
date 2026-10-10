// Real extracted textured programs. Image-allocation identity/fault/dimensional controls live in
// the render owner fixture; these checks qualify retained program aliases and submitted work.
void testRootedLutLifetime(Expectations& expectations, const GpuDeviceCreationOptions& options,
                           GpuOcioProgramPreparer& preparer, const Fixtures& fixtures) {
    if (!fixtures.aces) {
        expectations.expect(false, "the ACES configuration is required for LUT ownership");
        return;
    }
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Display;
    spec.display = "Rec.2100-PQ - Display";
    spec.view = "ACES 1.1 - HDR Video (1000 nits & Rec.2020 lim)";
    constexpr GpuOcioCommandGeometry geometry{4, 3};
    const auto prepared = preparer.prepare(*fixtures.aces, spec, geometry, fixtures.options);
    if (!prepared || prepared.command->program().textures.empty()) {
        expectations.expect(false, "a real textured HDR program prepares");
        return;
    }
    const auto& bytes = prepared.command->artifact().spirv;
    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    bloom::render::GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(kBudget, true);
    budget.setDeviceCapacity(kBudget, true);
    auto madeDevice = GpuDevice::create(options);
    if (!madeDevice) {
        expectations.expect(false, "the actual LUT lifetime device is ready");
        return;
    }
    auto& device = *madeDevice.device;
    const auto envelope = bloom::render::GpuOcioProgramBudgets{}.maxOwnedBytes;
    auto phase = budget.reserve({.deviceWorkingBytes = envelope});
    if (!phase.lease || !device.retainProcessingPhase(phase.lease)) {
        expectations.expect(false, "the declared program envelope is retained once");
        return;
    }
    auto made = bloom::render::GpuOcioProgram::create(device, prepared.command->program(), words,
                                                      {}, {}, phase.lease);
    if (!made) {
        expectations.expect(false, "the rooted extracted LUT program uploads");
        return;
    }
    const auto provisional = budget.snapshot();
    expectations.expect(provisional.ownedNativeImages ==
                                prepared.command->program().textures.size() &&
                            provisional.ownedNativeImageBytes > 0,
                        "each actual retained LUT carries one unique native token");
    auto retained = std::shared_ptr<bloom::render::GpuOcioProgram>(std::move(made.program));
    expectations.expect(retained->publishSampledStorage(),
                        "actual retained LUT publication succeeds");
    const std::weak_ptr<bloom::render::GpuProcessingPhaseLease> weak = phase.lease;
    device.retireProcessingPhase(phase.lease->id(), true);
    phase.lease.reset();
    const auto published = budget.snapshot();
    expectations.expect(weak.expired() && published.admittedPhases == 0 &&
                            published.ownedNativeImageBytes == provisional.ownedNativeImageBytes &&
                            published.totalDeviceClaimBytes == published.ownedNativeImageBytes,
                        "published LUT ownership outlives its phase without pinning it");
    std::vector<std::shared_ptr<bloom::render::GpuOcioProgram>> aliases(256, retained);
    retained.reset();
    expectations.expect(budget.snapshot().ownedNativeImageBytes ==
                                published.ownedNativeImageBytes &&
                            budget.snapshot().ownedNativeImages == published.ownedNativeImages,
                        "256 aliases preserve one charge per actual sampled backing");
    std::thread foreign([held = std::move(aliases), &device]() mutable {
        held.clear();
        device.reclaimReleasedImages();
    });
    foreign.join();
    expectations.expect(
        device.ocioProgramRetirementStats().pendingPrograms == 1 &&
            budget.snapshot().ownedNativeImageBytes == published.ownedNativeImageBytes &&
            budget.snapshot().ownedNonlocalNativeHostBytes ==
                published.ownedNonlocalNativeHostBytes,
        "foreign final release retains LUT/device/overlapping-host claims until native drain");
    device.reclaimReleasedImages();
    expectations.expect(device.ocioProgramRetirementStats().registeredPrograms == 0 &&
                            budget.snapshot().ownedNativeImages == 0 &&
                            budget.snapshot().ownedNativeImageBytes == 0 &&
                            budget.snapshot().ownedNonlocalNativeHostBytes == 0 &&
                            budget.snapshot().totalHostClaimBytes == 0 &&
                            budget.snapshot().totalDeviceClaimBytes == 0,
                        "actual owner LUT destruction releases all alias-backed claims");
    std::cout << "LUT_STORAGE textures=" << published.ownedNativeImages
              << " bytes=" << published.ownedNativeImageBytes << '\n';

    // The actual runtime program cache must promote LUTs, independently of direct aliases.
    {
        auto cacheUploader = GpuImageUpload::create(device);
        auto cacheInput = cacheUploader ? uploadImage(*cacheUploader.upload, geometry.width,
                                                       geometry.height,
                                                       fixturePixels(geometry.width, geometry.height))
                                         : nullptr;
        phase = budget.reserve({.deviceWorkingBytes = envelope});
        auto executor = GpuOcioProgramExecutor::create(device);
        if (!cacheInput || !phase.lease || !device.retainProcessingPhase(phase.lease) || !executor) {
            expectations.expect(false, "actual textured runtime cache prerequisites are ready");
            return;
        }
        const auto cacheRevision = budget.snapshot().admissionRevision;
        struct PublicationReset final {
            ~PublicationReset() {
                bloom::render::ocio_program_detail::setPublicationFailureAfterForTest(UINT32_MAX);
            }
        };
        bloom::runtime::GpuOcioExecutorDiagnostic rejected;
        {
            PublicationReset restore;
            bloom::render::ocio_program_detail::setPublicationFailureAfterForTest(0);
            rejected = executor.executor->begin(prepared.command, cacheInput, {}, kBudget);
        }
        const auto rolledBack = executor.executor->counters();
        expectations.expect(rejected.code == GpuOcioExecutorDiagnosticCode::DeviceUnavailable &&
                                rolledBack.cacheEntries == 0 && rolledBack.cacheBytes == 0 &&
                                rolledBack.programCreations == 0 &&
                                budget.snapshot().ownedNativeImages == 0 &&
                                budget.snapshot().admissionRevision == cacheRevision,
                            "post-emplace publication rejection rolls back actual LUT cache ownership");
        const auto runCached = [&] {
            const auto begun = executor.executor->begin(prepared.command, cacheInput, {}, kBudget);
            if (begun.code != GpuOcioExecutorDiagnosticCode::None)
                return false;
            auto poll = executor.executor->poll();
            while (poll == GpuOcioExecutorPollResult::Pending)
                poll = executor.executor->poll();
            if (poll != GpuOcioExecutorPollResult::Ready)
                return false;
            auto output = executor.executor->takeDisplayOutput();
            return output && output->isValid();
        };
        expectations.expect(runCached(), "actual textured runtime cache cold dispatch succeeds");
        const auto cold = budget.snapshot();
        const auto coldCounters = executor.executor->counters();
        const std::weak_ptr<bloom::render::GpuProcessingPhaseLease> cacheOrigin = phase.lease;
        device.retireProcessingPhase(phase.lease->id(), true);
        phase.lease.reset();
        expectations.expect(cacheOrigin.expired() && budget.snapshot().admittedPhases == 0 &&
                                cold.ownedNativeImages == prepared.command->program().textures.size() &&
                                cold.ownedNativeImageBytes > 0 &&
                                budget.snapshot().ownedNativeImageBytes == cold.ownedNativeImageBytes,
                            "runtime cache publication detaches the exact original phase");
        phase = budget.reserve({.deviceWorkingBytes = envelope});
        if (!phase.lease || !device.retainProcessingPhase(phase.lease)) {
            expectations.expect(false, "same-root new phase admits actual warm LUT cache use");
            return;
        }
        expectations.expect(runCached(), "actual textured runtime cache warm dispatch succeeds");
        const auto warm = budget.snapshot();
        const auto warmCounters = executor.executor->counters();
        expectations.expect(warm.ownedNativeImages == cold.ownedNativeImages &&
                                warm.ownedNativeImageBytes == cold.ownedNativeImageBytes &&
                                warm.ownedNonlocalNativeHostBytes == cold.ownedNonlocalNativeHostBytes &&
                                warmCounters.programCreations == coldCounters.programCreations &&
                                warmCounters.programReuses == coldCounters.programReuses + 1,
                            "warm runtime LUT cache adds no program or retained native claim");
        executor.executor.reset();
        device.retireProcessingPhase(phase.lease->id(), true);
        phase.lease.reset();
        expectations.expect(budget.snapshot().ownedNativeImageBytes == 0 &&
                                budget.snapshot().ownedNativeImages == 0 &&
                                budget.snapshot().totalHostClaimBytes == 0 &&
                                budget.snapshot().totalDeviceClaimBytes == 0,
                            "actual runtime LUT cache destruction returns all claims once");
        std::cout << "LUT_RUNTIME_CACHE textures=" << cold.ownedNativeImages
                  << " bytes=" << cold.ownedNativeImageBytes
                  << " warmNewPrograms=0 warmNewClaims=0 rollback=executed\n";
    }

    auto uploader = GpuImageUpload::create(device);
    if (!uploader) {
        expectations.expect(false, "the submitted LUT control uploader is ready");
        return;
    }
    auto input = uploadImage(*uploader.upload, geometry.width, geometry.height,
                             fixturePixels(geometry.width, geometry.height));
    phase = budget.reserve({.deviceWorkingBytes = envelope});
    if (!input || !phase.lease || !device.retainProcessingPhase(phase.lease)) {
        expectations.expect(false, "actual submitted LUT control prerequisites are ready");
        return;
    }
    made = bloom::render::GpuOcioProgram::create(device, prepared.command->program(), words, {}, {},
                                                 phase.lease);
    if (!made || !made.program->publishSampledStorage()) {
        expectations.expect(false, "the submitted control retains actual charged LUTs");
        return;
    }
    bloom::render::ocio_program_detail::setJobFenceUnknownForTest(true);
    const auto submitted = made.program->beginDisplay(input, {}, kBudget);
    expectations.expect(submitted.code == bloom::render::GpuOcioProgramDiagnosticCode::None &&
                            made.program->poll() ==
                                bloom::render::GpuOcioProgramPollResult::Failure &&
                            made.program->hasUnretiredSubmission(),
                        "a real LUT display submission produces an observed unknown fence failure");
    const auto unknownClaims = budget.snapshot();
    expectations.expect(unknownClaims.ownedNativeImageBytes > published.ownedNativeImageBytes,
                        "actual submitted output and LUT backing both remain charged");
    std::thread unknownForeign([held = std::move(made.program)]() mutable { held.reset(); });
    unknownForeign.join();
    expectations.expect(
        device.ocioProgramRetirementStats().pendingPrograms == 1 &&
            budget.snapshot().ownedNativeImageBytes == unknownClaims.ownedNativeImageBytes &&
            budget.snapshot().ownedNonlocalNativeHostBytes ==
                unknownClaims.ownedNonlocalNativeHostBytes &&
            budget.snapshot().unprovenPhases == 1,
        "submitted-unknown foreign handoff retains actual new tokens and sticky phase");
    bloom::render::ocio_program_detail::setJobFenceUnknownForTest(false);
    for (int attempt = 0; attempt < 200 && device.ocioProgramRetirementStats().pendingPrograms != 0;
         ++attempt) {
        device.reclaimReleasedImages();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expectations.expect(
        device.ocioProgramRetirementStats().registeredPrograms == 0 &&
            budget.snapshot().ownedNativeImageBytes == 0 &&
            budget.snapshot().ownedNativeImages == 0 && budget.snapshot().unprovenPhases == 1,
        "actual owner fence/backing destruction removes tokens but preserves sticky authority");
    phase.lease.reset();
    input.reset();
    uploader.upload.reset();
    madeDevice.device.reset();
    expectations.expect(
        budget.snapshot().admittedPhases == 0 && budget.snapshot().unprovenPhases == 0 &&
            budget.snapshot().totalHostClaimBytes == 0 &&
            budget.snapshot().totalDeviceClaimBytes == 0,
        "actual final generation destruction releases the independently sticky phase");
}
