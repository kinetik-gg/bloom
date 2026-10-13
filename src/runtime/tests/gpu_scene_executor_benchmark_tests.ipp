// Controlled caller cadence around the same production executor, never a second graph runner.
// Included by gpu_scene_executor_tests.cpp inside its anonymous namespace.

[[nodiscard]] double medianValue(std::vector<double> samples) {
    if (samples.empty())
        return 0.0;
    std::ranges::sort(samples);
    const std::size_t middle = samples.size() / 2;
    return samples.size() % 2 == 0 ? (samples[middle - 1] + samples[middle]) / 2.0
                                   : samples[middle];
}
struct BenchmarkMemory final {
    std::uint64_t vmaPayload = 0;
    std::uint64_t vmaBlocks = 0;
    std::uint64_t rootOwnedNative = 0;
    std::uint64_t rootDeviceClaims = 0;
    std::uint64_t rootHostClaims = 0;
};
struct BenchmarkRun final {
    SceneRun scene;
    GpuSceneExecutorCounters before;
    double wallMilliseconds = 0;
    double beginPlanMilliseconds = 0;
    double completedTurnGapMilliseconds = 0;
    std::uint64_t pollTurns = 0;
    BenchmarkMemory memory;
};
struct BenchmarkPhase final {
    GpuDevice& device;
    GpuSceneExecutor& executor;
    std::shared_ptr<bloom::render::GpuProcessingPhaseLease> lease;
    ~BenchmarkPhase() {
        if (lease) {
            const bool retired = !executor.deviceLost() && !executor.hasUnretiredSubmission() &&
                                 !executor.ownerDrainRequired() &&
                                 executor.state() != GpuSceneExecutorJobState::Pending;
            device.retireProcessingPhase(lease->id(), retired);
        }
    }
};

[[nodiscard]] BenchmarkRun
runBenchmarkScene(GpuDevice& device, bloom::render::GpuProcessingPhaseBudget& root,
                  GpuSceneExecutor& executor, const std::shared_ptr<const PreparedGpuScene>& scene,
                  const std::chrono::milliseconds cadence, const bool observeMemory = false,
                  const std::uint64_t requestLimit = UINT64_MAX) {
    BenchmarkRun run;
    const auto grant =
        root.reserve({.fixedHostBytes = 1,
                      .requestedExecutorBytes = kSceneBudget,
                      .minimumExecutorBytes = scene->outputDescriptor().layout().pixelStorageBytes,
                      .sourceFree = true});
    if (!grant.lease || !device.retainProcessingPhase(grant.lease)) {
        run.scene.begin = {GpuSceneExecutorDiagnosticCode::OverBudget,
                           "benchmark capacity-one native phase was refused"};
        return run;
    }
    BenchmarkPhase phase{device, executor, grant.lease};
    const auto sampleMemory = [&] {
        const auto native = device.allocatorMemorySnapshot();
        const auto claims = root.snapshot();
        run.memory.vmaPayload = std::max(run.memory.vmaPayload, native.allocationBytes);
        run.memory.vmaBlocks = std::max(run.memory.vmaBlocks, native.blockBytes);
        run.memory.rootOwnedNative =
            std::max(run.memory.rootOwnedNative,
                     claims.ownedNativeImageBytes + claims.ownedNativeBufferBytes);
        run.memory.rootDeviceClaims =
            std::max(run.memory.rootDeviceClaims, claims.totalDeviceClaimBytes);
        run.memory.rootHostClaims = std::max(run.memory.rootHostClaims, claims.totalHostClaimBytes);
    };
    run.before = executor.counters();
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(30);
    run.scene.begin =
        executor.begin(scene, std::min(grant.lease->executorByteLimit(), requestLimit));
    run.beginPlanMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    if (run.scene.begin.code != GpuSceneExecutorDiagnosticCode::None)
        return run;
    std::optional<std::chrono::steady_clock::time_point> completedAt;
    auto previous = run.before;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto turnStarted = std::chrono::steady_clock::now();
        if (completedAt) {
            run.completedTurnGapMilliseconds +=
                std::chrono::duration<double, std::milli>(turnStarted - *completedAt).count();
            completedAt.reset();
        }
        const auto result = executor.poll();
        const auto observedAt = std::chrono::steady_clock::now();
        ++run.pollTurns;
        const auto current = executor.counters();
        if (result == GpuSceneExecutorPollResult::Pending &&
            current.nativeJobsCompleted > previous.nativeJobsCompleted &&
            current.completedStepContinuations == previous.completedStepContinuations)
            completedAt = observedAt;
        if (observeMemory &&
            (current.dispatches != previous.dispatches || current.uploads != previous.uploads ||
             current.nativeJobsCompleted != previous.nativeJobsCompleted ||
             result != GpuSceneExecutorPollResult::Pending))
            sampleMemory();
        previous = current;
        if (result != GpuSceneExecutorPollResult::Pending) {
            run.scene.finalPoll = result;
            run.wallMilliseconds =
                std::chrono::duration<double, std::milli>(observedAt - started).count();
            run.scene.countersAtReady = current;
            if (result == GpuSceneExecutorPollResult::Ready) {
                run.scene.image = executor.takeImage();
                run.scene.ready = run.scene.image != nullptr;
            }
            return run;
        }
        if (cadence.count() != 0)
            std::this_thread::sleep_for(cadence);
        else
            std::this_thread::yield();
    }
    executor.cancel();
    return run;
}

[[nodiscard]] bloom::render::GpuQueueTimingSample benchmarkQueueSample(GpuSceneExecutor& executor) {
    auto sample = executor.pollQueueTiming();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sample.status == bloom::render::GpuQueueTimingStatus::Pending &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
        sample = executor.pollQueueTiming();
    }
    return sample;
}
struct BenchmarkSamples final {
    std::vector<double> cold;
    std::vector<double> warm;
    std::vector<double> beginPlan;
    std::vector<double> uploadSubmit;
    std::vector<double> uploadFence;
    std::vector<double> processingSubmit;
    std::vector<double> processingFence;
    std::vector<double> turnGaps;
    std::vector<double> queue;
    std::vector<double> turns;
    std::uint64_t dispatches = 0;
    std::uint64_t continuations = 0;
    std::uint64_t liveImagePeak = 0;
    BenchmarkMemory memory;
};

void runBenchmark(Expectations& expectations, GpuDevice& device) {
    using TimingStatus = bloom::render::GpuQueueTimingStatus;
    const CpuCompositionEvaluator oracle;
    bloom::render::GpuProcessingPhaseBudget root(1);
    const auto capacity = device.availableAllocationBudget();
    root.setHostCapacity(kSceneBudget, true);
    root.setDeviceCapacity(capacity.available_bytes,
                           capacity.source !=
                               bloom::render::GpuAllocationBudgetSource::Unavailable);
    expectations.expect(device.bindProcessingRoot(root.rootIdentity()),
                        "benchmark: production device binds the capacity-one root");
    std::cout << "POLL_DEVICE_BUDGET source="
              << (capacity.source == bloom::render::GpuAllocationBudgetSource::MemoryBudget
                      ? "live-memory-budget"
                      : "nominal-device-estimate")
              << " capacity=" << capacity.available_bytes << " requestCapacity=1\n";
    auto clock = bloom::render::GpuQueueTiming::create(device);
    const bool clockSupported = clock && clock->sample().status == TimingStatus::Idle;
    for (const auto size : {std::pair{1280U, 720U}, std::pair{1920U, 1080U}}) {
        const auto allocatorBaseline = device.allocatorMemorySnapshot();
        const auto halfWidth = static_cast<double>(size.first) / 2.0;
        const auto halfHeight = static_cast<double>(size.second) / 2.0;
        const auto plan = twoLayerPlan(
            format(size.first, size.second),
            LayerValues{.position = {halfWidth * 0.45, halfHeight * 0.5}},
            LayerValues{.position = {halfWidth * 0.62, halfHeight * 0.42}, .opacity = 0.75},
            halfWidth, halfHeight, 77000);
        const auto request = requestFor(*plan);
        const CpuGpuSceneBuilder builder;
        std::vector<double> prepareSamples;
        std::vector<double> cpuSamples;
        for (int iteration = 0; iteration < 15; ++iteration) {
            const auto start = std::chrono::steady_clock::now();
            const auto prepared = builder.build(plan, request);
            const auto afterPrepare = std::chrono::steady_clock::now();
            if (!prepared) {
                expectations.expect(false, "benchmark: the fixed real graph prepares");
                return;
            }
            auto cpuRequest = request;
            cpuRequest.bypassOperationCache = true;
            const auto frame = oracle.evaluate(plan, cpuRequest, {});
            const auto afterCpu = std::chrono::steady_clock::now();
            expectations.expect(frame.frame() != nullptr,
                                "benchmark: uncached CPU oracle evaluates");
            if (!frame.frame())
                return;
            if (iteration >= 5) {
                prepareSamples.push_back(
                    std::chrono::duration<double, std::milli>(afterPrepare - start).count());
                cpuSamples.push_back(
                    std::chrono::duration<double, std::milli>(afterCpu - afterPrepare).count());
            }
        }
        auto cpuRequest = request;
        cpuRequest.bypassOperationCache = true;
        const auto reference = oracle.evaluate(plan, cpuRequest, {});
        const auto prepared = builder.build(plan, request);
        auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
        if (!reference.frame() || !prepared || !cache) {
            expectations.expect(false, "benchmark: parity reference/prepared scene/cache exists");
            return;
        }
        std::array<std::unique_ptr<GpuSceneExecutor>, 2> executors;
        for (std::size_t mode = 0; mode < executors.size(); ++mode) {
            GpuSceneExecutorBudgets budgets;
            budgets.collectQueueTimestamps = true;
            budgets.enableNativeBatches = false;
            budgets.continueCompletedStepImmediately = mode != 0;
            auto created = GpuSceneExecutor::create(device, *cache.cache, budgets);
            if (!created) {
                expectations.expect(false, "benchmark: both production polling modes create");
                return;
            }
            executors[mode] = std::move(created.executor);
        }
        for (const auto cadence : {std::chrono::milliseconds(0), std::chrono::milliseconds(2)}) {
            std::array<BenchmarkSamples, 2> samples;
            for (int iteration = 0; iteration < 15; ++iteration) {
                std::vector<Rgba32f> pairedPixels;
                for (std::size_t order = 0; order < executors.size(); ++order) {
                    const auto mode = (order + static_cast<std::size_t>(iteration % 2)) % 2;
                    auto& executor = *executors[mode];
                    auto& stats = samples[mode];
                    cache.cache->clear();
                    device.reclaimReleasedImages();
                    auto cold = runBenchmarkScene(device, root, executor, prepared.scene, cadence);
                    if (!cold.scene.ready) {
                        expectations.expect(false, "benchmark: every cold graph executes natively");
                        std::cerr << cold.scene.begin.message << ' '
                                  << executor.diagnostic().message << '\n';
                        return;
                    }
                    const auto& after = cold.scene.countersAtReady;
                    expectations.expect(
                        after.solidDispatches > cold.before.solidDispatches &&
                            after.translationDispatches > cold.before.translationDispatches &&
                            after.sourceOverDispatches > cold.before.sourceOverDispatches &&
                            after.readbacks == cold.before.readbacks,
                        "benchmark: real cold solid/translation/source-over, zero intermediate "
                        "readbacks");
                    const auto queue = benchmarkQueueSample(executor);
                    expectations.expect(!clockSupported || queue.status == TimingStatus::Available,
                                        "benchmark: supported genuine graph query retires outside "
                                        "pixel timing window");
                    auto pixels = readbackResidentImage(*cold.scene.image, kReadbackBudget);
                    if (!pixels) {
                        expectations.expect(false,
                                            "benchmark: one terminal parity readback succeeds");
                        return;
                    }
                    const bool cpuParity =
                        descriptorMatchesScene(*cold.scene.image, *prepared.scene) &&
                        pixelsClose(pixels.pixels, reference.frame()->processImage().pixels());
                    expectations.expect(
                        cpuParity, "benchmark: both polling modes match CPU pixels/descriptors");
                    if (!cpuParity)
                        return;
                    if (pairedPixels.empty()) {
                        pairedPixels = std::move(pixels.pixels);
                    } else {
                        const bool pairedParity = pixelsExact(pixels.pixels, pairedPixels);
                        expectations.expect(pairedParity,
                                            "benchmark: paired GPU outputs are byte-identical");
                        if (!pairedParity)
                            return;
                    }
                    const auto warm =
                        runBenchmarkScene(device, root, executor, prepared.scene, cadence);
                    expectations.expect(
                        warm.scene.ready &&
                            warm.scene.countersAtReady.dispatches == after.dispatches &&
                            warm.scene.countersAtReady.uploads == after.uploads &&
                            executor.pollQueueTiming().status == TimingStatus::NotRequested,
                        "benchmark: warm graph hits cache without native work or query markers");
                    if (iteration >= 5) {
                        const auto milliseconds = [](const std::uint64_t ns) {
                            return static_cast<double>(ns) / 1.0e6;
                        };
                        stats.cold.push_back(cold.wallMilliseconds);
                        stats.warm.push_back(warm.wallMilliseconds);
                        stats.beginPlan.push_back(cold.beginPlanMilliseconds);
                        stats.uploadSubmit.push_back(milliseconds(
                            after.uploadSubmitNanoseconds - cold.before.uploadSubmitNanoseconds));
                        stats.uploadFence.push_back(
                            milliseconds(after.uploadFenceObservationNanoseconds -
                                         cold.before.uploadFenceObservationNanoseconds));
                        stats.processingSubmit.push_back(
                            milliseconds(after.processingSubmitNanoseconds -
                                         cold.before.processingSubmitNanoseconds));
                        stats.processingFence.push_back(
                            milliseconds(after.processingFenceObservationNanoseconds -
                                         cold.before.processingFenceObservationNanoseconds));
                        stats.turnGaps.push_back(cold.completedTurnGapMilliseconds);
                        stats.turns.push_back(static_cast<double>(cold.pollTurns));
                        if (queue.status == TimingStatus::Available)
                            stats.queue.push_back(milliseconds(queue.nanoseconds));
                        stats.dispatches = after.dispatches - cold.before.dispatches;
                        stats.continuations = after.completedStepContinuations -
                                              cold.before.completedStepContinuations;
                        stats.liveImagePeak = after.peakLiveImageBytes;
                    }
                }
            }
            // Separate observation pass: VMA traversal and driver sampling never contaminate the
            // timing medians. Every submitted native allocation is observable at a poll boundary.
            for (std::size_t mode = 0; mode < executors.size(); ++mode) {
                cache.cache->clear();
                device.reclaimReleasedImages();
                const auto memory =
                    runBenchmarkScene(device, root, *executors[mode], prepared.scene,
                                      std::chrono::milliseconds(2), true);
                expectations.expect(memory.scene.ready,
                                    "benchmark: memory observation graph completes");
                samples[mode].memory = memory.memory;
                static_cast<void>(benchmarkQueueSample(*executors[mode]));
            }
            expectations.expect(samples[0].dispatches == samples[1].dispatches &&
                                    samples[0].continuations == 0 && samples[1].continuations > 0 &&
                                    samples[0].liveImagePeak == samples[1].liveImagePeak &&
                                    samples[0].memory.vmaPayload == samples[1].memory.vmaPayload &&
                                    samples[0].memory.rootOwnedNative ==
                                        samples[1].memory.rootOwnedNative,
                                "benchmark: bounded continuation changes scheduling with equal "
                                "native work/peak ownership");
            for (std::size_t mode = 0; mode < samples.size(); ++mode) {
                const auto& stats = samples[mode];
                std::cout << std::fixed << std::setprecision(3) << "POLL_BASELINE " << size.first
                          << 'x' << size.second << " mode="
                          << (mode == 0 ? "one-step-per-turn" : "immediate-continuation")
                          << " pendingPollDelayMs=" << cadence.count()
                          << " prepareMs=" << medianValue(prepareSamples)
                          << " cpuReferenceMs=" << medianValue(cpuSamples)
                          << " coldMs=" << medianValue(stats.cold)
                          << " warmMs=" << medianValue(stats.warm)
                          << " beginPlanMs=" << medianValue(stats.beginPlan)
                          << " uploadSubmitMs=" << medianValue(stats.uploadSubmit)
                          << " uploadFenceObserveMs=" << medianValue(stats.uploadFence)
                          << " processingSubmitMs=" << medianValue(stats.processingSubmit)
                          << " processingFenceObserveMs=" << medianValue(stats.processingFence)
                          << " completedTurnGapMs=" << medianValue(stats.turnGaps)
                          << " pollTurns=" << medianValue(stats.turns) << " genuineQueueSpanMs=";
                if (stats.queue.empty())
                    std::cout << "Unavailable";
                else
                    std::cout << medianValue(stats.queue);
                std::cout << " queueSamples=" << stats.queue.size()
                          << " nativeDispatches=" << stats.dispatches
                          << " immediateContinuations=" << stats.continuations
                          << " liveImagePeak=" << stats.liveImagePeak
                          << " vmaPayloadPeak=" << stats.memory.vmaPayload
                          << " vmaBlockPeak=" << stats.memory.vmaBlocks
                          << " rootOwnedNativePeak=" << stats.memory.rootOwnedNative
                          << " rootDeviceClaimPeak=" << stats.memory.rootDeviceClaims
                          << " rootHostClaimPeak=" << stats.memory.rootHostClaims
                          << " CPUparity=pass pairedGPU=byte-exact\n";
            }
            std::cout << "POLL_COMPARISON " << size.first << 'x' << size.second
                      << " pendingPollDelayMs=" << cadence.count() << " coldSavedMs="
                      << medianValue(samples[0].cold) - medianValue(samples[1].cold)
                      << " completedTurnGapSavedMs="
                      << medianValue(samples[0].turnGaps) - medianValue(samples[1].turnGaps)
                      << " genuineQueueSpanSavedMs=";
            if (samples[0].queue.empty() || samples[1].queue.empty())
                std::cout << "Unavailable";
            else
                std::cout << medianValue(samples[0].queue) - medianValue(samples[1].queue);
            std::cout << " liveImagePeakEqual="
                      << (samples[0].liveImagePeak == samples[1].liveImagePeak)
                      << " vmaPayloadPeakEqual="
                      << (samples[0].memory.vmaPayload == samples[1].memory.vmaPayload)
                      << " rootOwnedNativePeakEqual="
                      << (samples[0].memory.rootOwnedNative == samples[1].memory.rootOwnedNative)
                      << '\n';
        }
        cache.cache->clear();
        executors = {};
        device.reclaimReleasedImages();
        expectations.expect(
            root.snapshot().admittedPhases == 0 && root.snapshot().ownedNativeImages == 0 &&
                root.snapshot().ownedNativeBuffers == 0 &&
                root.snapshot().totalDeviceClaimBytes == 0 &&
                root.snapshot().totalHostClaimBytes == 0,
            "benchmark: capacity-one phases and all native backing retire to zero claims");
        const auto settled = device.allocatorMemorySnapshot();
        expectations.expect(settled.allocationBytes == allocatorBaseline.allocationBytes &&
                                settled.allocationCount == allocatorBaseline.allocationCount,
                            "benchmark: actual VMA payload/count returns to the same baseline");
        std::cout << "POLL_SETTLED " << size.first << 'x' << size.second
                  << " vmaPayload=" << settled.allocationBytes
                  << " vmaBlocks=" << settled.blockBytes
                  << " rootDevice=" << root.snapshot().totalDeviceClaimBytes
                  << " rootHost=" << root.snapshot().totalHostClaimBytes << '\n';
    }
    std::cout
        << "Boundaries: paired fixed content-cache-cold graph with retained pipelines, five "
           "warmups "
           "and ten measured samples per mode; "
           "alternating order. 2ms is a controlled pending-poll delay matching the service "
           "interval; "
           "live notifications can wake earlier. GPU queue spans include idle/poll/submission "
           "gaps; "
           "fence observation is host wall time. Terminal parity readback, query retrieval and "
           "separate "
           "VMA peak observation are outside timing windows. Blocks, payload and overlapping quota "
           "claims remain separate. Viewer/presentation excluded; no application FPS claim.\n";
}
