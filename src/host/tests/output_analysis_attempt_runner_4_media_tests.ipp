// Portable media resolves against the captured project directory, including CPU-only final
// rendering. Neither a destination-neighbor file nor retained preview bytes establishes authority.
void testFinalRelativeMediaSourceContext(Expectations& expectations, const bool requireDevice) {
    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "relative media: temporary directory");
        return;
    }
    const auto projectDirectory = directory.path() / "project";
    const auto outputDirectory = directory.path() / "output";
    std::filesystem::create_directories(projectDirectory);
    std::filesystem::create_directories(outputDirectory);
    const auto sourcePath = projectDirectory / "portable.jpg";
    const auto writeFixture = [](const std::filesystem::path& path) {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(kGeneratedJpeg.data()), kGeneratedJpeg.size());
    };
    writeFixture(sourcePath);
    // An identical destination-neighbor is deliberately present throughout the refusal cases.
    writeFixture(outputDirectory / "portable.jpg");
    const auto digest = bloom::core::Sha256Hasher::hash(std::as_bytes(std::span(kGeneratedJpeg)));
    expectations.expect(digest.has_value(), "relative media: actual generated JPEG digest");
    if (!digest) {
        return;
    }
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(0x1201);
    asset.kind = document::AssetKind::Image;
    asset.locator.path = "portable.jpg";
    // The schema requires a file URI hint, but this contains no usable absolute path.
    asset.locator.relinkHint = "file:";
    asset.contentDigest = *digest;
    asset.width = 2;
    asset.height = 2;
    asset.interpretation.colorSpace = document::AssetColorSpace::Raw;
    auto definition = smallSolidPlan()->copyDefinition();
    definition.operations.front() =
        runtime::CompiledImageSource{kSolidNodeId, asset, 0, 0, 0, {}, false};
    const auto plan =
        std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));

    runtime::TaskScheduler scheduler;
    auto artifacts = platform::StagedArtifactCoordinator::create({});
    if (!artifacts) {
        expectations.expect(false, "relative media: artifact coordinator");
        return;
    }
    auto coordinator = std::move(artifacts).takeCoordinator();
    output::ExportResourceLedgerV1 ledger;
    auto request = requestFor(outputDirectory / "relative.exr");
    request.plan = plan;
    request.assetBaseDirectory = projectDirectory;
    // This fixture includes JPEG decoder scratch, unlike requestFor()'s tiny solid-only plan.
    request.evaluation.pixelStorageByteLimit = std::size_t{1024} * 1024;
    request.evaluation.mediaSourceRevisionPolicy = runtime::MediaSourceRevisionPolicy::Retained;
    const runtime::CpuCompositionEvaluator preview;
    preview.setAssetBaseDirectory(projectDirectory);
    const auto reference = preview.evaluate(plan, request.evaluation, {});
    expectations.expect(reference.frame() != nullptr, "relative media: retained CPU preview");
    if (!reference.frame()) {
        for (const auto& diagnostic : reference.diagnostics()) {
            std::cerr << "diagnostic=" << static_cast<unsigned>(diagnostic.code) << ' '
                      << diagnostic.summary << ": " << diagnostic.detail << '\n';
        }
        return;
    }
    expectations.expect(reference.diagnostics().empty(),
                        "relative media: preview decoded without placeholders");
    // A failed preview decode may display a placeholder, but it cannot authorize a final cache hit
    // or poison a later successful decode of the same original bytes at a sufficient allowance.
    const runtime::CpuCompositionEvaluator constrainedPreview;
    constrainedPreview.setAssetBaseDirectory(projectDirectory);
    auto constrained = request.evaluation;
    constrained.pixelStorageByteLimit = 4096;
    const auto placeholder = constrainedPreview.evaluate(plan, constrained, {});
    expectations.expect(
        placeholder.frame() && !placeholder.diagnostics().empty(),
        "relative media: small JPEG decoder allowance produces an honest preview placeholder");
    constrained.requireVerifiedMediaSources = true;
    const auto refusedPlaceholder = constrainedPreview.evaluate(plan, constrained, {});
    expectations.expect(
        !refusedPlaceholder.frame() &&
            refusedPlaceholder.status() == runtime::EvaluationStatus::Failed,
        "relative media: final cannot reuse a failed decode's memoized placeholder");
    const auto recovered = constrainedPreview.evaluate(plan, request.evaluation, {});
    expectations.expect(
        recovered.frame() && recovered.diagnostics().empty() &&
            std::ranges::equal(recovered.frame()->processImage().pixels(),
                               reference.frame()->processImage().pixels()),
        "relative media: successful decode replaces every downstream placeholder identity");
    auto verified = request.evaluation;
    verified.mediaSourceRevisionPolicy = runtime::MediaSourceRevisionPolicy::Revalidate;
    verified.requireVerifiedMediaSources = true;
    const auto checked = preview.evaluate(plan, verified, {});
    expectations.expect(checked.frame() != nullptr, "relative media: verified CPU fixture");
    if (!checked.frame()) {
        for (const auto& diagnostic : checked.diagnostics())
            std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
    }
    const auto run = [&](host::OutputAnalysisAttemptRequestV1 attemptRequest) {
        auto begin = host::beginOutputAnalysisAttemptV1(scheduler, coordinator, ledger,
                                                        std::move(attemptRequest));
        expectations.expect(static_cast<bool>(begin), "relative media: analysis submitted");
        if (!begin) {
            return std::optional<host::OutputAnalysisAttemptOutcomeV1>{};
        }
        auto runner = std::move(begin).takeHandle();
        return pumpUntilComplete(runner);
    };
    {
        const auto outcome = run(request);
        expectations.expect(
            outcome && static_cast<bool>(*outcome),
            "relative media: project-relative CPU final succeeds without absolute relink hint");
        if (outcome && outcome->failure()) {
            std::cerr << "relative media final failure stage="
                      << static_cast<unsigned>(outcome->failure()->stage()) << " payload=";
            if (const auto* code =
                    outcome->failure()->payloadAs<runtime::EvaluationDiagnosticCode>())
                std::cerr << static_cast<unsigned>(*code);
            std::cerr << '\n';
        }
        if (outcome && *outcome) {
            const auto& frame = outcome->attempt()->frame();
            expectations.expect(
                outcome->attempt()->approvable() &&
                    frame->identity().provider == runtime::EvaluationProvider::CpuReference &&
                    std::ranges::equal(frame->processImage().pixels(),
                                       reference.frame()->processImage().pixels()),
                "relative media: final retains genuine CPU pixels and approvable report");
        }
    }
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader && *loader) {
        auto options = gpuOptions(true, std::filesystem::path{loader});
        options.mediaContextProvider = [wrongBase = directory.path() / "new-project"] {
            runtime::GpuSceneMediaContext context;
            context.assetBaseDirectory = wrongBase;
            return context;
        };
        auto provider = host::GpuExportProvider::create(std::move(options));
        provider->prepare(scheduler);
        const auto bootstrapDeadline = std::chrono::steady_clock::now() + 30s;
        while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline)
            std::this_thread::sleep_for(1ms);
        auto nativeRequest = request;
        nativeRequest.gpuProvider = provider;
        {
            const auto native = run(std::move(nativeRequest));
            expectations.expect(native && static_cast<bool>(*native),
                                "relative media: GPU-provider final completes");
            if (native && *native) {
                const auto& frame = native->attempt()->frame();
                const auto& provenance = native->gpuProvenance();
                expectations.expect(
                    !provider->deviceAvailable() ||
                        (frame->identity().provider == runtime::EvaluationProvider::GpuResident &&
                         provenance && provenance->gpuEvaluated() &&
                         provenance->counters.nativeDispatches > 0 &&
                         provenance->counters.readbacks == 1 &&
                         std::ranges::equal(frame->processImage().pixels(),
                                            reference.frame()->processImage().pixels())),
                    "relative media: native final uses captured old-project base with exact CPU "
                    "pixels");
            }
        }
        expectations.expect(!requireDevice || provider->deviceAvailable(),
                            "relative media: required native device is available");
        expectations.expect(provider->shutdownAndWait(10s),
                            "relative media: native provider safely retires");
    } else {
        expectations.expect(!requireDevice, "relative media: required native loader is specified");
    }
    const auto refused = [&](const auto& outcome) {
        const auto* failure = outcome ? outcome->failure() : nullptr;
        const auto* diagnostic =
            failure ? failure->template payloadAs<runtime::EvaluationDiagnosticCode>() : nullptr;
        expectations.expect(
            failure && failure->stage() == host::OutputAnalysisAttemptStageV1::Evaluating &&
                diagnostic && *diagnostic == runtime::EvaluationDiagnosticCode::InvalidParameter &&
                ledger.chargedBytes() == 0,
            "relative media: unavailable/changed original fails final verification without charge");
    };
    auto noBase = request;
    noBase.assetBaseDirectory.clear();
    refused(run(std::move(noBase)));
    const auto originalTime = std::filesystem::last_write_time(sourcePath);
    {
        std::ofstream file(sourcePath, std::ios::binary | std::ios::app);
        file.put('\0');
    }
    std::filesystem::last_write_time(sourcePath, originalTime);
    std::vector<unsigned char> changedBytes(kGeneratedJpeg.begin(), kGeneratedJpeg.end());
    changedBytes.push_back(0);
    const auto changed = bloom::core::Sha256Hasher::hash(std::as_bytes(std::span(changedBytes)));
    expectations.expect(
        changed.has_value() && *changed != asset.contentDigest,
        "relative media: same-path same-mtime replacement has a different strong digest");
    refused(run(request));
    std::filesystem::remove(sourcePath);
    const auto warm = preview.evaluate(plan, request.evaluation, {});
    expectations.expect(
        warm.frame() && std::ranges::equal(warm.frame()->processImage().pixels(),
                                           reference.frame()->processImage().pixels()),
        "relative media: retained preview still owns the old revision during outage");
    refused(run(request));
}

void testCapturedFinalColorIntentLifetime(Expectations& expectations) {
    TempDirectory directory;
    auto artifacts = platform::StagedArtifactCoordinator::create({});
    if (!directory.isValid() || !artifacts) {
        expectations.expect(false, "final color capture: fixture is available");
        return;
    }
    runtime::TaskScheduler scheduler;
    auto coordinator = std::move(artifacts).takeCoordinator();
    output::ExportResourceLedgerV1 ledger;
    std::shared_ptr<const runtime::ProcessFrame> retained;
    {
        std::string workingSpace{runtime::kLinearRec709SceneColorSpaceId};
        std::string configUri{runtime::kBloomNeutralOcioConfigUri};
        auto request = requestFor(directory.path() / "captured-color.exr");
        request.evaluation.colorIntent.workingColorSpaceId = workingSpace;
        request.evaluation.colorIntent.ocioConfigUri = configUri;
        auto begin =
            host::beginOutputAnalysisAttemptV1(scheduler, coordinator, ledger, std::move(request));
        if (!begin) {
            expectations.expect(false, "final color capture: analysis submitted");
            return;
        }
        workingSpace.assign("missing-after-admission");
        configUri.assign("missing://after-admission");
        auto runner = std::move(begin).takeHandle();
        const auto outcome = pumpUntilComplete(runner);
        expectations.expect(
            outcome && static_cast<bool>(*outcome),
            "final color capture: producer string changes cannot alter admitted work");
        if (outcome && *outcome)
            retained = outcome->attempt()->frame();
    }
    expectations.expect(
        retained && retained->identity().colorIntent.storage &&
            retained->identity().colorIntent.workingColorSpaceId ==
                runtime::kLinearRec709SceneColorSpaceId &&
            retained->identity().colorIntent.ocioConfigUri == runtime::kBloomNeutralOcioConfigUri,
        "final color capture: frame owns color views after runner and caller destruction");
}
