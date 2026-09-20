// GPU publication group: command-refusal negatives and the still-frame route proof. Split from
// frame_export_publication_tests.cpp. The still proof now records the ACTUAL GPU output-colour
// (PNG) arm's combined-readback provenance and evidence, while the EXR parity/reopen checks remain
// unchanged.

// Native GPU-composited publication parity. Requires a real loader/device
// (BLOOM_TEST_VULKAN_LOADER, an absolute path) and skips otherwise unless --require-device is
// passed. It builds a CPU reference attempt and GPU attempts over the identical plan/request,
// proves the retained process images agree within the frozen 2e-6 finite-component tolerance with
// exact alpha, then publishes the GPU attempt as EXR and both attempts as PNG, independently
// reopen-verifying the EXR and independently decoding both PNGs to compare the RGBA8 bytes (RGB
// within one code, alpha exact) against the unchanged CPU reference.
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
// Real provider with a qualified device and packaged shader tools, ready to prepare display
// commands. Returns null (with a NOTE) when the loader/device is unavailable, exactly like the
// existing GPU publication tests.
[[nodiscard]] std::shared_ptr<host::GpuExportProvider>
makeGpuDisplayTestProvider(ExportFixture& fixture, const std::string_view context) {
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        std::cout << "NOTE: BLOOM_TEST_VULKAN_LOADER unset; skipping " << context << '\n';
        return nullptr;
    }
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = true;
    options.loaderPath = std::filesystem::path(loader);
    auto provider = host::GpuExportProvider::create(options);
    provider->prepare(fixture.scheduler());
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + 30s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    if (!provider->deviceAvailable()) {
        std::cout << "NOTE: no compatible Vulkan device; skipping " << context << '\n';
        return nullptr;
    }
    runtime::GpuOcioCompileOptions displayCompile;
    displayCompile.glslangValidatorPath =
        std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    displayCompile.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    provider->setGpuDisplayCompileOptions(std::move(displayCompile));
    return provider;
}

// Runs one PNG attempt carrying a deliberately-refused `request.outputColorCommand` and proves the
// runner never launders it: no GPU display payload is retained, the retained frame and provenance
// are the honest CPU reference path, and the CPU display products still publish a valid PNG.
void expectGpuDisplayCommandRefused(
    Expectations& expectations, ExportFixture& fixture,
    const std::shared_ptr<host::GpuExportProvider>& provider,
    const std::shared_ptr<const runtime::PreparedGpuOcioCommand>& refused,
    const std::string_view context) {
    const auto target = fixture.path() / (std::string(context) + "-refused.png");
    auto request = attemptRequestFor(target, output::OutputPresetV1::PngRgba8SrgbV1);
    request.gpuProvider = provider;
    request.outputColorCommand = refused;
    auto begin = host::beginOutputAnalysisAttemptV1(fixture.scheduler(), fixture.artifacts(),
                                                    fixture.ledger(), std::move(request));
    expectations.expect(static_cast<bool>(begin), "gpu command refusal: the attempt begins");
    if (!begin) {
        return;
    }
    auto runner = std::move(begin).takeHandle();
    std::optional<host::OutputAnalysisAttemptOutcomeV1> outcome;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        outcome = runner.tryComplete();
        if (outcome.has_value()) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(outcome.has_value() && static_cast<bool>(*outcome),
                        "gpu command refusal: the mismatched command is refused and the attempt "
                        "falls back to the honest CPU path");
    if (!outcome.has_value() || !*outcome) {
        return;
    }
    expectations.expect(!(*outcome).attempt()->gpuDisplay().isPresent(),
                        "gpu command refusal: no GPU display payload is retained");
    expectations.expect((*outcome).attempt()->frame() != nullptr &&
                            (*outcome).attempt()->frame()->identity().provider ==
                                runtime::EvaluationProvider::CpuReference,
                        "gpu command refusal: the retained frame is the CPU reference");
    const auto& provenance = (*outcome).gpuProvenance();
    expectations.expect(provenance.has_value() && !provenance->gpuEvaluated() &&
                            provenance->encodedArm == runtime::GpuOutputColorArm::None &&
                            provenance->transferredPayloads == 0,
                        "gpu command refusal: the GPU arm is never reported as evaluated");
    auto approval = host::approveFrameExportV1(fixture.coordinator(), (*outcome).attempt(),
                                               requireDigest((*outcome).attempt(), expectations));
    if (!approval) {
        return;
    }
    auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                              std::move(approval).takeRequest(), fixture.path());
    if (!run.has_value()) {
        return;
    }
    const auto resultOpt = awaitExportRun(*run);
    expectations.expect(resultOpt.has_value() && static_cast<bool>(*resultOpt) &&
                            std::filesystem::exists(target),
                        "gpu command refusal: the honest CPU fallback publishes the PNG");
}
#endif

// A command prepared for the SAME geometry but a WRONG transform (a process-effect CST rather than
// the resolved display transform) must be refused. The runner accepts a supplied command only when
// its canonical identity byte-equals the command it prepared from the exact resolved
// config/working space/display/view and geometry.
void testGpuSameGeometryWrongTransformRefused(Expectations& expectations) {
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "wrong transform: fixture is available")) {
        return;
    }
    auto provider = makeGpuDisplayTestProvider(fixture, "wrong transform refusal");
    if (provider == nullptr) {
        return;
    }
    auto resolution = bloom::color::resolveOcioBuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest, runtime::kLinearRec709SceneColorSpaceId);
    expectations.expect(resolution.ready(),
                        "wrong transform: the built-in neutral config resolves");
    if (!resolution.ready()) {
        static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
        return;
    }
    auto config = std::move(resolution).takeResolved();
    expectations.expect(config.has_value(), "wrong transform: the resolved config is taken");
    if (!config.has_value()) {
        static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
        return;
    }
    runtime::GpuOcioCompileOptions compile;
    compile.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    compile.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    runtime::GpuOcioProgramPreparer preparer(runtime::GpuOcioPreparerBudgets{});
    runtime::GpuOcioTransformSpec spec;
    spec.kind = runtime::GpuOcioTransformKind::Cst;
    spec.fromId = std::string(config->processColorSpaceId());
    spec.toId = std::string(config->outputColorSpaceId());
    auto prepared = preparer.prepare(*config, spec, runtime::GpuOcioCommandGeometry{2, 2}, compile);
    expectations.expect(prepared.hasValue(),
                        "wrong transform: the process-effect command itself prepares with the "
                        "same geometry");
    if (prepared.hasValue()) {
        expectations.expect(
            prepared.command->encoding() == runtime::GpuOcioOutputEncoding::FinalRgba32f,
            "wrong transform: the injected command is a process-effect (not display) command");
        expectGpuDisplayCommandRefused(expectations, fixture, provider, prepared.command,
                                       "wrong transform");
    }
    static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
#else
    static_cast<void>(expectations);
#endif
}

// A command prepared for a DIFFERENT config revision / frame (the ACEScg built-in rather than the
// resolved Bloom Neutral config) with the same geometry must be refused: its canonical identity
// differs from the command the runner prepared from the exact resolved config.
void testGpuCrossRevisionCommandRefused(Expectations& expectations) {
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "cross revision: fixture is available")) {
        return;
    }
    auto provider = makeGpuDisplayTestProvider(fixture, "cross revision refusal");
    if (provider == nullptr) {
        return;
    }
    const auto acesRevision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    expectations.expect(acesRevision.has_value(), "cross revision: the ACES revision resolves");
    if (!acesRevision.has_value()) {
        static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
        return;
    }
    auto resolution =
        bloom::color::resolveOcioBuiltIn(bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                                         bloom::color::kAcesCgV1ConfigUri, *acesRevision, "ACEScg");
    expectations.expect(resolution.ready(), "cross revision: the ACES config resolves");
    if (!resolution.ready()) {
        static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
        return;
    }
    auto aces = std::move(resolution).takeResolved();
    expectations.expect(aces.has_value(), "cross revision: the ACES config is taken");
    if (!aces.has_value()) {
        static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
        return;
    }
    auto prepared = provider->prepareGpuDisplayCommand(*aces, aces->displayName(), aces->viewName(),
                                                       runtime::GpuOcioCommandGeometry{2, 2});
    expectations.expect(prepared.hasValue(),
                        "cross revision: the ACES display command itself prepares with the same "
                        "geometry");
    if (prepared.hasValue()) {
        expectGpuDisplayCommandRefused(expectations, fixture, provider, prepared.command,
                                       "cross revision");
    }
    static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
#else
    static_cast<void>(expectations);
#endif
}

// A stale/mismatched GPU display command (prepared for the wrong geometry) is refused, never
// silently used: the evaluator's combined readback rejects the geometry binding and the attempt
// fails typed at Evaluating. The honest CPU display path is the no-command case, covered by the
// existing CPU-only PNG publication tests.
void testGpuStaleDisplayBindingRefused(Expectations& expectations) {
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        std::cout << "NOTE: BLOOM_TEST_VULKAN_LOADER unset; skipping stale binding refusal\n";
        return;
    }
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "stale binding: fixture is available")) {
        return;
    }
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = true;
    options.loaderPath = std::filesystem::path(loader);
    auto provider = host::GpuExportProvider::create(options);
    provider->prepare(fixture.scheduler());
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + 30s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    if (!provider->deviceAvailable()) {
        std::cout << "NOTE: no compatible Vulkan device; skipping stale binding refusal\n";
        return;
    }
    runtime::GpuOcioCompileOptions displayCompile;
    displayCompile.glslangValidatorPath =
        std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    displayCompile.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    provider->setGpuDisplayCompileOptions(std::move(displayCompile));

    auto resolution = bloom::color::resolveOcioBuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest, runtime::kLinearRec709SceneColorSpaceId);
    if (!resolution.ready()) {
        return;
    }
    auto config = std::move(resolution).takeResolved();
    if (!config.has_value()) {
        return;
    }
    // Deliberately wrong geometry for the 2x2 fixture plan.
    auto prepared = provider->prepareGpuDisplayCommand(
        *config, config->displayName(), config->viewName(), runtime::GpuOcioCommandGeometry{1, 1});
    expectations.expect(prepared.hasValue(), "stale binding: the stale command itself prepares");
    if (!prepared.hasValue()) {
        return;
    }
    const auto target = fixture.path() / "stale.png";
    auto request = attemptRequestFor(target, output::OutputPresetV1::PngRgba8SrgbV1);
    request.gpuProvider = provider;
    request.outputColorCommand = prepared.command;
    auto begin = host::beginOutputAnalysisAttemptV1(fixture.scheduler(), fixture.artifacts(),
                                                    fixture.ledger(), std::move(request));
    expectations.expect(static_cast<bool>(begin), "stale binding: the attempt begins");
    if (!begin) {
        return;
    }
    auto runner = std::move(begin).takeHandle();
    std::optional<host::OutputAnalysisAttemptOutcomeV1> outcome;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        outcome = runner.tryComplete();
        if (outcome.has_value()) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(outcome.has_value() && static_cast<bool>(*outcome),
                        "stale binding: the mismatched command is refused and the attempt falls "
                        "back to the honest CPU display path");
    if (!outcome.has_value() || !*outcome) {
        static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
        return;
    }
    expectations.expect(!(*outcome).attempt()->gpuDisplay().isPresent(),
                        "stale binding: no GPU display payload is retained");
    const auto& provenance = (*outcome).gpuProvenance();
    expectations.expect(provenance.has_value() && !provenance->gpuEvaluated(),
                        "stale binding: the GPU arm is never reported as evaluated");
    // The CPU fallback still publishes a valid PNG (the honest retained display products).
    auto approval = host::approveFrameExportV1(fixture.coordinator(), (*outcome).attempt(),
                                               requireDigest((*outcome).attempt(), expectations));
    if (approval) {
        auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                                  std::move(approval).takeRequest(), fixture.path());
        if (run.has_value()) {
            const auto resultOpt = awaitExportRun(*run);
            expectations.expect(resultOpt.has_value() && static_cast<bool>(*resultOpt) &&
                                    std::filesystem::exists(target),
                                "stale binding: the CPU fallback publishes the PNG");
        }
    }
    static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
#else
    static_cast<void>(expectations);
#endif
}

//
// When `proofDirectory` is non-empty this same real run also publishes the genuine
// `route.export.still_frame` proof, but only after every assertion above passed: the proof carries
// the ACTUAL GPU output-colour (PNG) arm's canonical process identity, its real single combined
// readback submission and two payloads (process-analysis + encoded display), and an evidence digest
// bound to the actual GPU/CPU process comparison and the independently decoded PNG bytes. An
// absent loader/device returns Skipped so the proof CTest reports 77, never a pass.
enum class GpuProofOutcome : std::uint8_t { NotRequested, Skipped, Ran };

GpuProofOutcome testGpuCompositedExportParity(Expectations& expectations, const bool requireDevice,
                                              const std::filesystem::path& proofDirectory) {
    const auto skip = [&proofDirectory] {
        return proofDirectory.empty() ? GpuProofOutcome::NotRequested : GpuProofOutcome::Skipped;
    };
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        if (requireDevice) {
            expectations.expect(
                false, "gpu parity: --require-device needs BLOOM_TEST_VULKAN_LOADER absolute");
        } else {
            std::cout << "NOTE: BLOOM_TEST_VULKAN_LOADER unset; skipping GPU publication parity\n";
        }
        return skip();
    }
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "gpu parity: fixture is available")) {
        return GpuProofOutcome::Ran;
    }
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = true;
    options.loaderPath = std::filesystem::path(loader);
    auto provider = host::GpuExportProvider::create(options);
    provider->prepare(fixture.scheduler());
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + 30s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    if (!provider->deviceAvailable()) {
        expectations.expect(!requireDevice, "gpu parity: a device is required");
        std::cout << "NOTE: no compatible Vulkan device; skipping GPU publication parity\n";
        return skip();
    }
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
    // Qualified packaged shader tools: enables the real GPU DisplayRgba8 PNG route (the CPU display
    // path remains the fallback when these are unavailable).
    runtime::GpuOcioCompileOptions displayCompile;
    displayCompile.glslangValidatorPath =
        std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    displayCompile.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    provider->setGpuDisplayCompileOptions(std::move(displayCompile));
#endif

    const auto buildAttempt =
        [&](const std::filesystem::path& target, const output::OutputPresetV1 preset,
            const std::shared_ptr<host::GpuExportProvider>& gpu,
            std::shared_ptr<const output::OutputAnalysisAttemptV1>& attempt,
            std::optional<host::OutputAnalysisAttemptGpuProvenanceV1>& provenance) -> bool {
        auto request = attemptRequestFor(target, preset);
        request.gpuProvider = gpu;
        auto begin = host::beginOutputAnalysisAttemptV1(fixture.scheduler(), fixture.artifacts(),
                                                        fixture.ledger(), std::move(request));
        if (!begin) {
            return false;
        }
        auto runner = std::move(begin).takeHandle();
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (std::chrono::steady_clock::now() < deadline) {
            auto outcome = runner.tryComplete();
            if (!outcome.has_value()) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            if (!*outcome) {
                return false;
            }
            attempt = outcome->attempt();
            provenance = outcome->gpuProvenance();
            return attempt != nullptr;
        }
        return false;
    };
    const auto publish = [&](const std::shared_ptr<const output::OutputAnalysisAttemptV1>& attempt,
                             const std::filesystem::path& target) -> bool {
        auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                                   requireDigest(attempt, expectations));
        if (!approval) {
            return false;
        }
        auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                                  std::move(approval).takeRequest(), fixture.path());
        if (!run.has_value()) {
            return false;
        }
        auto resultOpt = awaitExportRun(*run);
        return resultOpt.has_value() && static_cast<bool>(*resultOpt) &&
               resultOpt->publication() != nullptr &&
               resultOpt->publication()->outcome ==
                   platform::StagedArtifactPublicationOutcome::Published &&
               std::filesystem::exists(target);
    };

    std::shared_ptr<const output::OutputAnalysisAttemptV1> cpuAttempt;
    std::shared_ptr<const output::OutputAnalysisAttemptV1> gpuExrAttempt;
    std::shared_ptr<const output::OutputAnalysisAttemptV1> gpuPngAttempt;
    std::optional<host::OutputAnalysisAttemptGpuProvenanceV1> cpuProvenance;
    std::optional<host::OutputAnalysisAttemptGpuProvenanceV1> gpuExrProvenance;
    std::optional<host::OutputAnalysisAttemptGpuProvenanceV1> gpuPngProvenance;
    const auto cpuPngTarget = fixture.path() / "parity-cpu.png";
    const auto gpuExrTarget = fixture.path() / "parity-gpu.exr";
    const auto gpuPngTarget = fixture.path() / "parity-gpu.png";
    if (!buildAttempt(cpuPngTarget, output::OutputPresetV1::PngRgba8SrgbV1, nullptr, cpuAttempt,
                      cpuProvenance) ||
        !buildAttempt(gpuExrTarget, output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
                      provider, gpuExrAttempt, gpuExrProvenance) ||
        !buildAttempt(gpuPngTarget, output::OutputPresetV1::PngRgba8SrgbV1, provider, gpuPngAttempt,
                      gpuPngProvenance)) {
        expectations.expect(false, "gpu parity: the CPU/GPU attempts complete");
        return GpuProofOutcome::Ran;
    }
    expectations.expect(
        gpuExrAttempt->frame()->identity().provider == runtime::EvaluationProvider::GpuResident &&
            gpuPngAttempt->frame()->identity().provider == runtime::EvaluationProvider::GpuResident,
        "gpu parity: both GPU attempts retain GpuResident provenance");
    expectations.expect(gpuExrProvenance.has_value() && gpuExrProvenance->gpuEvaluated() &&
                            gpuExrProvenance->counters.readbacks == 1 &&
                            gpuPngProvenance.has_value() && gpuPngProvenance->gpuEvaluated() &&
                            gpuPngProvenance->counters.readbacks == 1,
                        "gpu parity: both GPU attempts performed exactly one final readback");

    // Process parity against the unchanged CPU final reference: finite-component absolute/relative
    // 2e-6, alpha exact.
    const auto& cpuPixels = cpuAttempt->frame()->processImage().pixels();
    const auto& gpuPixels = gpuExrAttempt->frame()->processImage().pixels();
    constexpr float kTolerance = 2e-6F;
    bool processParity = cpuPixels.size() == gpuPixels.size();
    if (processParity) {
        for (std::size_t index = 0; index < cpuPixels.size() && processParity; ++index) {
            const auto& cpuPixel = cpuPixels[index];
            const auto& gpuPixel = gpuPixels[index];
            if (cpuPixel.alpha() != gpuPixel.alpha()) {
                processParity = false;
                break;
            }
            for (std::size_t component = 0; component < 3; ++component) {
                const auto a = cpuPixel.components()[component];
                const auto b = gpuPixel.components()[component];
                const auto difference = std::abs(a - b);
                if (difference > kTolerance &&
                    difference > kTolerance * std::max(std::abs(a), std::abs(b))) {
                    processParity = false;
                    break;
                }
            }
        }
    }
    expectations.expect(processParity,
                        "gpu parity: GPU process pixels match the CPU reference within 2e-6 with "
                        "exact alpha");

    expectations.expect(publish(gpuExrAttempt, gpuExrTarget),
                        "gpu parity: the GPU-composited EXR publishes");
    const output::FlatExrRgba32fLinRec709SceneReopenVerifierV1 exrVerifier;
    const auto exrVerify = exrVerifier.verify(gpuExrTarget, gpuExrAttempt->processIdentity(),
                                              gpuExrAttempt->report(), {});
    expectations.expect(exrVerify.status() == output::FlatExrVerifyStatusV1::Verified,
                        "gpu parity: the GPU-composited EXR independently reopen-verifies pixels "
                        "and descriptors");

    expectations.expect(publish(cpuAttempt, cpuPngTarget) && publish(gpuPngAttempt, gpuPngTarget),
                        "gpu parity: both PNGs publish");
    const auto cpuDecoded = independentlyDecodePng(cpuPngTarget);
    const auto gpuDecoded = independentlyDecodePng(gpuPngTarget);
    expectations.expect(cpuDecoded.ok && gpuDecoded.ok && cpuDecoded.width == gpuDecoded.width &&
                            cpuDecoded.height == gpuDecoded.height &&
                            cpuDecoded.rgba.size() == gpuDecoded.rgba.size(),
                        "gpu parity: both PNGs independently decode with matching descriptors");
    bool pngParity =
        cpuDecoded.ok && gpuDecoded.ok && cpuDecoded.rgba.size() == gpuDecoded.rgba.size();
    if (pngParity) {
        for (std::size_t index = 0; index < cpuDecoded.rgba.size(); ++index) {
            const int difference = std::abs(static_cast<int>(cpuDecoded.rgba[index]) -
                                            static_cast<int>(gpuDecoded.rgba[index]));
            const bool alpha = index % 4U == 3U;
            if ((alpha && difference != 0) || (!alpha && difference > 1)) {
                pngParity = false;
                break;
            }
        }
    }
    expectations.expect(pngParity,
                        "gpu parity: GPU-composited PNG pixels match the CPU reference within one "
                        "code with exact alpha");

#ifdef BLOOM_GPUSHADER_TOOLS_DIR
    // Real GPU display route: the PNG attempt retains the verified DisplayRgba8 payload directly
    // (no CPU per-pixel display conversion), transferred in the SAME single submission as the
    // process payload.
    expectations.expect(gpuPngAttempt->gpuDisplay().isPresent(),
                        "gpu parity: the PNG attempt retains a GPU-encoded display payload");
    expectations.expect(gpuPngProvenance.has_value() &&
                            gpuPngProvenance->encodedArm ==
                                runtime::GpuOutputColorArm::DisplayRgba8 &&
                            gpuPngProvenance->readbackSubmissions == 1 &&
                            gpuPngProvenance->transferredPayloads == 2 &&
                            gpuPngProvenance->encodedPayloadBytes > 0 &&
                            gpuPngProvenance->counters.nativeDispatches > 0,
                        "gpu parity: the PNG route makes one submission carrying process+encoded "
                        "payloads with real dispatches");
#endif

    // Publish the genuine still-frame route proof from the exact run above. It is written only
    // after every assertion passed, so a failed parity/publication can never become a passing
    // proof; the evidence digest is bound to the actual process comparison and the independent PNG
    // decode, and the counters are the ACTUAL PNG output-colour arm's combined-readback provenance.
    if (!proofDirectory.empty() && expectations.failures() == 0) {
        // The still route proof REQUIRES the genuine GPU display-colour arm: one final submission
        // carrying the process-analysis and encoded display payloads with real encoded bytes. A GPU
        // process evaluation whose output colour silently ran on the CPU (one payload, no encoded
        // bytes) can never publish a green still proof.
        const bool gpuDisplayArm =
            gpuPngProvenance.has_value() &&
            gpuPngProvenance->encodedArm == runtime::GpuOutputColorArm::DisplayRgba8 &&
            gpuPngProvenance->readbackSubmissions == 1 &&
            gpuPngProvenance->transferredPayloads == 2 && gpuPngProvenance->encodedPayloadBytes > 0;
        expectations.expect(gpuDisplayArm,
                            "gpu parity: the still route proof requires the genuine GPU "
                            "display-colour arm (one submission, two payloads, encoded bytes)");
        if (gpuDisplayArm) {
            const auto identityDigest =
                routeproof::sha256Hex(gpuPngAttempt->processIdentity()->canonicalBytes());
            const auto& proofCpuPixels = cpuAttempt->frame()->processImage().pixels();
            const auto& proofGpuPixels = gpuPngAttempt->frame()->processImage().pixels();
            constexpr double kProofTolerance = static_cast<double>(kTolerance);
            routeproof::FrameEvidence evidence;
            evidence.identityHex = identityDigest;
            evidence.comparedPixels = proofCpuPixels.size();
            evidence.alphaExact = proofCpuPixels.size() == proofGpuPixels.size();
            double maxFloatDelta = 0.0;
            std::uint64_t processMismatches = 0;
            if (proofCpuPixels.size() == proofGpuPixels.size()) {
                for (std::size_t index = 0; index < proofCpuPixels.size(); ++index) {
                    const auto& cpuPixel = proofCpuPixels[index];
                    const auto& gpuPixel = proofGpuPixels[index];
                    if (cpuPixel.alpha() != gpuPixel.alpha()) {
                        evidence.alphaExact = false;
                        ++processMismatches;
                        continue;
                    }
                    bool pixelBad = false;
                    for (std::size_t component = 0; component < 3; ++component) {
                        const double cpuValue =
                            static_cast<double>(cpuPixel.components()[component]);
                        const double gpuValue =
                            static_cast<double>(gpuPixel.components()[component]);
                        const double difference = std::abs(cpuValue - gpuValue);
                        maxFloatDelta = std::max(maxFloatDelta, difference);
                        if (difference > kProofTolerance &&
                            difference > kProofTolerance *
                                             std::max(std::abs(cpuValue), std::abs(gpuValue))) {
                            pixelBad = true;
                        }
                    }
                    if (pixelBad) {
                        ++processMismatches;
                    }
                }
            }
            evidence.maxFloatDelta = maxFloatDelta;
            evidence.mismatchedPixels = processMismatches;
            evidence.cpuDecodedDigest =
                routeproof::sha256Pixels(std::span<const std::uint8_t>(cpuDecoded.rgba));
            evidence.gpuDecodedDigest =
                routeproof::sha256Pixels(std::span<const std::uint8_t>(gpuDecoded.rgba));
            std::uint64_t decodedMismatches = 0;
            std::uint64_t maxIntegerDelta = 0;
            bool decodedAlphaExact = true;
            const auto decodedCount = std::min(cpuDecoded.rgba.size(), gpuDecoded.rgba.size());
            for (std::size_t index = 0; index < decodedCount; ++index) {
                const auto difference =
                    static_cast<std::uint64_t>(std::abs(static_cast<int>(cpuDecoded.rgba[index]) -
                                                        static_cast<int>(gpuDecoded.rgba[index])));
                maxIntegerDelta = std::max(maxIntegerDelta, difference);
                const bool alpha = index % 4U == 3U;
                if (alpha) {
                    if (difference != 0) {
                        decodedAlphaExact = false;
                        ++decodedMismatches;
                    }
                } else if (difference > 1) {
                    ++decodedMismatches;
                }
            }
            evidence.maxIntegerDelta = maxIntegerDelta;
            evidence.mismatchedPixels += decodedMismatches;
            evidence.alphaExact = evidence.alphaExact && decodedAlphaExact;

            routeproof::ExportProofCounters counters;
            counters.deviceOwnershipEpoch = gpuPngProvenance->deviceOwnershipEpoch;
            counters.nativeDispatches = gpuPngProvenance->counters.nativeDispatches;
            counters.verifiedFrames = 1;
            // The ACTUAL combined-readback submission/payload/byte counters from the PNG attempt
            // provenance: one submission, two payloads (process + encoded display), exact combined
            // bytes. Never derived from the readback count or the frame dimensions.
            counters.readbackSubmissions = gpuPngProvenance->readbackSubmissions;
            counters.payloads = gpuPngProvenance->transferredPayloads;
            counters.transferredBytes =
                gpuPngProvenance->processPayloadBytes + gpuPngProvenance->encodedPayloadBytes;

            const std::array<std::string, 1> identityList{identityDigest};
            const std::array<routeproof::FrameEvidence, 1> evidenceList{evidence};
            const auto processDigest = routeproof::orderedIdentityDigest(identityList);
            const auto capturedEvidenceDigest = routeproof::evidenceDigest(evidenceList);
            std::string nonce;
            if (!routeproof::readProofNonce(proofDirectory, nonce)) {
                expectations.expect(
                    false, "gpu parity: a fresh run nonce is required for the still proof");
            } else {
                const auto written = routeproof::publishExportProof(
                    proofDirectory, nonce, "route.export.still_frame",
                    bloom::runtime::GpuRouteHarnessKind::StillFrameExport, counters, processDigest,
                    capturedEvidenceDigest);
                if (!written.written) {
                    expectations.expect(false, "gpu parity: the still route proof was rejected: " +
                                                   written.detail);
                } else {
                    std::cout << "PASS(route-proof) route.export.still_frame frames="
                              << counters.verifiedFrames
                              << " dispatches=" << counters.nativeDispatches
                              << " readbacks=" << counters.readbackSubmissions
                              << " payloads=" << counters.payloads
                              << " bytes=" << counters.transferredBytes << '\n';
                }
            }
        }
    }
    static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
    return GpuProofOutcome::Ran;
}
