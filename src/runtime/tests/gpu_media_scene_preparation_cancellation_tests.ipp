// Cancellation group: a pre-cancelled preparation and cancellation during preparation both publish
// no partial scene. Included by gpu_media_scene_preparation_tests.cpp inside its anonymous
// namespace.

void testPreCancelledPreparationPublishesNothing(
    Expectations& expectations, const std::shared_ptr<const CompiledCompositionPlan>& plan) {
    const auto token = makeCancelledToken();
    expectations.expect(token.isCancellationRequested(), "the captured token is cancelled");
    const CpuGpuSceneBuilder builder{};
    const auto prepared = builder.build(plan, requestFor(*plan), token);
    expectations.expect(!prepared.hasValue() &&
                            prepared.diagnostic.code ==
                                bloom::runtime::PreparedGpuSceneDiagnosticCode::Cancelled,
                        "a pre-cancelled request returns no partial scene");
}

// Cancellation requested while preparation is already running still publishes no partial scene. The
// builder is deterministically parked at its private in-build checkpoint before the request is
// issued, so this cannot race a build that has already finished. No wall-clock sleep is used as
// synchronization, and the bounded wait fails the test rather than hanging.
void testCancellationDuringPreparation(Expectations& expectations) {
    const auto plan =
        twoLayerPlan(format(16, 12), LayerValues{.position = {8.3, 6.1}},
                     LayerValues{.position = {8.7, 6.4}, .opacity = 0.75}, 2048.0, 2048.0, 3300);
    const auto request = requestFor(*plan, RationalTime::fromInteger(0), std::size_t{1} << 29U);
    bloom::runtime::TaskSchedulerConfig config = bloom::runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    bloom::runtime::TaskScheduler scheduler(config);

    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
    CpuGpuSceneBuilder builder{};
    bloom::runtime::GpuSceneBuilderTestAccess::setCheckpoint(builder, [&] {
        std::unique_lock lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return released; });
    });

    std::atomic_bool observedCancelled = false;
    std::atomic_bool observedSuccess = false;
    auto submission = scheduler.submit<void>(
        bloom::runtime::TaskRequest("gpu media cancellation fixture",
                                    {.kind = bloom::runtime::TaskOwnerKind::Composition,
                                     .id = bloom::runtime::TaskOwnerId::fromRaw(78)}),
        [&](bloom::runtime::TaskContext& context) {
            const auto prepared = builder.build(plan, request, context.cancellation());
            if (!prepared) {
                observedCancelled.store(prepared.diagnostic.code ==
                                        bloom::runtime::PreparedGpuSceneDiagnosticCode::Cancelled);
            } else {
                observedSuccess.store(true);
            }
            return bloom::runtime::TaskResult<void>::succeeded();
        });
    expectations.expect(submission.accepted(), "the cancellation fixture task is accepted");
    {
        std::unique_lock lock(mutex);
        const bool reached =
            condition.wait_for(lock, std::chrono::seconds(10), [&] { return entered; });
        expectations.expect(reached,
                            "preparation deterministically reaches the in-build checkpoint");
    }
    submission.handle.cancel();
    {
        std::lock_guard lock(mutex);
        released = true;
    }
    condition.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!submission.handle.tryTakeResult().has_value() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    expectations.expect(observedCancelled.load() && !observedSuccess.load(),
                        "cancellation during preparation returns no partial scene");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
}
