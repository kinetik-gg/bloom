// GPU host-path group: the REAL output attempt with the genuine GPU final-render bridge attached.
// Split verbatim from output_analysis_attempt_runner_tests.cpp.

// Exercises the ACTUAL export host path (beginOutputAnalysisAttemptV1 -> Resolving -> Evaluating ->
// Identifying -> Analyzing) with the genuine GPU final-render bridge attached. The loader is read
// from BLOOM_TEST_VULKAN_LOADER so no machine-specific path is embedded in source; an absent loader
// or device prints an explicit NOTE and is not a failure. When `requireDevice` is set the same
// absence is a hard failure, so a native qualification run can demand a real device. The runtime
// fixture bloom.runtime.gpu_process_frame is the authority for per-op native dispatch counters;
// this host test is the authority for the counters/provenance retained by the REAL output attempt.
void testGpuEvaluatorHostPath(Expectations& expectations, const bool requireDevice) {
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        if (requireDevice) {
            expectations.expect(false,
                                "gpu host path: --require-device needs BLOOM_TEST_VULKAN_LOADER "
                                "to name an absolute loader");
            return;
        }
        std::cout << "NOTE: BLOOM_TEST_VULKAN_LOADER unset; skipping the GPU export host path\n";
        return;
    }
    runtime::TaskScheduler scheduler;
    auto provider =
        host::GpuExportProvider::create(gpuOptions(true, std::filesystem::path(loader)));
    provider->prepare(scheduler);
    // Test convenience only: wait for the provider's off-thread bootstrap to terminal. Production
    // never polls; the attempt runner defers its evaluation stage instead.
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + 30s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->prepared(), "gpu host path: provider bootstrap terminals");
    if (!provider->deviceAvailable()) {
        expectations.expect(!requireDevice, "gpu host path: a device is required");
        const auto handle = provider->evaluator();
        std::cout << "NOTE: no compatible Vulkan device; skipping the GPU export host path: "
                  << (handle != nullptr ? handle->availabilityDiagnostic().message : std::string{})
                  << '\n';
        return;
    }
    TempDirectory directory;
    expectations.expect(directory.isValid(), "gpu host path: temp directory is available");
    if (!directory.isValid()) {
        return;
    }
    auto artifacts = platform::StagedArtifactCoordinator::create({});
    expectations.expect(artifacts.succeeded(), "gpu host path: coordinator is created");
    if (!artifacts) {
        return;
    }
    auto coordinator = std::move(artifacts).takeCoordinator();
    output::ExportResourceLedgerV1 ledger;

    auto request = requestFor(directory.path() / "gpu-attempt.exr");
    request.gpuProvider = provider;
    auto begin =
        host::beginOutputAnalysisAttemptV1(scheduler, coordinator, ledger, std::move(request));
    expectations.expect(static_cast<bool>(begin),
                        "gpu host path: begin submits the Resolving task");
    if (!begin) {
        return;
    }
    auto runner = std::move(begin).takeHandle();
    auto outcome = pumpUntilComplete(runner);
    expectations.expect(outcome.has_value(), "gpu host path: attempt reaches a terminal outcome");
    if (!outcome.has_value()) {
        return;
    }
    expectations.expect(static_cast<bool>(*outcome),
                        "gpu host path: the GPU-evaluated attempt completes");
    if (*outcome) {
        expectations.expect((*outcome).attempt()->approvable() &&
                                (*outcome).attempt()->digest().has_value(),
                            "gpu host path: the attempt is approvable with a digest");
        // Native provenance/counters on the real output attempt: the frame came back from the
        // prepared-GPU scene executor with GpuResident provenance, performed real dispatches, and
        // did exactly one final RGBA32F readback.
        expectations.expect((*outcome).attempt()->frame()->identity().provider ==
                                runtime::EvaluationProvider::GpuResident,
                            "gpu host path: the retained frame has GpuResident provenance");
        const auto& provenance = (*outcome).gpuProvenance();
        expectations.expect(provenance.has_value() && provenance->gpuEvaluated(),
                            "gpu host path: the attempt records a GPU evaluation");
        if (provenance.has_value()) {
            expectations.expect(provenance->counters.nativeDispatches > 0,
                                "gpu host path: the attempt records real native dispatches");
            expectations.expect(provenance->counters.readbacks == 1,
                                "gpu host path: the attempt records exactly one final readback");
            expectations.expect(provenance->deviceOwnershipEpoch > 0,
                                "gpu host path: the attempt reports the genuine device epoch");
            expectations.expect(
                provenance->readbackSubmissions == 1 && provenance->transferredPayloads == 1 &&
                    provenance->encodedArm == runtime::GpuOutputColorArm::None &&
                    provenance->encodedPayloadBytes == 0 && provenance->processPayloadBytes > 0,
                "gpu host path: the identity arm transfers exactly one process "
                "payload in one submission");
        }
    }
    static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
}
