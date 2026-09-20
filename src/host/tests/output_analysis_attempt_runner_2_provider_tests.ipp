// Provider-lifecycle group: asynchronous retirement, shutdown races, and concurrent display
// preparation. Split verbatim from output_analysis_attempt_runner_tests.cpp (including the shared
// single-CPU-worker blocker helpers used by the bootstrap tests).

// Genuine asynchronous retirement against the real provider lifecycle. With a live device the
// provider is prepared, a real output attempt is started and deliberately interrupted by
// beginShutdown(), and the last attempt outcome handle is retained until after retirement
// completion is proven. beginShutdown() must return without blocking, and the UI-simulated pump
// must observe completion; only then is the evaluator released. The provider is destroyed before
// the scheduler (the ordering every route guarantees).
void testGpuExportProviderAsyncRetirement(Expectations& expectations, const bool requireDevice) {
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        if (requireDevice) {
            expectations.expect(
                false, "async-retirement: --require-device needs BLOOM_TEST_VULKAN_LOADER");
        } else {
            std::cout << "NOTE: BLOOM_TEST_VULKAN_LOADER unset; skipping async GPU retirement\n";
        }
        return;
    }
    runtime::TaskScheduler scheduler;
    auto provider =
        host::GpuExportProvider::create(gpuOptions(true, std::filesystem::path(loader)));
    provider->prepare(scheduler);
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + 30s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->prepared(), "async-retirement: bootstrap terminals");
    if (!provider->deviceAvailable()) {
        expectations.expect(!requireDevice, "async-retirement: a device is required");
        std::cout << "NOTE: no compatible Vulkan device; skipping async GPU retirement\n";
        return;
    }

    TempDirectory directory;
    expectations.expect(directory.isValid(), "async-retirement: temp directory is available");
    if (!directory.isValid()) {
        return;
    }
    auto artifacts = platform::StagedArtifactCoordinator::create({});
    expectations.expect(artifacts.succeeded(), "async-retirement: coordinator created");
    if (!artifacts) {
        return;
    }
    auto coordinator = std::move(artifacts).takeCoordinator();
    output::ExportResourceLedgerV1 ledger;
    auto request = requestFor(directory.path() / "async-retirement.exr");
    request.gpuProvider = provider;
    auto begin =
        host::beginOutputAnalysisAttemptV1(scheduler, coordinator, ledger, std::move(request));
    expectations.expect(static_cast<bool>(begin), "async-retirement: attempt begins");
    std::optional<host::OutputAnalysisAttemptOutcomeV1> outcome;
    if (begin) {
        auto runner = std::move(begin).takeHandle();
        // Deliberate mid-flight shutdown: signal the owner while the attempt may still be pending.
        const auto shutdownStarted = std::chrono::steady_clock::now();
        provider->beginShutdown();
        const auto shutdownElapsed = std::chrono::steady_clock::now() - shutdownStarted;
        expectations.expect(shutdownElapsed < std::chrono::seconds(1),
                            "async-retirement: beginShutdown never blocks the caller");
        outcome = pumpUntilComplete(runner);
        expectations.expect(outcome.has_value(),
                            "async-retirement: the interrupted attempt reaches a terminal outcome");
    }
    // The last attempt outcome handle is intentionally still alive here; retirement must complete
    // asynchronously without it being released.
    const auto retireDeadline = std::chrono::steady_clock::now() + 15s;
    while (!provider->retirementComplete() && std::chrono::steady_clock::now() < retireDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->retirementComplete(),
                        "async-retirement: retirement completes asynchronously");
    provider->collectRetired();
    outcome.reset(); // last attempt handle dropped only after completion proof
    provider.reset();
    const auto quiesceDeadline = std::chrono::steady_clock::now() + 15s;
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < quiesceDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(scheduler.isQuiescent(), "async-retirement: the scheduler quiesces");
}

// The root regression: admission is closed while the provider is still alive. Retirement completion
// is still proven asynchronously through the same non-blocking poll; the caller never blocks and
// never releases before completion proof. The assertion deliberately does not depend on which
// thread performs the eventual (trivial) release.
void testGpuExportProviderRetirementAfterAdmissionClosed(Expectations& expectations) {
    runtime::TaskScheduler scheduler;
    auto provider = host::GpuExportProvider::create(
        gpuOptions(true, "/nonexistent/bloom/test/vulkan/loader.so"));
    provider->prepare(scheduler);
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->prepared(), "closed-admission: bootstrap terminals");
    expectations.expect(scheduler.isAccepting(), "closed-admission: admission starts open");
    scheduler.beginShutdown();
    expectations.expect(!scheduler.isAccepting(), "closed-admission: admission is closed");

    const auto shutdownStarted = std::chrono::steady_clock::now();
    provider->beginShutdown();
    expectations.expect(std::chrono::steady_clock::now() - shutdownStarted <
                            std::chrono::seconds(1),
                        "closed-admission: beginShutdown never blocks the caller");
    const auto retireDeadline = std::chrono::steady_clock::now() + 15s;
    while (!provider->retirementComplete() && std::chrono::steady_clock::now() < retireDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(
        provider->retirementComplete(),
        "closed-admission: retirement completes asynchronously after admission closed");
    provider->collectRetired();
    provider.reset();
    const auto quiesceDeadline = std::chrono::steady_clock::now() + 15s;
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < quiesceDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(scheduler.isQuiescent(),
                        "closed-admission: the scheduler quiesces after provider destruction");
}

// Shutting the provider down before its lazy bootstrap finishes must not create a device, must not
// join anything on the caller, and must leave the scheduler able to quiesce.
void testGpuExportProviderShutdownDuringBootstrap(Expectations& expectations) {
    runtime::TaskScheduler scheduler;
    auto provider = host::GpuExportProvider::create(
        gpuOptions(true, "/nonexistent/bloom/test/vulkan/loader.so"));
    provider->prepare(scheduler);
    provider->beginShutdown(); // shutdown while the bootstrap task is queued or running
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!provider->retirementComplete() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->retirementComplete(),
                        "shutdown-during-bootstrap: retirement completes");
    provider->collectRetired();
    provider.reset();
    const auto quiesceDeadline = std::chrono::steady_clock::now() + 15s;
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < quiesceDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(scheduler.isQuiescent(),
                        "shutdown-during-bootstrap: the scheduler quiesces, no join on caller");
}

// Deterministically holds the scheduler's single CPU worker so the provider's bootstrap stays
// queued. `started` proves the worker is genuinely occupied; `release` lets it finish. A single
// CPU worker is required: the default config derives a multi-worker pool, where the bootstrap would
// run on another worker instead of staying queued.
[[nodiscard]] runtime::TaskSchedulerConfig singleCpuWorkerConfig() {
    runtime::TaskSchedulerConfig config;
    config.cpuWorkerCount = 1;
    config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    return config;
}

struct CpuWorkerBlocker final {
    std::atomic_bool started{false};
    std::atomic_bool release{false};
    runtime::TaskHandle<void> handle;
};

[[nodiscard]] std::shared_ptr<CpuWorkerBlocker> occupyCpuWorker(runtime::TaskScheduler& scheduler) {
    auto blocker = std::make_shared<CpuWorkerBlocker>();
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest(
            "hold the CPU worker",
            {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(7)},
            runtime::TaskPriority::Interactive, runtime::TaskExecutor::Cpu),
        [blocker](runtime::TaskContext&) -> runtime::TaskResult<void> {
            blocker->started.store(true, std::memory_order_release);
            while (!blocker->release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(1ms);
            }
            return runtime::TaskResult<void>::succeeded();
        });
    if (submission.accepted()) {
        blocker->handle = std::move(submission.handle);
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!blocker->started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return blocker;
}

// A scheduled bootstrap that is still queued (the CPU worker is deliberately occupied) is NOT
// complete, and beginShutdown() -- a mere signal -- must not make it complete either. Completion
// arrives only when the bootstrap task genuinely runs and terminalizes.
void testGpuExportProviderBootstrapInFlightIsNotComplete(Expectations& expectations) {
    runtime::TaskScheduler scheduler(singleCpuWorkerConfig());
    auto blocker = occupyCpuWorker(scheduler);
    expectations.expect(blocker->started.load(std::memory_order_acquire),
                        "bootstrap-in-flight: the CPU worker is genuinely occupied");

    auto provider = host::GpuExportProvider::create(
        gpuOptions(true, "/nonexistent/bloom/test/vulkan/loader.so"));
    provider->prepare(scheduler); // bootstrap is accepted but stays queued behind the blocker
    expectations.expect(!provider->retirementComplete(),
                        "bootstrap-in-flight: a queued bootstrap is not complete");
    provider->beginShutdown();
    expectations.expect(!provider->retirementComplete(),
                        "bootstrap-in-flight: beginShutdown (a signal) is not completion proof");

    blocker->release.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!provider->retirementComplete() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->retirementComplete(),
                        "bootstrap-in-flight: retirement completes once the task terminalizes");
    expectations.expect(provider->evaluator() == nullptr,
                        "bootstrap-in-flight: a shutdown-racing bootstrap publishes no device");
    provider->collectRetired();
    provider.reset();
    const auto quiesceDeadline = std::chrono::steady_clock::now() + 15s;
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < quiesceDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(scheduler.isQuiescent(), "bootstrap-in-flight: the scheduler quiesces");
}

// A bootstrap queued when the scheduler closes admission is cancelled before its lambda ever runs.
// The provider must observe that owned task's terminal state and report completion, without ever
// constructing an evaluator and without waiting on anything the scheduler might never run.
void testGpuExportProviderQueuedBootstrapCancellationCompletes(Expectations& expectations) {
    runtime::TaskScheduler scheduler(singleCpuWorkerConfig());
    auto blocker = occupyCpuWorker(scheduler);
    expectations.expect(blocker->started.load(std::memory_order_acquire),
                        "queued-cancel: the CPU worker is genuinely occupied");

    auto provider = host::GpuExportProvider::create(
        gpuOptions(true, "/nonexistent/bloom/test/vulkan/loader.so"));
    provider->prepare(scheduler);
    expectations.expect(!provider->retirementComplete(),
                        "queued-cancel: the queued bootstrap is not yet complete");

    scheduler.beginShutdown(); // cancels the queued bootstrap before its lambda can run
    blocker->release.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!provider->retirementComplete() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->retirementComplete(),
                        "queued-cancel: the cancelled bootstrap terminalizes retirement");
    expectations.expect(provider->evaluator() == nullptr,
                        "queued-cancel: no device was ever constructed");
    provider->collectRetired();
    provider.reset();
    const auto quiesceDeadline = std::chrono::steady_clock::now() + 15s;
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < quiesceDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(scheduler.isQuiescent(), "queued-cancel: the scheduler quiesces");
}

// Deterministic concurrent setter/prepare coverage for the GPU display preparer's ownership. A
// concurrent setGpuDisplayCompileOptions() swaps the options and resets the owned preparer while
// prepareGpuDisplayCommand() calls are in flight on other threads. The provider must hand each
// prepare an immutable (preparer, options) pair so the reset can never destroy the preparer
// mid-prepare (use-after-free); every prepare must still observe qualified options and return only
// a typed refusal. Deterministic in outcome: the bogus display/view can never compile, so no
// timing-dependent success or failure is possible. A device is not needed (preparation is
// CPU-only); the fixed qualified paths are non-existent on purpose, and the refusal happens during
// extraction.
void testGpuDisplayConcurrentSetterPrepare(Expectations& expectations) {
    auto resolution = bloom::color::resolveOcioBuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest, runtime::kLinearRec709SceneColorSpaceId);
    expectations.expect(resolution.ready(),
                        "concurrent-prepare: the built-in neutral config resolves");
    if (!resolution.ready()) {
        return;
    }
    auto config = std::move(resolution).takeResolved();
    if (!config.has_value()) {
        return;
    }
    const auto& configRef = *config;

    auto provider = host::GpuExportProvider::create(gpuOptions(false));
    const auto makeOptions = [] {
        runtime::GpuOcioCompileOptions options;
        options.glslangValidatorPath = "/nonexistent/bloom/test/glslangValidator";
        options.spirvValPath = "/nonexistent/bloom/test/spirv-val";
        return options;
    };
    provider->setGpuDisplayCompileOptions(makeOptions());
    expectations.expect(provider->gpuDisplayPreparationAvailable(),
                        "concurrent-prepare: qualified options report the route available");

    std::atomic_bool stop{false};
    std::atomic<std::uint64_t> unexpectedSuccesses{0};
    std::atomic<std::uint64_t> refusals{0};
    std::thread setter([&] {
        while (!stop.load(std::memory_order_acquire)) {
            provider->setGpuDisplayCompileOptions(makeOptions());
        }
    });
    constexpr int kWorkers = 4;
    constexpr int kIterations = 400;
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (int worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&] {
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                const auto prepared =
                    provider->prepareGpuDisplayCommand(configRef, "NoSuchDisplay", "NoSuchView",
                                                       runtime::GpuOcioCommandGeometry{2, 2});
                if (prepared.hasValue()) {
                    unexpectedSuccesses.fetch_add(1, std::memory_order_relaxed);
                } else {
                    refusals.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    stop.store(true, std::memory_order_release);
    setter.join();

    expectations.expect(unexpectedSuccesses.load(std::memory_order_relaxed) == 0,
                        "concurrent-prepare: a bogus display/view never yields a command");
    expectations.expect(refusals.load(std::memory_order_relaxed) ==
                            static_cast<std::uint64_t>(kWorkers) * kIterations,
                        "concurrent-prepare: every prepare returns a typed refusal (the shared "
                        "preparer survives every concurrent reset)");
    expectations.expect(provider->gpuDisplayPreparationAvailable(),
                        "concurrent-prepare: the route stays available after the race");
}

// The production factory path: the provider qualifies the packaged GPU shader tools through the
// shared GpuOcioContextResolver on its CPU-worker bootstrap and uses that ONE shared preparer for
// output display. No setGpuDisplayCompileOptions() call appears anywhere; a build without packaged
// tools reports the route unavailable and this test asserts only the typed fallback.
void testGpuExportProviderFactoryDisplayPreparation(Expectations& expectations) {
#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE
    runtime::TaskScheduler scheduler;
    auto resolver = host::makePackagedGpuOcioResolver(host::currentExecutablePath());
    expectations.expect(resolver != nullptr, "factory: the packaged resolver is composed");
    if (resolver == nullptr) {
        return;
    }
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = false; // display preparation is CPU-only: no device needed here
    options.ocioResolver = resolver;
    auto provider = host::GpuExportProvider::create(std::move(options));
    provider->prepare(scheduler);
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->prepared(), "factory: the bootstrap reaches a terminal state");
    expectations.expect(provider->gpuDisplayPreparationAvailable(),
                        "factory: the resolved context makes the display route available with no "
                        "manual compiler paths");

    auto resolution = bloom::color::resolveOcioBuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest, runtime::kLinearRec709SceneColorSpaceId);
    expectations.expect(resolution.ready(), "factory: the built-in config resolves");
    if (resolution.ready()) {
        if (auto config = std::move(resolution).takeResolved(); config.has_value()) {
            const runtime::GpuOcioCommandGeometry geometry{2, 2};
            const auto viaProvider = provider->prepareGpuDisplayCommand(
                *config, config->displayName(), config->viewName(), geometry);
            expectations.expect(viaProvider.hasValue(),
                                "factory: the provider prepares a display command");
            const auto context = resolver->resolve();
            expectations.expect(context.hasValue(), "factory: the resolver exposes its context");
            if (context.hasValue() && viaProvider.hasValue()) {
                runtime::GpuOcioTransformSpec spec;
                spec.kind = runtime::GpuOcioTransformKind::Display;
                spec.display = std::string(config->displayName());
                spec.view = std::string(config->viewName());
                const auto viaSharedPreparer = context.context->preparer->prepare(
                    *config, spec, geometry, context.context->compileOptions);
                expectations.expect(viaSharedPreparer.hasValue() &&
                                        viaProvider.command->identity() ==
                                            viaSharedPreparer.command->identity(),
                                    "factory: output display uses the SAME shared preparer as the "
                                    "resolved context");
            }
        }
    }
    static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
#else
    // CPU-fallback build: this target publishes no packaged tools, so the shared composition-root
    // helper composes no resolver and the provider must report the display route unavailable. This
    // is the real typed fallback path, asserted rather than skipped.
    runtime::TaskScheduler scheduler;
    const auto resolver = host::makePackagedGpuOcioResolver(host::currentExecutablePath());
    expectations.expect(resolver == nullptr,
                        "factory: no packaged resolver is composed without qualified tools");
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = false; // display preparation is CPU-only: no device needed here
    options.ocioResolver = resolver;
    auto provider = host::GpuExportProvider::create(std::move(options));
    provider->prepare(scheduler);
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->prepared(),
                        "factory: the CPU-fallback bootstrap reaches a terminal state");
    expectations.expect(!provider->gpuDisplayPreparationAvailable(),
                        "factory: without packaged tools the display route is a typed unavailable "
                        "fallback");
    static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
#endif
}
