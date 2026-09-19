// GPU scene executor native ownership and retirement tests: the native-first teardown pin
// ownership proof and the proof-only fault-injected deadline/unknown-fence/device-loss contracts.
// Included by gpu_scene_executor_tests.cpp inside its anonymous namespace.

// The executor pins a cached input for a composite job; the native op retains the same image while
// the submission is in flight. Owner-thread, native-first destruction must drain (or quarantine)
// the entire native Impl, which owns that input pin, before the executor releases its own pins.
// This reads the real shared ownership to prove it: the input's use_count rises above the
// cache+probe baseline while the job is live and falls back after teardown, with no executor-global
// leak list.
void testNativeOwnershipDuringTeardown(Expectations& expectations, GpuDevice& device) {
    const Color4d colorA{0.5, 0.25, 0.125, 1.0};
    const Color4d colorB{0.125, 0.375, 0.75, 0.5};
    const auto warmPlan =
        twoSolidPlan(format(16, 12), colorA, LayerValues{.position = {4.0, 3.5}}, colorB,
                     LayerValues{.position = {9.0, 7.5}}, 6.0, 5.0, 41000);
    const auto runPlan = twoSolidPlan(format(16, 12), colorA, LayerValues{.position = {5.0, 3.5}},
                                      colorB, LayerValues{.position = {9.0, 6.5}}, 6.0, 5.0, 41000);
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    expectations.expect(cache.hasValue(), "ownership: cache created");
    if (!cache) {
        return;
    }
    const auto warmPrepared = CpuGpuSceneBuilder{}.build(warmPlan, requestFor(*warmPlan));
    const auto runPrepared = CpuGpuSceneBuilder{}.build(runPlan, requestFor(*runPlan));
    expectations.expect(warmPrepared.hasValue() && runPrepared.hasValue(),
                        "ownership: both plans prepare");
    if (!warmPrepared || !runPrepared) {
        return;
    }
    // The first dispatched step is the bottom merge foreground's translation, which consumes its
    // (cached) solid input.
    std::optional<std::string> firstStepInputKey;
    for (const auto& command : runPrepared.scene->commands()) {
        const auto* merge = std::get_if<GpuSceneMergeCommand>(&command);
        if (merge == nullptr || merge->foregrounds.empty()) {
            continue;
        }
        const auto bottom = merge->foregrounds.front();
        if (bottom == bloom::runtime::kInvalidGpuSceneCommand ||
            static_cast<std::size_t>(bottom) >= runPrepared.scene->commands().size()) {
            continue;
        }
        if (const auto* translation =
                std::get_if<GpuSceneTranslationCommand>(&runPrepared.scene->commands()[bottom]);
            translation != nullptr) {
            firstStepInputKey = keyOf(runPrepared.scene->commands()[translation->input]);
        }
        break;
    }
    expectations.expect(firstStepInputKey.has_value(),
                        "ownership: the first native step has a cached input key");
    if (!firstStepInputKey.has_value()) {
        return;
    }

    auto exec = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(exec.hasValue(), "ownership: executor created");
    if (!exec) {
        return;
    }
    const auto warm = runScene(*exec.executor, warmPrepared.scene, kSceneBudget);
    expectations.expect(warm.ready, "ownership: the warming run completes");
    if (!warm.ready) {
        return;
    }
    auto probe = cache.cache->find(*firstStepInputKey);
    expectations.expect(probe != nullptr, "ownership: the input is cached");
    if (probe == nullptr) {
        return;
    }
    const auto baseline = probe.use_count();

    expectations.expect(exec.executor->begin(runPrepared.scene, kSceneBudget).code ==
                            GpuSceneExecutorDiagnosticCode::None,
                        "ownership: begin accepts the run scene");
    expectations.expect(exec.executor->poll() == GpuSceneExecutorPollResult::Pending,
                        "ownership: the first poll dispatches the composite step");
    const auto during = probe.use_count();
    expectations.expect(during >= baseline + 2,
                        "ownership: the executor AND the native op retain the in-flight input");

    // Owner-thread native-first destruction while the composite job is live.
    exec.executor.reset();
    expectations.expect(!GpuSceneExecutor::teardownDrainIncomplete(),
                        "ownership: the healthy native drain is not reported incomplete");
    const auto after = probe.use_count();
    expectations.expect(after < during,
                        "ownership: the native drain released the input before executor pins");
}

// ---- native retirement / drain contracts ------------------------------------------------------

void testNativeRetirementContracts(Expectations& expectations, GpuDevice& device) {
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
    namespace fault = bloom::render::gpu_scene_executor_fault;
    using Fault = fault::PollFault;
    fault::clear();

    const auto plan = basicPlan();
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "drain: the scene prepares");
    if (!prepared) {
        return;
    }

    // 1) Deadline while a real submitted job's poll is stalled: fail the logical request promptly,
    //    retain the unproven submission, refuse reuse, then drain and reuse.
    {
        GpuSceneExecutorBudgets budgets;
        budgets.jobDeadlineMilliseconds = 0;
        auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
        auto exec = GpuSceneExecutor::create(device, *cache.cache, budgets);
        expectations.expect(exec.hasValue(), "drain: the deadline executor is created");
        if (exec) {
            const auto begun = exec.executor->begin(prepared.scene, kSceneBudget);
            expectations.expect(begun.code == GpuSceneExecutorDiagnosticCode::None,
                                "drain: begin accepts");
            expectations.expect(exec.executor->poll() == GpuSceneExecutorPollResult::Pending,
                                "drain: the first poll submits and stays pending");
            fault::set(Fault::StallPending);
            const auto timedOut = exec.executor->poll();
            expectations.expect(timedOut == GpuSceneExecutorPollResult::Failure,
                                "drain: the deadline fails the request promptly");
            expectations.expect(exec.executor->diagnostic().code ==
                                    GpuSceneExecutorDiagnosticCode::NativeTimeout,
                                "drain: the diagnostic is NativeTimeout");
            expectations.expect(exec.executor->ownerDrainRequired(),
                                "drain: the unproven submission requires owner drain");
            expectations.expect(exec.executor->takeImage() == nullptr,
                                "drain: no image is published at the deadline");
            expectations.expect(exec.executor->begin(prepared.scene, kSceneBudget).code ==
                                    GpuSceneExecutorDiagnosticCode::OwnerDrainRequired,
                                "drain: reuse is refused while the submission is unproven");
            fault::clear();
            for (std::uint64_t i = 0; i < kMaxPollIterations && exec.executor->ownerDrainRequired();
                 ++i) {
                static_cast<void>(exec.executor->poll());
                std::this_thread::yield();
            }
            expectations.expect(!exec.executor->ownerDrainRequired(),
                                "drain: the owner drained the submission");
            expectations.expect(exec.executor->begin(prepared.scene, kSceneBudget).code ==
                                    GpuSceneExecutorDiagnosticCode::None,
                                "drain: the executor is reusable after the drain");
            exec.executor->cancel();
            static_cast<void>(exec.executor->poll());
        }
    }

    // 2) Unknown fence: a Failure that does not prove retirement retains and poisons, then drains.
    {
        auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
        auto exec = GpuSceneExecutor::create(device, *cache.cache);
        expectations.expect(exec.hasValue(), "unknown: the executor is created");
        if (exec) {
            static_cast<void>(exec.executor->begin(prepared.scene, kSceneBudget));
            static_cast<void>(exec.executor->poll());
            fault::set(Fault::UnknownFence);
            const auto failed = exec.executor->poll();
            expectations.expect(failed == GpuSceneExecutorPollResult::Failure,
                                "unknown: the unproven failure fails the request");
            expectations.expect(exec.executor->diagnostic().code ==
                                    GpuSceneExecutorDiagnosticCode::NativeUnproven,
                                "unknown: the diagnostic is NativeUnproven");
            expectations.expect(exec.executor->ownerDrainRequired(),
                                "unknown: the unproven submission requires owner drain");
            expectations.expect(exec.executor->takeImage() == nullptr,
                                "unknown: no image is published");
            expectations.expect(exec.executor->begin(prepared.scene, kSceneBudget).code ==
                                    GpuSceneExecutorDiagnosticCode::OwnerDrainRequired,
                                "unknown: reuse is refused while unproven");
            for (std::uint64_t i = 0; i < kMaxPollIterations && exec.executor->ownerDrainRequired();
                 ++i) {
                static_cast<void>(exec.executor->poll());
                std::this_thread::yield();
            }
            expectations.expect(!exec.executor->ownerDrainRequired(),
                                "unknown: the owner drained the submission");
        }
    }

    // 3) Device loss is mapped distinctly and is terminal.
    {
        auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
        auto exec = GpuSceneExecutor::create(device, *cache.cache);
        expectations.expect(exec.hasValue(), "lost: the executor is created");
        if (exec) {
            static_cast<void>(exec.executor->begin(prepared.scene, kSceneBudget));
            static_cast<void>(exec.executor->poll());
            fault::set(Fault::DeviceLost);
            const auto failed = exec.executor->poll();
            expectations.expect(failed == GpuSceneExecutorPollResult::Failure,
                                "lost: the request fails");
            expectations.expect(exec.executor->diagnostic().code ==
                                    GpuSceneExecutorDiagnosticCode::DeviceLost,
                                "lost: the diagnostic is DeviceLost");
            expectations.expect(exec.executor->deviceLost() && !exec.executor->ownerDrainRequired(),
                                "lost: the executor is terminal without an unproven drain");
            expectations.expect(exec.executor->takeImage() == nullptr,
                                "lost: no image is published");
            expectations.expect(exec.executor->begin(prepared.scene, kSceneBudget).code ==
                                    GpuSceneExecutorDiagnosticCode::DeviceLost,
                                "lost: reuse is refused on a lost device");
        }
        // The seam proved the REAL fence retired before injecting loss, so destroying the poisoned
        // pipeline must not report a failed bounded native drain.
        expectations.expect(!GpuSceneExecutor::teardownDrainIncomplete(),
                            "lost: no failed native bounded drain is reported");
    }

    // 4) A real submitted job cancelled and proven retired is immediately reusable.
    {
        auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
        auto exec = GpuSceneExecutor::create(device, *cache.cache);
        expectations.expect(exec.hasValue(), "reuse: the executor is created");
        if (exec) {
            static_cast<void>(exec.executor->begin(prepared.scene, kSceneBudget));
            static_cast<void>(exec.executor->poll());
            exec.executor->cancel();
            for (std::uint64_t i = 0; i < kMaxPollIterations; ++i) {
                if (exec.executor->poll() != GpuSceneExecutorPollResult::Pending) {
                    break;
                }
                std::this_thread::yield();
            }
            expectations.expect(exec.executor->diagnostic().code ==
                                    GpuSceneExecutorDiagnosticCode::Cancelled,
                                "reuse: the proven cancellation is Cancelled");
            expectations.expect(!exec.executor->ownerDrainRequired() &&
                                    !exec.executor->deviceLost(),
                                "reuse: a proven cancellation needs no drain");
            expectations.expect(exec.executor->begin(prepared.scene, kSceneBudget).code ==
                                    GpuSceneExecutorDiagnosticCode::None,
                                "reuse: the executor is reusable after a proven cancellation");
            exec.executor->cancel();
            static_cast<void>(exec.executor->poll());
        }
    }

    // 5) Destroying while a fault-injected poll still reports Pending must not falsely report an
    //    incomplete teardown: the REAL healthy job is still retired by the owned pipeline's bounded
    //    native drain (which owns the input pins), and only a genuine drain failure would be fused.
    {
        fault::clear();
        auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
        {
            auto exec = GpuSceneExecutor::create(device, *cache.cache);
            if (exec) {
                static_cast<void>(exec.executor->begin(prepared.scene, kSceneBudget));
                static_cast<void>(exec.executor->poll());
                fault::set(Fault::StallPending);
                exec.executor.reset();
                expectations.expect(
                    !GpuSceneExecutor::teardownDrainIncomplete(),
                    "destroy: a healthy bounded native drain is not reported incomplete");
            }
        }
        fault::clear();
    }
#else
    static_cast<void>(expectations);
    static_cast<void>(device);
    std::cout << "NOTE: native retirement fault tests skipped (fault injection not compiled in)\n";
#endif
}
