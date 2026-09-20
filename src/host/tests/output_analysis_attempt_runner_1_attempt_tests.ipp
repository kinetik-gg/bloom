// Attempt-lifecycle group: stable digest, cancellation, resource exhaustion, and the honest
// provider-disabled CPU fallback. Split verbatim from output_analysis_attempt_runner_tests.cpp.

void testFullGraphProducesStableDigestAcrossTwoRuns(Expectations& expectations) {
    TempDirectory directory;
    expectations.expect(directory.isValid(), "full graph: temp directory is available");
    if (!directory.isValid()) {
        return;
    }
    runtime::TaskScheduler scheduler;
    auto artifacts = platform::StagedArtifactCoordinator::create({});
    expectations.expect(artifacts.succeeded(),
                        "full graph: staged-artifact coordinator is created");
    if (!artifacts) {
        return;
    }
    auto coordinator = std::move(artifacts).takeCoordinator();
    output::ExportResourceLedgerV1 ledger;

    std::optional<bloom::core::Sha256Digest> firstDigest;
    for (int run = 0; run < 2; ++run) {
        auto begin = host::beginOutputAnalysisAttemptV1(
            scheduler, coordinator, ledger,
            requestFor(directory.path() / ("attempt-" + std::to_string(run) + ".exr")));
        expectations.expect(static_cast<bool>(begin),
                            "full graph: begin submits the Resolving task");
        if (!begin) {
            continue;
        }
        auto runner = std::move(begin).takeHandle();
        auto outcome = pumpUntilComplete(runner);
        expectations.expect(outcome.has_value(),
                            "full graph: the attempt reaches a terminal outcome");
        if (!outcome.has_value()) {
            continue;
        }
        expectations.expect(static_cast<bool>(*outcome),
                            "full graph: Resolving -> Evaluating -> Identifying -> Analyzing "
                            "completes successfully");
        if (!*outcome) {
            continue;
        }
        expectations.expect((*outcome).attempt()->approvable() &&
                                (*outcome).attempt()->digest().has_value(),
                            "full graph: the completed attempt is approvable with a digest");
        if (run == 0) {
            firstDigest = (*outcome).attempt()->digest();
        } else {
            expectations.expect(firstDigest.has_value() &&
                                    (*outcome).attempt()->digest() == firstDigest,
                                "full graph: the digest is stable across two independent runs "
                                "over the identical fixture");
        }
    }
}

void testCancellationBeforeResolvingCompletes(Expectations& expectations) {
    TempDirectory directory;
    expectations.expect(directory.isValid(), "cancel: temp directory is available");
    if (!directory.isValid()) {
        return;
    }
    runtime::TaskScheduler scheduler;
    auto artifacts = platform::StagedArtifactCoordinator::create({});
    expectations.expect(artifacts.succeeded(), "cancel: staged-artifact coordinator is created");
    if (!artifacts) {
        return;
    }
    auto coordinator = std::move(artifacts).takeCoordinator();
    output::ExportResourceLedgerV1 ledger;

    auto begin = host::beginOutputAnalysisAttemptV1(scheduler, coordinator, ledger,
                                                    requestFor(directory.path() / "cancelled.exr"));
    expectations.expect(static_cast<bool>(begin), "cancel: begin submits the Resolving task");
    if (!begin) {
        return;
    }
    auto runner = std::move(begin).takeHandle();
    runner.requestCancellation();
    auto outcome = pumpUntilComplete(runner);
    expectations.expect(outcome.has_value(), "cancel: the attempt reaches a terminal outcome");
    if (!outcome.has_value()) {
        return;
    }
    expectations.expect(!*outcome, "cancel: an immediately-cancelled attempt does not complete");
    const auto* failure = outcome->failure();
    expectations.expect(failure != nullptr && failure->cancelled(),
                        "cancel: the typed failure is marked cancelled");
}

void testResourceExhaustionIsTypedWithZeroLeak(Expectations& expectations) {
    TempDirectory directory;
    expectations.expect(directory.isValid(), "resource exhaustion: temp directory is available");
    if (!directory.isValid()) {
        return;
    }
    runtime::TaskScheduler scheduler;
    auto artifacts = platform::StagedArtifactCoordinator::create({});
    expectations.expect(artifacts.succeeded(),
                        "resource exhaustion: staged-artifact coordinator is created");
    if (!artifacts) {
        return;
    }
    auto coordinator = std::move(artifacts).takeCoordinator();
    output::ExportResourceLedgerV1 ledger(/*concurrentAllowance=*/1);

    auto begin = host::beginOutputAnalysisAttemptV1(scheduler, coordinator, ledger,
                                                    requestFor(directory.path() / "exhausted.exr"));
    expectations.expect(static_cast<bool>(begin),
                        "resource exhaustion: begin submits the Resolving task");
    if (!begin) {
        return;
    }
    auto runner = std::move(begin).takeHandle();
    auto outcome = pumpUntilComplete(runner);
    expectations.expect(outcome.has_value(),
                        "resource exhaustion: the attempt reaches a terminal outcome");
    if (!outcome.has_value()) {
        return;
    }
    expectations.expect(!*outcome,
                        "resource exhaustion: a 1-byte ledger cannot admit a real evaluated frame");
    const auto* failure = outcome->failure();
    expectations.expect(failure != nullptr &&
                            failure->payloadAs<output::OutputAnalysisAttemptErrorCodeV1>() !=
                                nullptr &&
                            *failure->payloadAs<output::OutputAnalysisAttemptErrorCodeV1>() ==
                                output::OutputAnalysisAttemptErrorCodeV1::ResourceReservationFailed,
                        "resource exhaustion: the failure is a typed ResourceReservationFailed at "
                        "the Analyzing stage");
    expectations.expect(ledger.chargedBytes() == 0,
                        "resource exhaustion: a refused attempt charges nothing (zero leak)");
}

// The explicit-off provider is truthful: no device native work is possible, the published
// evaluator reports unavailable, and the real output attempt still completes with zero GPU
// counters and CPU frame provenance. This is the desktop/headless fallback contract. The provider
// itself is dropped while the attempt is still in flight, proving the async attempt owns it.
void testGpuExportProviderDisabledFallback(Expectations& expectations) {
    runtime::TaskScheduler scheduler;
    auto provider = host::GpuExportProvider::create(gpuOptions(false));
    provider->prepare(scheduler);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->prepared(),
                        "provider-disabled: bootstrap reaches a terminal state");
    expectations.expect(provider->evaluator() != nullptr,
                        "provider-disabled: a CPU-unavailable evaluator is published");
    expectations.expect(!provider->deviceAvailable(),
                        "provider-disabled: no device is reported available");

    TempDirectory directory;
    expectations.expect(directory.isValid(), "provider-disabled: temp directory is available");
    if (!directory.isValid()) {
        return;
    }
    auto artifacts = platform::StagedArtifactCoordinator::create({});
    expectations.expect(artifacts.succeeded(), "provider-disabled: coordinator is created");
    if (!artifacts) {
        return;
    }
    auto coordinator = std::move(artifacts).takeCoordinator();
    output::ExportResourceLedgerV1 ledger;
    auto request = requestFor(directory.path() / "disabled.exr");
    request.gpuProvider = provider;
    auto begin =
        host::beginOutputAnalysisAttemptV1(scheduler, coordinator, ledger, std::move(request));
    expectations.expect(static_cast<bool>(begin), "provider-disabled: begin submits the attempt");
    if (!begin) {
        return;
    }
    // The attempt now owns the provider; retire the local application handle immediately. Nothing
    // may dangle or fall back differently because the provider was retired mid-attempt.
    provider.reset();
    auto runner = std::move(begin).takeHandle();
    auto outcome = pumpUntilComplete(runner);
    expectations.expect(outcome.has_value() && static_cast<bool>(*outcome),
                        "provider-disabled: the CPU reference attempt completes");
    if (!outcome.has_value() || !*outcome) {
        return;
    }
    expectations.expect((*outcome).attempt()->frame()->identity().provider ==
                            runtime::EvaluationProvider::CpuReference,
                        "provider-disabled: the retained frame keeps CPU provenance");
    const auto& provenance = (*outcome).gpuProvenance();
    expectations.expect(
        provenance.has_value() && provenance->status == runtime::GpuProcessFrameStatus::Disabled &&
            provenance->counters.nativeDispatches == 0 && provenance->counters.readbacks == 0,
        "provider-disabled: provenance is Disabled with zero GPU work");
}
