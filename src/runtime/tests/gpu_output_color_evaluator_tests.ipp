// Complete production evaluator terminal ordering and controlled submitted-unknown retirement.
// Published final host storage remains charged after request authority detaches.
void testFinalEvaluatorTerminalAndUnknown(Expectations& expectations) {
    namespace test = bloom::runtime::executor_test;
    namespace faults = bloom::render::output_color_readback_detail;
    using bloom::runtime::GpuProcessFrameDiagnosticCode;
    using bloom::runtime::GpuProcessFrameEvaluator;
    using bloom::runtime::GpuProcessFrameEvaluatorOptions;
    using bloom::runtime::GpuProcessFrameStatus;
    bloom::render::test::FinalHostRetirementScope host;
    auto policy = std::make_shared<bloom::runtime::GpuProcessingPhasePolicy>(1, host.owner);
    auto sample = bloom::runtime::memoryBudgetSample();
    bloom::runtime::MemoryBudgetLedger ledger(sample.physicalBytes,
                                              sample.machine.availableBytes.value_or(0));
    ledger.beginProcessSampling();
    const auto now = bloom::runtime::GpuProcessingPhasePolicy::Clock::now();
    const auto delivered = ledger.applySample(std::move(sample), now);
    policy->applyHostSample(delivered, ledger.physicalByteBudget(), now);
    if (!policy->snapshot().reservations.hostReady) {
        expectations.expect(false, "final-evaluator: actual host/process capacity is ready");
        return;
    }
    GpuProcessFrameEvaluatorOptions options;
    options.processingPhasePolicy = policy;
    options.requestByteBudget = 64ULL << 20;
    options.readbackByteBudget = 64ULL << 20;
    options.nativeDeadline = std::chrono::seconds(2);
    options.maxQueuedRequests = 1;
    auto evaluator = GpuProcessFrameEvaluator::create(options);
    if (!evaluator || !evaluator->gpuAvailable()) {
        expectations.expect(false, "final-evaluator: actual native owner is ready");
        return;
    }
    // Clear only the test observation before evaluator destruction on every exceptional exit.
    struct RestoreObservation final {
        ~RestoreObservation() { faults::setReadbackFaultForTest(faults::ReadbackFault::None); }
    } restore;
    const auto plan = test::twoLayerPlan(test::format(8, 8), {}, {}, 8, 8, 92000);
    const auto request = test::requestFor(*plan, test::RationalTime::fromInteger(0), 64ULL << 20);
    faults::setReadbackFaultForTest(faults::ReadbackFault::FailHostAllocationAfterFence);
    const auto failedHandoff = evaluator->evaluate(plan, request);
    faults::setReadbackFaultForTest(faults::ReadbackFault::None);
    const auto afterFailure = policy->snapshot().reservations;
    expectations.expect(
        failedHandoff.status == GpuProcessFrameStatus::Failed && !failedHandoff.frame &&
            failedHandoff.diagnostic.code == GpuProcessFrameDiagnosticCode::ReadbackFailed &&
            failedHandoff.diagnostic.message ==
                "the readback host buffers could not be allocated" &&
            failedHandoff.counters.nativeDispatches > 0 && afterFailure.admittedPhases == 0 &&
            afterFailure.unprovenPhases == 0 && afterFailure.ownedNativeBuffers == 0,
        "final-evaluator: failed host handoff after real fence proof releases exact max1 phase");
    auto first = evaluator->evaluate(plan, request);
    expectations.expect(first.status == GpuProcessFrameStatus::Evaluated && first.frame &&
                            first.deviceOwnershipEpoch != 0 &&
                            first.counters.nativeDispatches > 0 && first.counters.readbacks == 1 &&
                            policy->snapshot().reservations.admittedPhases == 0,
                        "final-evaluator: genuine completion ends request authority before return");
    // Immediate admission, without sleeping or waiting for an owner-loop alias to disappear.
    auto second = evaluator->evaluate(plan, request);
    expectations.expect(second.status == GpuProcessFrameStatus::Evaluated && second.frame &&
                            second.deviceOwnershipEpoch == first.deviceOwnershipEpoch &&
                            second.counters.readbacks == 1 &&
                            policy->snapshot().reservations.admittedPhases == 0,
                        "final-evaluator: immediate same-root max1 next request succeeds");
    faults::setReadbackFaultForTest(faults::ReadbackFault::ForceFenceTimeout);
    const auto unknown = evaluator->evaluate(plan, request);
    const auto held = policy->snapshot().reservations;
    expectations.expect(unknown.status == GpuProcessFrameStatus::Failed && !unknown.frame &&
                            unknown.diagnostic.code ==
                                GpuProcessFrameDiagnosticCode::ReadbackFailed &&
                            held.ownedNativeBufferBytes > 0 && held.ownedNativeBuffers == 1 &&
                            held.admittedPhases == 1 && held.unprovenPhases == 1,
                        "final-evaluator: real submitted readback holds staging and sticky phase");
    evaluator->beginShutdown();
    const auto shortWait = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    while (std::chrono::steady_clock::now() < shortWait && !evaluator->retirementComplete())
        std::this_thread::yield();
    const auto stillHeld = policy->snapshot().reservations;
    expectations.expect(!evaluator->retirementComplete() &&
                            stillHeld.ownedNativeBufferBytes == held.ownedNativeBufferBytes &&
                            stillHeld.ownedNativeBuffers == 1 && stillHeld.admittedPhases == 1 &&
                            stillHeld.unprovenPhases == 1,
                        "final-evaluator: unknown owner cannot publish a clean shutdown");
    std::cout << "FINAL_READBACK_UNKNOWN bufferBytes=" << held.ownedNativeBufferBytes
              << " nonlocalHost=" << held.ownedNonlocalNativeBufferHostBytes
              << " buffers=" << held.ownedNativeBuffers << " phases=" << held.admittedPhases
              << " unproven=" << held.unprovenPhases << '\n';
    // Removing the observed-status override permits the real native fence/backing drain. The
    // independent device-report unknown state is not cleared or asserted to disappear.
    faults::setReadbackFaultForTest(faults::ReadbackFault::None);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!evaluator->retirementComplete() && std::chrono::steady_clock::now() < until)
        std::this_thread::yield();
    expectations.expect(evaluator->retirementComplete(),
                        "final-evaluator: actual owner drains the exact generation");
    evaluator.reset();
    const auto published = policy->snapshot().reservations;
    expectations.expect(
        published.admittedPhases == 0 &&
            published.ownedFinalProcessHostBytes ==
                2ULL * 8ULL * 8ULL * sizeof(bloom::render::Rgba32f) &&
            published.ownedFinalHostAllocations == 2 &&
            published.ownedFinalHostMetadataBytes ==
                2 * bloom::render::detail::packedHostAllocationMetadataBytes(),
        "final-evaluator: two published host images outlive actual native retirement");
    host.owner->beginShutdown();
    expectations.expect(!host.owner->exitAcknowledged(),
                        "final-evaluator: external frame aliases block actual host worker exit");
    std::thread releaseFrames([a = std::move(first.frame), b = std::move(second.frame)]() mutable {
        a.reset();
        b.reset();
    });
    releaseFrames.join();
    const auto hostUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!host.owner->exitAcknowledged() && std::chrono::steady_clock::now() < hostUntil)
        std::this_thread::yield();
    expectations.expect(
        host.owner->exitAcknowledged(),
        "final-evaluator: actual host worker free completes after foreign last drop");
    const auto final = policy->snapshot().reservations;
    expectations.expect(
        final.ownedFinalProcessHostBytes == 0 && final.ownedFinalEncodedHostBytes == 0 &&
            final.ownedFinalHostMetadataBytes == 0 && final.ownedFinalHostAllocations == 0 &&
            final.ownedNativeBufferBytes == 0 && final.ownedNativeBuffers == 0 &&
            final.ownedNonlocalNativeBufferHostBytes == 0 && final.ownedNativeImageBytes == 0 &&
            final.ownedNativeImages == 0 && final.ownedNonlocalNativeHostBytes == 0 &&
            final.admittedPhases == 0 && final.unprovenPhases == 0 &&
            final.totalHostClaimBytes == 0 && final.totalDeviceClaimBytes == 0,
        "final-evaluator: actual backing/generation destruction ends covered claims");
    std::cout << "FINAL_READBACK_EVALUATOR immediateMax1=1 nativeRetired=1 coveredClaims=0\n";
}
