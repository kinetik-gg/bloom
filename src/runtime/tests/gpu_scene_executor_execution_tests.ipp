// GPU scene executor execution tests: cancellation, request/command/structural budgets, foreign
// device and thread refusal, and the live-unique-pin budget accounting (including aliased inputs).
// Included by gpu_scene_executor_tests.cpp inside its anonymous namespace.

void testCancellation(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "cancel: cache created");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "cancel: executor created");
    if (!executor) {
        return;
    }
    {
        const auto plan = basicPlan();
        const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
        expectations.expect(prepared.hasValue(), "cancel: the scene prepares");
        if (!prepared) {
            return;
        }
        const auto begun = executor.executor->begin(prepared.scene, kSceneBudget);
        expectations.expect(begun.code == GpuSceneExecutorDiagnosticCode::None,
                            "cancel: begin accepts");
        executor.executor->cancel();
        const auto result = executor.executor->poll();
        expectations.expect(result == GpuSceneExecutorPollResult::Failure,
                            "cancel: a cancelled job fails closed");
        expectations.expect(executor.executor->diagnostic().code ==
                                GpuSceneExecutorDiagnosticCode::Cancelled,
                            "cancel: the diagnostic is Cancelled");
        expectations.expect(executor.executor->takeImage() == nullptr,
                            "cancel: no image is published after cancel");
        expectations.expect(executor.executor->counters().dispatches == 0,
                            "cancel: cancelling before the first poll never dispatches");
    }
    {
        const auto plan = fractionalPlan();
        const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
        expectations.expect(prepared.hasValue(), "cancel-live: the scene prepares");
        if (!prepared) {
            return;
        }
        const auto begun = executor.executor->begin(prepared.scene, kSceneBudget);
        expectations.expect(begun.code == GpuSceneExecutorDiagnosticCode::None,
                            "cancel-live: begin accepts");
        const auto firstPoll = executor.executor->poll();
        expectations.expect(firstPoll == GpuSceneExecutorPollResult::Pending,
                            "cancel-live: the first poll submits and stays pending");
        executor.executor->cancel();
        GpuSceneExecutorPollResult final = GpuSceneExecutorPollResult::Pending;
        for (std::uint64_t iteration = 0; iteration < kMaxPollIterations; ++iteration) {
            final = executor.executor->poll();
            if (final != GpuSceneExecutorPollResult::Pending) {
                break;
            }
            std::this_thread::yield();
        }
        expectations.expect(final == GpuSceneExecutorPollResult::Failure,
                            "cancel-live: the cancelled job fails closed");
        expectations.expect(executor.executor->diagnostic().code ==
                                GpuSceneExecutorDiagnosticCode::Cancelled,
                            "cancel-live: the diagnostic is Cancelled");
        expectations.expect(executor.executor->takeImage() == nullptr,
                            "cancel-live: no image is published");
        expectations.expect(!executor.executor->ownerDrainRequired() &&
                                !executor.executor->deviceLost(),
                            "cancel-live: a proven cancellation needs no owner drain");
    }
    // Recovery on the vector-coverage path specifically: the same fractional scene that was
    // cancelled live above must still dispatch its native coverage producer and publish.
    const auto plan = fractionalPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    const auto recovered = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(recovered.ready, "cancel: the executor is usable after cancellation");
    expectations.expect(executor.executor->counters().coverageDispatches > 0,
                        "cancel: the coverage producer recovers and dispatches after cancellation");
}

void testTinyBudget(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "budget: cache created");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "budget: executor created");
    if (!executor) {
        return;
    }
    // The vector-coverage path: a tiny budget must be refused before any GpuPathCoverage dispatch.
    const auto plan = fractionalPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "budget: the scene prepares");
    if (!prepared) {
        return;
    }
    const auto refused = executor.executor->begin(prepared.scene, 1);
    expectations.expect(refused.code == GpuSceneExecutorDiagnosticCode::OverBudget,
                        "budget: a tiny request budget is refused before Vulkan");
    expectations.expect(executor.executor->counters().coverageDispatches == 0,
                        "budget: a refused coverage scene dispatched no native producer");
    expectations.expect(executor.executor->state() == GpuSceneExecutorJobState::Idle,
                        "budget: a refusal leaves the executor idle");
    expectations.expect(executor.executor->counters().budgetRefusals >= 1,
                        "budget: the refusal is counted");
    const auto retried = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(retried.ready, "budget: the executor remains usable with a real budget");
}

// A configured maximum is an upper bound, not an allocation request. A huge configured maximum
// must never refuse the pipeline or a small request, and a configured maximum below the actual
// request must refuse that request cleanly while leaving the executor usable.
void testPermissiveMaximum(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "permissive: cache created");
    if (!cache) {
        return;
    }

    // Huge configured maximum, small actual request: creation and dispatch succeed.
    GpuSceneExecutorBudgets huge;
    huge.maxImageBytes = std::uint64_t{1} << 62;
    huge.maxMetadataBytes = std::uint64_t{1} << 62;
    huge.maxAffineMetadataBytes = std::uint64_t{1} << 62;
    auto permissive = GpuSceneExecutor::create(device, *cache.cache, huge);
    expectations.expect(permissive.hasValue(),
                        "permissive: a huge configured maximum does not refuse the executor");
    if (permissive) {
        const auto plan = basicPlan();
        const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
        expectations.expect(prepared.hasValue(), "permissive: the small scene prepares");
        if (prepared) {
            const auto run = runScene(*permissive.executor, prepared.scene, kSceneBudget);
            expectations.expect(run.ready,
                                "permissive: a small request runs under a huge configured maximum");
        }
    }

    // Tiny configured maximum: the pipeline is still created (no whole-renderer refusal), and an
    // actual request above the configured maximum is refused cleanly. A fresh cache prevents a
    // content-cache hit from cutting the subtree before the per-step budget is enforced.
    auto limitedCache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(limitedCache.hasValue(), "permissive: the limited cache is created");
    if (!limitedCache) {
        return;
    }
    GpuSceneExecutorBudgets tiny;
    tiny.maxImageBytes = 1;
    auto limited = GpuSceneExecutor::create(device, *limitedCache.cache, tiny);
    expectations.expect(limited.hasValue(),
                        "permissive: a tiny configured maximum still creates the executor");
    if (limited) {
        const auto plan = basicPlan();
        const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
        expectations.expect(prepared.hasValue(), "permissive: the scene prepares under a tiny max");
        if (prepared) {
            const auto refused = runScene(*limited.executor, prepared.scene, kSceneBudget);
            expectations.expect(!refused.ready,
                                "permissive: an oversized actual request is refused, not rendered");
            expectations.expect(refused.image == nullptr,
                                "permissive: the refusal publishes no image");
        }
    }
}

// An actual 6000x4000 RGBA32F solid (384 MB) must dispatch natively when the configured maximum is
// capacity-sized and the device's real maxResourceSize allows it. This is the >256 MiB gate.
void testLargeImageNative(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "large: cache created");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "large: executor created");
    if (!executor) {
        return;
    }
    const auto plan = largeSolidPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "large: the 6000x4000 scene prepares");
    if (!prepared) {
        return;
    }
    const auto run = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(run.ready, "large: the 6000x4000 solid dispatches natively");
    if (run.ready) {
        expectations.expect(
            run.countersAtReady.solidDispatches + run.countersAtReady.coveredSolidDispatches +
                    run.countersAtReady.translationDispatches +
                    run.countersAtReady.sourceOverDispatches + run.countersAtReady.uploads >
                0,
            "large: at least one native operation ran for the 384 MB image");
    }
}

void testStructureRefusal(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "structure: cache created");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "structure: executor created");
    if (!executor) {
        return;
    }
    const auto nullResult = executor.executor->begin(nullptr, kSceneBudget);
    expectations.expect(nullResult.code == GpuSceneExecutorDiagnosticCode::InvalidArgument,
                        "structure: a null scene is refused");

    const auto plan = basicPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "structure: the scene prepares");
    if (!prepared) {
        return;
    }
    GpuSceneExecutorBudgets tight;
    tight.maxCommands = 1;
    auto limitedCache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(limitedCache.hasValue(), "structure: the limited cache is created");
    if (!limitedCache) {
        return;
    }
    auto limited = GpuSceneExecutor::create(device, *limitedCache.cache, tight);
    expectations.expect(limited.hasValue(), "structure: the limited executor is created");
    if (limited) {
        const auto refused = limited.executor->begin(prepared.scene, kSceneBudget);
        expectations.expect(refused.code == GpuSceneExecutorDiagnosticCode::TooManyCommands,
                            "structure: the command ceiling is enforced before Vulkan");
    }
}

void testForeignDeviceAndThread(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache,
                                GpuDevice* foreignDevice) {
    auto created = GpuSceneExecutor::create(device, cache);
    expectations.expect(created.hasValue(), "foreign: the owner thread creates an executor");
    if (!created) {
        return;
    }
    if (foreignDevice != nullptr) {
        expectations.expect(!created.executor->isBoundTo(*foreignDevice),
                            "foreign: a different device generation is not bound");
        auto foreignCache =
            GpuSceneCache::create(*foreignDevice, GpuSceneCacheBudgets{kCacheBudget});
        expectations.expect(foreignCache.hasValue(), "foreign: a foreign cache is created");
        if (foreignCache) {
            auto mismatched = GpuSceneExecutor::create(*foreignDevice, cache);
            expectations.expect(mismatched.diagnostic.code ==
                                    GpuSceneExecutorDiagnosticCode::InvalidArgument,
                                "foreign: a cache from another device is refused");
        }
    }

    const auto plan = basicPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "foreign: the scene prepares");
    if (!prepared) {
        return;
    }
    std::atomic<int> observed{static_cast<int>(GpuSceneExecutorDiagnosticCode::None)};
    std::thread worker([&]() {
        const auto result = created.executor->begin(prepared.scene, kSceneBudget);
        observed.store(static_cast<int>(result.code));
    });
    worker.join();
    expectations.expect(static_cast<GpuSceneExecutorDiagnosticCode>(observed.load()) ==
                            GpuSceneExecutorDiagnosticCode::WrongThread,
                        "foreign: a foreign-thread begin is WrongThread");
}

// ---- live-budget accounting -------------------------------------------------------------------

[[nodiscard]] std::optional<std::string> firstSolidKey(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* solid = std::get_if<GpuSceneSolidCommand>(&command)) {
            return solid->semanticKey;
        }
    }
    return std::nullopt;
}

// A long sequential graph with a tiny cache: cumulative allocation exceeds the budget but the live
// peak fits, so it must be accepted. A one-byte budget is refused and leaves the executor usable.
void testLiveBudgetLongGraph(Expectations& expectations, GpuDevice& device) {
    const auto plan = basicPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "live: the scene prepares");
    if (!prepared) {
        return;
    }
    auto measureCache = GpuSceneCache::create(device, GpuSceneCacheBudgets{1});
    auto measureExec = GpuSceneExecutor::create(device, *measureCache.cache);
    expectations.expect(measureCache.hasValue() && measureExec.hasValue(), "live: harness created");
    if (!measureCache || !measureExec) {
        return;
    }
    const auto measured = runScene(*measureExec.executor, prepared.scene, kSceneBudget);
    expectations.expect(measured.ready, "live: the measuring run completes");
    if (!measured.ready) {
        return;
    }
    const auto peak = measured.countersAtReady.peakLiveImageBytes;
    const auto cumulative = measured.countersAtReady.cumulativeProducedImageBytes;
    expectations.expect(peak > 0, "live: the live peak is non-zero");
    expectations.expect(cumulative > peak, "live: cumulative allocation exceeds the live peak");
    expectations.expect(measureCache.cache->entryCount() == 0,
                        "live: the tiny cache retained nothing");

    auto tightCache = GpuSceneCache::create(device, GpuSceneCacheBudgets{1});
    auto tightExec = GpuSceneExecutor::create(device, *tightCache.cache);
    expectations.expect(tightCache.hasValue() && tightExec.hasValue(),
                        "live: tight harness created");
    if (!tightCache || !tightExec) {
        return;
    }
    const auto tight = runScene(*tightExec.executor, prepared.scene, peak);
    expectations.expect(tight.ready, "live: a graph whose live peak fits is accepted");
    expectations.expect(tight.countersAtReady.cumulativeProducedImageBytes > peak,
                        "live: the accepted run allocated cumulatively beyond the budget");
    expectations.expect(tight.countersAtReady.peakLiveImageBytes <= peak,
                        "live: the accepted run's live peak stayed within the budget");

    const auto refused = tightExec.executor->begin(prepared.scene, 1);
    expectations.expect(refused.code == GpuSceneExecutorDiagnosticCode::OverBudget,
                        "live: a one-byte budget is refused before Vulkan");
    expectations.expect(tightExec.executor->counters().budgetRefusals >= 1,
                        "live: the refusal is counted");
    const auto recovered = runScene(*tightExec.executor, prepared.scene, peak);
    expectations.expect(recovered.ready, "live: the executor is usable after the refusal");
}

// A tight budget must refuse when a cached input pinned for this request already exceeds it, before
// any native work.
void testTightBudgetWithCachedInputs(Expectations& expectations, GpuDevice& device) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto exec = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(cache.hasValue() && exec.hasValue(), "cached-budget: harness created");
    if (!cache || !exec) {
        return;
    }
    const auto firstPlan = basicPlan();
    const auto firstPrepared = CpuGpuSceneBuilder{}.build(firstPlan, requestFor(*firstPlan));
    expectations.expect(firstPrepared.hasValue(), "cached-budget: the warming scene prepares");
    if (!firstPrepared) {
        return;
    }
    const auto firstRun = runScene(*exec.executor, firstPrepared.scene, kSceneBudget);
    expectations.expect(firstRun.ready, "cached-budget: the warming scene completes");
    if (!firstRun.ready) {
        return;
    }
    // The warmed output is now a cached pin this request would hold: a one-byte budget must refuse
    // it at begin, before any Vulkan work.
    const auto cachedRefused = exec.executor->begin(firstPrepared.scene, 1);
    expectations.expect(
        cachedRefused.code == GpuSceneExecutorDiagnosticCode::OverBudget,
        "cached-budget: a cached output pin over a one-byte budget is refused at begin");
    expectations.expect(exec.executor->state() == GpuSceneExecutorJobState::Idle,
                        "cached-budget: the cached refusal leaves the executor idle");

    // Move the top layer so the output misses while the bottom layer is a cached input.
    const auto movedPlan =
        twoLayerPlan(format(16, 12), LayerValues{.position = {13.0, 9.5}, .opacity = 1.0},
                     LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0, 1000);
    const auto moved = CpuGpuSceneBuilder{}.build(movedPlan, requestFor(*movedPlan));
    expectations.expect(moved.hasValue(), "cached-budget: the moved scene prepares");
    if (!moved) {
        return;
    }
    const auto refused = exec.executor->begin(moved.scene, 1);
    expectations.expect(
        refused.code == GpuSceneExecutorDiagnosticCode::OverBudget,
        "cached-budget: a cached input under a one-byte budget is refused at begin");
    expectations.expect(exec.executor->state() == GpuSceneExecutorJobState::Idle,
                        "cached-budget: the refusal leaves the executor idle");
    const auto recovered = runScene(*exec.executor, moved.scene, kSceneBudget);
    expectations.expect(recovered.ready,
                        "cached-budget: the executor is usable with a real budget");
}

// A wide merge of several DISTINCT full-size layers must stay within a live-peak bound of
// accumulator + one foreground + the next output. The retired order planned every foreground
// subtree first, so a full-size layer was pinned per layer until its source-over ran and a
// six-layer merge needed roughly eight frames; the interleaved planner holds about three. The
// bound is a measured per-step native peak (independent of hardware VMA padding), so this test
// accepts the interleaved order and refuses the old one. The constrained run must still reach
// Ready and match the CPU oracle pixel for pixel.
void testWideMergeLivePeakBound(Expectations& expectations, GpuDevice& device,
                                const CpuCompositionEvaluator& oracle) {
    constexpr std::uint32_t kLayerCount = 6;
    constexpr std::uint64_t kPeakFrameBound = 4;
    const auto plan = wideMergePlan(format(256, 192), kLayerCount, 41000);
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "wide: the scene prepares");
    if (!prepared) {
        return;
    }
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{1});
    auto exec = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(cache.hasValue() && exec.hasValue(), "wide: harness created");
    if (!cache || !exec) {
        return;
    }
    const auto probe = runScene(*exec.executor, prepared.scene, kSceneBudget);
    expectations.expect(probe.ready, "wide: the unbounded probe completes");
    if (!probe.ready) {
        return;
    }
    const auto perStepPeak = probe.countersAtReady.peakStepImageBytes;
    expectations.expect(perStepPeak > 0, "wide: the probe measured a native per-step allocation");
    if (perStepPeak == 0) {
        return;
    }
    const auto budget = kPeakFrameBound * perStepPeak;
    const auto run = runScene(*exec.executor, prepared.scene, budget);
    expectations.expect(run.ready, "wide: the constrained live-peak budget is accepted");
    if (!run.ready) {
        return;
    }
    expectations.expect(run.countersAtReady.peakLiveImageBytes <= budget,
                        "wide: the live peak stays within the constrained budget");
    expectations.expect(run.countersAtReady.cumulativeProducedImageBytes > budget,
                        "wide: cumulative allocation exceeds the constrained live budget");
    expectations.expect(run.countersAtReady.intermediatePinsReleased > 0,
                        "wide: intermediates are released at their last consumer");

    auto oracleRequest = requestFor(*plan);
    oracleRequest.bypassOperationCache = true;
    const auto frame = oracle.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, "wide: the CPU oracle evaluates");
    if (frame.frame() == nullptr) {
        return;
    }
    const auto& cpuImage = frame.frame()->processImage();
    const GpuImageReadback readback = readbackResidentImage(*run.image, kReadbackBudget);
    expectations.expect(readback.hasValue(), "wide: the test readback succeeds");
    if (!readback) {
        return;
    }
    expectations.expect(readback.pixels.size() == cpuImage.pixels().size(),
                        "wide: the pixel count matches the CPU frame");
    if (readback.pixels.size() != cpuImage.pixels().size()) {
        return;
    }
    expectations.expect(pixelsClose(readback.pixels, cpuImage.pixels()),
                        "wide: pixels are within the 2e-6 process gate");
}

// Two identical solids share one semantic key and resolve to the same cached image: the alias is
// pinned under two command indexes but charged once, and the alias bytes are reported.
void testAliasedInputChargedOnce(Expectations& expectations, GpuDevice& device) {
    const Color4d aliasedColor{0.4, 0.6, 0.2, 0.5};
    const auto warmPlan =
        twoSolidPlan(format(16, 12), aliasedColor, LayerValues{.position = {4.0, 3.5}},
                     aliasedColor, LayerValues{.position = {9.0, 7.5}}, 6.0, 5.0, 31000);
    const auto runPlan =
        twoSolidPlan(format(16, 12), aliasedColor, LayerValues{.position = {5.0, 3.5}},
                     aliasedColor, LayerValues{.position = {9.0, 6.5}}, 6.0, 5.0, 31000);
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto exec = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(cache.hasValue() && exec.hasValue(), "alias: harness created");
    if (!cache || !exec) {
        return;
    }
    const auto warmPrepared = CpuGpuSceneBuilder{}.build(warmPlan, requestFor(*warmPlan));
    const auto runPrepared = CpuGpuSceneBuilder{}.build(runPlan, requestFor(*runPlan));
    expectations.expect(warmPrepared.hasValue() && runPrepared.hasValue(),
                        "alias: both plans prepare");
    if (!warmPrepared || !runPrepared) {
        return;
    }
    std::size_t translationCommands = 0;
    for (const auto& command : runPrepared.scene->commands()) {
        if (std::holds_alternative<GpuSceneTranslationCommand>(command)) {
            ++translationCommands;
        }
    }
    const bool runUsesSolids = translationCommands >= 2;
    const auto warm = runScene(*exec.executor, warmPrepared.scene, kSceneBudget);
    expectations.expect(warm.ready, "alias: the warming run completes");
    if (!warm.ready) {
        return;
    }
    const auto key = firstSolidKey(*runPrepared.scene);
    expectations.expect(key.has_value(), "alias: a solid key is present");
    const auto solidImage = key.has_value() ? cache.cache->find(*key) : nullptr;
    expectations.expect(solidImage != nullptr, "alias: the shared solid image is cached");
    const std::uint64_t solidBytes = solidImage != nullptr ? solidImage->allocationBytes() : 0;
    expectations.expect(runUsesSolids && solidBytes > 0,
                        "alias: the identical solids are reachable and cached");

    const auto aliased = runScene(*exec.executor, runPrepared.scene, kSceneBudget);
    expectations.expect(aliased.ready, "alias: the aliased run completes");
    expectations.expect(aliased.countersAtReady.aliasedImagePinBytes >= solidBytes,
                        "alias: the shared image was charged once and the duplicate recorded");
    const auto aliasedPeak = aliased.countersAtReady.peakLiveImageBytes;
    const auto rerun = runScene(*exec.executor, runPrepared.scene, aliasedPeak);
    expectations.expect(rerun.ready, "alias: the aliased scene fits its single-count live peak");

    // Control: two distinct solids never alias.
    const auto distinctWarm = twoSolidPlan(
        format(16, 12), Color4d{0.5, 0.25, 0.125, 1.0}, LayerValues{.position = {4.0, 3.5}},
        Color4d{0.125, 0.375, 0.75, 0.5}, LayerValues{.position = {9.0, 7.5}}, 6.0, 5.0, 32000);
    const auto distinctRun = twoSolidPlan(
        format(16, 12), Color4d{0.5, 0.25, 0.125, 1.0}, LayerValues{.position = {5.0, 3.5}},
        Color4d{0.125, 0.375, 0.75, 0.5}, LayerValues{.position = {9.0, 6.5}}, 6.0, 5.0, 32000);
    auto cache2 = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto exec2 = GpuSceneExecutor::create(device, *cache2.cache);
    if (cache2 && exec2) {
        const auto warm2 = CpuGpuSceneBuilder{}.build(distinctWarm, requestFor(*distinctWarm));
        const auto run2 = CpuGpuSceneBuilder{}.build(distinctRun, requestFor(*distinctRun));
        if (warm2 && run2) {
            expectations.expect(runScene(*exec2.executor, warm2.scene, kSceneBudget).ready,
                                "alias: the distinct warming run completes");
            const auto distinct = runScene(*exec2.executor, run2.scene, kSceneBudget);
            expectations.expect(distinct.ready, "alias: the distinct run completes");
            expectations.expect(distinct.countersAtReady.aliasedImagePinBytes == 0,
                                "alias: distinct inputs are never counted as an alias");
        }
    }
}
