// Test group for render_injected_provider_test.cpp: provider reuse/retirement, the genuine native
// headless PNG render, and the genuine native PNG sequence range. Split from the original file;
// the native routes now exercise the production packaged resolver so a CPU output-colour omission
// fails the run instead of producing a green proof.

// True when the GPU and CPU decoded PNGs agree: RGB within one 8-bit code, alpha exactly.
[[nodiscard]] bool pngsMatch(const IndependentPngDecode& gpu, const IndependentPngDecode& cpu) {
    if (!gpu.ok || !cpu.ok || gpu.width != cpu.width || gpu.height != cpu.height ||
        gpu.rgba.size() != cpu.rgba.size()) {
        return false;
    }
    for (std::size_t index = 0; index < cpu.rgba.size(); ++index) {
        const int difference =
            std::abs(static_cast<int>(cpu.rgba[index]) - static_cast<int>(gpu.rgba[index]));
        const bool alpha = index % 4U == 3U;
        if ((alpha && difference != 0) || (!alpha && difference > 1)) {
            return false;
        }
    }
    return true;
}

void testInjectedProviderIsReusedAndNotRetired(Expectations& expectations) {
    auto created = scripting::Session::createNew("Injected Provider", "Composition");
    expectations.expect(static_cast<bool>(created), "session is created");
    if (!created) {
        return;
    }
    auto session = std::move(created).takeSession();
    const auto snapshot = session->snapshot();
    expectations.expect(!snapshot.project().compositions().empty(),
                        "the default project has a composition");
    if (snapshot.project().compositions().empty()) {
        return;
    }
    TempDirectory directory;
    expectations.expect(directory.isValid(), "temp directory is available");
    if (!directory.isValid()) {
        return;
    }

    runtime::TaskScheduler scheduler;
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    auto provider =
        host::GpuExportProvider::create(optionsFor("/nonexistent/bloom/test/vulkan/loader.so"));
    provider->prepare(scheduler);
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expectations.expect(provider->prepared(), "injected provider bootstraps");
    const auto evaluator = provider->evaluator();
    expectations.expect(evaluator != nullptr,
                        "injected provider publishes an (unavailable) evaluator");
    expectations.expect(!provider->retirementComplete(),
                        "injected provider is live before the render");

    const auto compositionId = snapshot.project().compositions().front().id();
    for (int pass = 0; pass < 2; ++pass) {
        const auto target = directory.path() / ("injected-" + std::to_string(pass) + ".exr");
        const auto result = scripting::Render::run(
            *session, scheduler, compiler,
            {.composition = compositionId,
             .frame = std::uint64_t{0},
             .range = std::nullopt,
             .preset = bloom::output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
             .destination = target},
            {}, provider);
        expectations.expect(result.succeeded, "the injected-provider render publishes");
        expectations.expect(!provider->retirementComplete(),
                            "Render::run never retires an injected provider between stills");
        expectations.expect(provider->evaluator() != nullptr,
                            "the injected evaluator is still published after a still");
    }

    expectations.expect(provider->shutdownAndWait(std::chrono::seconds(10)),
                        "the owner can still retire the injected provider explicitly");
}

void testLocalProviderIsRetired(Expectations& expectations) {
    auto created = scripting::Session::createNew("Local Provider", "Composition");
    expectations.expect(static_cast<bool>(created), "local session is created");
    if (!created) {
        return;
    }
    auto session = std::move(created).takeSession();
    const auto snapshot = session->snapshot();
    if (snapshot.project().compositions().empty()) {
        return;
    }
    TempDirectory directory;
    if (!directory.isValid()) {
        return;
    }
    runtime::TaskScheduler scheduler;
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto result = scripting::Render::run(
        *session, scheduler, compiler,
        {.composition = snapshot.project().compositions().front().id(),
         .frame = std::uint64_t{0},
         .range = std::nullopt,
         .preset = bloom::output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
         .destination = directory.path() / "local.exr"});
    expectations.expect(result.succeeded,
                        "a locally owned provider still renders and retires with proof");
}

// Real headless render integration over the production Render::run: at least two PNG frames through
// an injected provider prepared with the production packaged resolver, positive genuine GPU
// counters, the ACTUAL combined-readback one-submission/two-payload (process + encoded display)
// provenance per frame, strict CPU/GPU decoded-PNG parity against a disabled-provider CPU
// reference, and the repeated provider still live. Requires BLOOM_TEST_VULKAN_LOADER (an absolute
// loader path); skips honestly otherwise.
GpuProofOutcome testNativeHeadlessRenderProvenanceAndParity(Expectations& expectations,
                                                            const bool requireDevice,
                                                            const std::filesystem::path& proofDir) {
    const auto skip = [&proofDir] {
        return proofDir.empty() ? GpuProofOutcome::NotRequested : GpuProofOutcome::Skipped;
    };
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        if (requireDevice) {
            expectations.expect(false,
                                "native render: --require-device needs BLOOM_TEST_VULKAN_LOADER");
        } else {
            std::cout << "NOTE: BLOOM_TEST_VULKAN_LOADER unset; skipping native headless render\n";
        }
        return skip();
    }
    auto session = buildSolidSession(expectations);
    if (session == nullptr) {
        return GpuProofOutcome::Ran;
    }
    TempDirectory directory;
    expectations.expect(directory.isValid(), "native render: temp directory is available");
    if (!directory.isValid()) {
        return GpuProofOutcome::Ran;
    }
    runtime::TaskScheduler scheduler;
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    auto provider = host::GpuExportProvider::create(optionsFor(loader));
    provider->prepare(scheduler);
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!provider->deviceAvailable()) {
        expectations.expect(!requireDevice, "native render: a device is required");
        std::cout << "NOTE: no compatible Vulkan device; skipping native headless render\n";
        return skip();
    }
    if (!provider->gpuDisplayPreparationAvailable()) {
        // Without the qualified packaged tools the genuine GPU output-colour (two-payload) arm
        // cannot be prepared, so this route must not claim a proof; --require-device still demands
        // the whole qualified environment.
        expectations.expect(!requireDevice,
                            "native render: the production packaged resolver must qualify the "
                            "display route");
        if (!requireDevice) {
            std::cout << "NOTE: packaged GPU shader tools unavailable; skipping native headless "
                         "render\n";
            return skip();
        }
        return GpuProofOutcome::Ran;
    }
    auto cpuProvider = host::GpuExportProvider::create(disabledOptions());
    cpuProvider->prepare(scheduler);
    const auto snapshot = session->snapshot();
    const auto compositionId = snapshot.project().compositions().front().id();
    const auto* composition = snapshot.project().findComposition(compositionId);
    const auto planResult =
        compiler.compile({.snapshot = snapshot, .compositionId = compositionId}, {});
    expectations.expect(planResult.plan != nullptr,
                        "native render: the plan compiles for identity");
    std::vector<std::string> identityHex;
    std::vector<routeproof::FrameEvidence> evidence;
    scripting::RenderResult proofAggregate;
    for (const std::uint64_t frame : {std::uint64_t{0}, std::uint64_t{1}}) {
        const auto gpuPath = directory.path() / ("gpu-" + std::to_string(frame) + ".png");
        const auto cpuPath = directory.path() / ("cpu-" + std::to_string(frame) + ".png");
        const auto gpu =
            scripting::Render::run(*session, scheduler, compiler,
                                   {.composition = compositionId,
                                    .frame = frame,
                                    .range = std::nullopt,
                                    .preset = bloom::output::OutputPresetV1::PngRgba8SrgbV1,
                                    .destination = gpuPath},
                                   {}, provider);
        expectations.expect(gpu.succeeded, "native render: the GPU frame publishes");
        expectations.expect(gpu.gpuEvaluatedFrames == 1 && gpu.gpuNativeDispatches > 0 &&
                                gpu.gpuReadbacks == 1 && gpu.gpuReadbackSubmissions == 1,
                            "native render: genuine positive GPU counters for the frame");
        expectations.expect(gpu.gpuTransferredPayloads == 2 && gpu.gpuProcessPayloadBytes > 0 &&
                                gpu.gpuEncodedPayloadBytes > 0,
                            "native render: the PNG frame transfers one submission carrying the "
                            "process and encoded display payloads");
        expectations.expect(gpu.gpuDeviceOwnershipEpoch > 0,
                            "native render: a genuine device ownership epoch is reported");
        const auto cpu =
            scripting::Render::run(*session, scheduler, compiler,
                                   {.composition = compositionId,
                                    .frame = frame,
                                    .range = std::nullopt,
                                    .preset = bloom::output::OutputPresetV1::PngRgba8SrgbV1,
                                    .destination = cpuPath},
                                   {}, cpuProvider);
        expectations.expect(cpu.succeeded && cpu.gpuEvaluatedFrames == 0 &&
                                cpu.gpuDeviceOwnershipEpoch == 0 &&
                                cpu.gpuReadbackSubmissions == 0 &&
                                cpu.gpuTransferredPayloads == 0 && cpu.gpuEncodedPayloadBytes == 0,
                            "native render: the disabled provider is an honest CPU reference");
        const auto gpuImage = independentlyDecodePng(gpuPath);
        const auto cpuImage = independentlyDecodePng(cpuPath);
        expectations.expect(gpuImage.ok && cpuImage.ok && gpuImage.width == cpuImage.width &&
                                gpuImage.height == cpuImage.height &&
                                gpuImage.rgba.size() == cpuImage.rgba.size(),
                            "native render: both published PNGs independently decode with matching "
                            "descriptors");
        expectations.expect(pngsMatch(gpuImage, cpuImage),
                            "native render: GPU/CPU decoded PNG pixels match within one code with "
                            "exact alpha");
        if (planResult.plan != nullptr && composition != nullptr) {
            const auto time = host::FrameRangeRunnerV1::timeForFrame(
                {.destination = gpuPath,
                 .firstFrame = frame,
                 .lastFrame = frame,
                 .frameRate = composition->format().frameRate(),
                 .duration = composition->duration()},
                frame);
            if (time.has_value()) {
                FrameIdentityFields fields;
                fields.routeId = "route.export.headless_scripted";
                fields.projectId = planResult.plan->projectId().value();
                fields.compositionId = planResult.plan->compositionId().value();
                fields.sourceRevision = planResult.plan->sourceRevision().value();
                fields.outputIndex = planResult.plan->output().value();
                fields.operationCount = planResult.plan->operations().size();
                fields.planSemantics = planResult.plan->planSemanticsVersion();
                fields.animationSamplingSemantics =
                    planResult.plan->animationSamplingSemanticsVersion();
                fields.frameIndex = frame;
                fields.timeNumerator = time->numerator();
                fields.timeDenominator = time->denominator();
                fields.preset =
                    static_cast<std::uint64_t>(bloom::output::OutputPresetV1::PngRgba8SrgbV1);
                fields.provider = "gpu-resident";
                const auto identity = frameIdentityHex(fields);
                identityHex.push_back(identity);
                evidence.push_back(comparePngEvidence(gpuImage, cpuImage, identity));
            }
        }
        proofAggregate.gpuEvaluatedFrames += gpu.gpuEvaluatedFrames;
        proofAggregate.gpuNativeDispatches += gpu.gpuNativeDispatches;
        proofAggregate.gpuReadbacks += gpu.gpuReadbacks;
        proofAggregate.gpuReadbackSubmissions += gpu.gpuReadbackSubmissions;
        proofAggregate.gpuTransferredPayloads += gpu.gpuTransferredPayloads;
        proofAggregate.gpuProcessPayloadBytes += gpu.gpuProcessPayloadBytes;
        proofAggregate.gpuEncodedPayloadBytes += gpu.gpuEncodedPayloadBytes;
        if (gpu.gpuDeviceOwnershipEpoch != 0) {
            proofAggregate.gpuDeviceOwnershipEpoch = gpu.gpuDeviceOwnershipEpoch;
        }
    }
    expectations.expect(!provider->retirementComplete() && provider->evaluator() != nullptr,
                        "native render: the repeated injected provider stays live");
    expectations.expect(provider->shutdownAndWait(std::chrono::seconds(10)),
                        "native render: the provider retires with completion proof");
    if (expectations.failures() == 0 && identityHex.size() == 2) {
        publishScriptedProof(expectations, proofDir, "route.export.headless_scripted",
                             bloom::runtime::GpuRouteHarnessKind::HeadlessScripted, proofAggregate,
                             identityHex, evidence);
    }
    return GpuProofOutcome::Ran;
}

// Real sequence/range integration over the production Render::run range mode: the real
// FrameRangeRunner publishes numbered PNG frames through the same per-frame output attempt path
// with the production packaged resolver, and every frame is verified against an independent
// disabled-provider CPU export. A device is required; without one this returns Skipped.
GpuProofOutcome testNativeSequenceRangeProvenanceAndParity(Expectations& expectations,
                                                           const bool requireDevice,
                                                           const std::filesystem::path& proofDir) {
    const auto skip = [&proofDir] {
        return proofDir.empty() ? GpuProofOutcome::NotRequested : GpuProofOutcome::Skipped;
    };
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        if (requireDevice) {
            expectations.expect(
                false, "native sequence range: --require-device needs BLOOM_TEST_VULKAN_LOADER");
        }
        return skip();
    }
    auto session = buildSolidSession(expectations);
    if (session == nullptr) {
        return GpuProofOutcome::Ran;
    }
    TempDirectory directory;
    expectations.expect(directory.isValid(), "native sequence range: temp directory is available");
    if (!directory.isValid()) {
        return GpuProofOutcome::Ran;
    }
    runtime::TaskScheduler scheduler;
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    auto provider = host::GpuExportProvider::create(optionsFor(loader));
    provider->prepare(scheduler);
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!provider->deviceAvailable()) {
        expectations.expect(!requireDevice, "native sequence range: a device is required");
        return skip();
    }
    if (!provider->gpuDisplayPreparationAvailable()) {
        expectations.expect(!requireDevice,
                            "native sequence range: the production packaged resolver must qualify "
                            "the display route");
        if (!requireDevice) {
            std::cout << "NOTE: packaged GPU shader tools unavailable; skipping native sequence "
                         "range\n";
            return skip();
        }
        return GpuProofOutcome::Ran;
    }
    auto cpuProvider = host::GpuExportProvider::create(disabledOptions());
    cpuProvider->prepare(scheduler);
    const auto snapshot = session->snapshot();
    const auto compositionId = snapshot.project().compositions().front().id();
    const auto* composition = snapshot.project().findComposition(compositionId);
    expectations.expect(composition != nullptr, "native sequence range: the composition exists");
    if (composition == nullptr) {
        return GpuProofOutcome::Ran;
    }
    const auto planResult =
        compiler.compile({.snapshot = snapshot, .compositionId = compositionId}, {});
    expectations.expect(planResult.plan != nullptr,
                        "native sequence range: the plan compiles for identity");
    constexpr std::uint64_t kFirstFrame = 0;
    constexpr std::uint64_t kLastFrame = 2;
    const auto gpuBase = directory.path() / "gpu-sequence.png";
    const auto cpuBase = directory.path() / "cpu-sequence.png";
    const auto gpuRange =
        scripting::Render::run(*session, scheduler, compiler,
                               {.composition = compositionId,
                                .frame = std::nullopt,
                                .range = std::make_pair(kFirstFrame, kLastFrame),
                                .preset = bloom::output::OutputPresetV1::PngRgba8SrgbV1,
                                .destination = gpuBase},
                               {}, provider);
    expectations.expect(gpuRange.succeeded && gpuRange.publishedFrames == 3,
                        "native sequence range: the GPU range publishes three frames");
    const auto cpuRange =
        scripting::Render::run(*session, scheduler, compiler,
                               {.composition = compositionId,
                                .frame = std::nullopt,
                                .range = std::make_pair(kFirstFrame, kLastFrame),
                                .preset = bloom::output::OutputPresetV1::PngRgba8SrgbV1,
                                .destination = cpuBase},
                               {}, cpuProvider);
    expectations.expect(
        cpuRange.succeeded && cpuRange.publishedFrames == 3 && cpuRange.gpuEvaluatedFrames == 0 &&
            cpuRange.gpuReadbackSubmissions == 0 && cpuRange.gpuTransferredPayloads == 0 &&
            cpuRange.gpuEncodedPayloadBytes == 0,
        "native sequence range: the disabled provider is an honest CPU reference");
    expectations.expect(
        gpuRange.gpuEvaluatedFrames == 3 && gpuRange.gpuNativeDispatches > 0 &&
            gpuRange.gpuReadbacks == 3 && gpuRange.gpuReadbackSubmissions == 3 &&
            gpuRange.gpuTransferredPayloads == 6 && gpuRange.gpuProcessPayloadBytes > 0 &&
            gpuRange.gpuEncodedPayloadBytes > 0 && gpuRange.gpuDeviceOwnershipEpoch > 0,
        "native sequence range: genuine per-frame GPU counters with the GPU "
        "display-colour arm");
    std::vector<std::string> identityHex;
    std::vector<routeproof::FrameEvidence> evidence;
    for (std::uint64_t frame = kFirstFrame; frame <= kLastFrame; ++frame) {
        const auto time =
            host::FrameRangeRunnerV1::timeForFrame({.destination = gpuBase,
                                                    .firstFrame = kFirstFrame,
                                                    .lastFrame = kLastFrame,
                                                    .frameRate = composition->format().frameRate(),
                                                    .duration = composition->duration()},
                                                   frame);
        const auto gpuPath =
            host::FrameRangeRunnerV1::sequenceFramePath(gpuBase, frame, kLastFrame);
        const auto cpuPath =
            host::FrameRangeRunnerV1::sequenceFramePath(cpuBase, frame, kLastFrame);
        const auto gpuImage = independentlyDecodePng(gpuPath);
        const auto cpuImage = independentlyDecodePng(cpuPath);
        expectations.expect(gpuImage.ok && cpuImage.ok && gpuImage.width == cpuImage.width &&
                                gpuImage.height == cpuImage.height &&
                                gpuImage.rgba.size() == cpuImage.rgba.size(),
                            "native sequence range: both frame PNGs reopen with matching "
                            "descriptors");
        expectations.expect(pngsMatch(gpuImage, cpuImage),
                            "native sequence range: GPU frame matches the CPU reference within "
                            "one code with exact alpha");
        if (planResult.plan != nullptr && composition != nullptr && time.has_value()) {
            FrameIdentityFields fields;
            fields.routeId = "route.export.sequence_range";
            fields.projectId = planResult.plan->projectId().value();
            fields.compositionId = planResult.plan->compositionId().value();
            fields.sourceRevision = planResult.plan->sourceRevision().value();
            fields.outputIndex = planResult.plan->output().value();
            fields.operationCount = planResult.plan->operations().size();
            fields.planSemantics = planResult.plan->planSemanticsVersion();
            fields.animationSamplingSemantics =
                planResult.plan->animationSamplingSemanticsVersion();
            fields.frameIndex = frame;
            fields.timeNumerator = time->numerator();
            fields.timeDenominator = time->denominator();
            fields.preset =
                static_cast<std::uint64_t>(bloom::output::OutputPresetV1::PngRgba8SrgbV1);
            fields.provider = "gpu-resident";
            const auto identity = frameIdentityHex(fields);
            identityHex.push_back(identity);
            evidence.push_back(comparePngEvidence(gpuImage, cpuImage, identity));
        }
    }
    expectations.expect(provider->shutdownAndWait(std::chrono::seconds(10)),
                        "native sequence range: the provider retires with completion proof");
    if (expectations.failures() == 0 && identityHex.size() == 3) {
        publishScriptedProof(expectations, proofDir, "route.export.sequence_range",
                             bloom::runtime::GpuRouteHarnessKind::SequenceRangeExport, gpuRange,
                             identityHex, evidence);
    }
    return GpuProofOutcome::Ran;
}
