#pragma once
#include "gpu_preview_display_service_packed_support.hpp"
#include <bloom/runtime/gpu_processing_phase_policy.hpp>

namespace {
#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE
void payloadAllocationFailureProof(
    Expectations& checks, GpuPreviewDisplayServiceOptions options,
    const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
    const std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>& processor,
    const bloom::document::Snapshot& snapshot) {
    PackedRetirementScope retirement;
    TaskScheduler scheduler(schedulerConfig());
    auto policy = std::make_shared<bloom::runtime::GpuProcessingPhasePolicy>(32, retirement.owner);
    auto sample = bloom::runtime::memoryBudgetSample();
    bloom::runtime::MemoryBudgetLedger ledger(sample.physicalBytes,
                                              sample.machine.availableBytes.value_or(0));
    ledger.beginProcessSampling();
    const auto at = bloom::runtime::GpuProcessingPhasePolicy::Clock::now();
    const auto state = ledger.applySample(std::move(sample), at);
    policy->applyHostSample(state, ledger.physicalByteBudget(), at);
    options.processingPhasePolicy = policy;
    auto builder = std::make_shared<CpuGpuSceneBuilder>(
        nullptr, bloom::runtime::GpuSceneMediaContext{}, bloom::runtime::GpuSceneOcioContext{},
        bloom::runtime::GpuScenePreparationPhase{
            .policy = policy,
            .envelope = {.metadataBytes = 1024ULL * 1024,
                         .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                         .requestedExecutorBytes = kBudget,
                         .deviceWorkingBytes = 832ULL * 1024 * 1024}});
    GpuPreviewDisplayService service(
        scheduler, packedStageFunction(plan, builder, {}, processor),
        generalCpuStageFunction(plan, processor, std::make_shared<CpuCompositionEvaluator>()),
        generalFallback(processor), options);
    checks.expect(
        waitUntil(
            [&] { return service.status().state != GpuPreviewDisplayServiceState::Initializing; },
            30s) &&
            service.status().state == GpuPreviewDisplayServiceState::Ready,
        "payload allocation fault uses genuine native service");
    auto core = bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(service);
    auto faultArmed = std::make_shared<std::atomic_uint>(1);
    core->testBeforePackedPayloadAllocation = [faultArmed] {
        packed_payload_fault::failuresRemaining = faultArmed->exchange(0);
    };
    const auto previousFailures = packed_payload_fault::actualFailures.load();
    const bloom::runtime::TaskOwner owner{.kind = bloom::runtime::TaskOwnerKind::Composition,
                                          .id = bloom::runtime::TaskOwnerId::fromRaw(7)};
    auto identity = makeIdentity(*plan, 1000, {}, {}, {});
    auto submission = service.submit(TaskRequest("actual packed payload allocation fault", owner),
                                     snapshot, identity, kBudget, {});
    checks.expect(submission.accepted(), "payload fault task admitted");
    if (!submission.accepted())
        return;
    const auto failed = awaitResult(submission.handle, 10s);
    checks.expect(
        failed && failed->state() == TaskState::Failed &&
            packed_payload_fault::actualFailures == previousFailures + 1 &&
            std::any_of(failed->diagnostics().begin(), failed->diagnostics().end(),
                        [](const auto& diagnostic) {
                            return diagnostic.code ==
                                       "bloom.runtime.gpu-packed-product-construction-failed" &&
                                   !diagnostic.admissionRetry;
                        }),
        "actual final make_shared allocation fault terminates without capacity retry");
    checks.expect(
        waitUntil(
            [&] {
                const auto root = policy->snapshot().reservations;
                return root.admittedPhases == 0 && root.unprovenPhases == 0 &&
                       root.hostReservedBytes == 0 && root.deviceReservedBytes == 0 &&
                       root.ownedPackedHostBytes == 0 && root.ownedPackedHostAllocations == 0 &&
                       root.totalHostClaimBytes == 0;
            },
            2s) &&
            service.status().counters.packedTransfers == 1 &&
            service.status().counters.cpuFallbacks == 0,
        "fence-proven product fault unwinds every provisional/phase claim with no CPU loop");
    faultArmed->store(2);
    identity.requestGeneration = 1001;
    auto doubleFault =
        service.submit(TaskRequest("packed payload plus diagnostic allocation fault", owner),
                       snapshot, identity, kBudget, {});
    const auto doubleFailed =
        doubleFault.accepted() ? awaitResult(doubleFault.handle, 10s) : std::nullopt;
    checks.expect(
        doubleFailed && doubleFailed->state() == TaskState::Failed &&
            doubleFailed->diagnostics().empty() &&
            packed_payload_fault::actualFailures == previousFailures + 3 &&
            waitUntil(
                [&] {
                    const auto root = policy->snapshot().reservations;
                    return root.admittedPhases == 0 && root.unprovenPhases == 0 &&
                           root.hostReservedBytes == 0 && root.deviceReservedBytes == 0 &&
                           root.ownedPackedHostBytes == 0 && root.totalHostClaimBytes == 0;
                },
                2s),
        "payload plus diagnostic allocation faults terminate through empty nonallocating failure");
    identity.requestGeneration = 1002;
    auto recovery = service.submit(TaskRequest("native after payload allocation fault", owner),
                                   snapshot, identity, kBudget, {});
    auto recovered = recovery.accepted() ? packedResult(awaitResult(recovery.handle)) : nullptr;
    checks.expect(
        recovered &&
            recovered->provenance().provider ==
                bloom::runtime::PreviewDisplayProvider::GpuPreparedPacked &&
            recovered->displayOnlyFrame()->packedHostOwnership() ==
                bloom::render::PackedRgba8Ownership::RootedPublished &&
            service.status().counters.packedTransfers == 3 &&
            service.status().counters.cpuFallbacks == 0,
        "following real native request recovers after allocation-failed terminal product");
    if (recovered)
        compareCpu(checks, *recovered, plan, *processor);
    recovered.reset();
    checks.expect(
        waitUntil([&] { return policy->snapshot().reservations.totalHostClaimBytes == 0; }, 2s),
        "recovered product's last alias also releases owned payload");
    service.beginShutdown();
    std::cout << "PAYLOAD_ALLOCATION_FAULT actualFailures=" << packed_payload_fault::actualFailures
              << " nativeTransfers=" << service.status().counters.packedTransfers << '\n';
}

void unsupportedPackedProof(
    Expectations& checks, GpuPreviewDisplayServiceOptions options,
    const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
    const std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>& processor,
    const std::shared_ptr<const GpuDisplayProgramService>& programs,
    const bloom::document::Snapshot& snapshot) {
    TaskScheduler scheduler(schedulerConfig());
    options.enabled = false;
    // This CPU fallback is a production processing attempt, so it needs the same explicit,
    // actually sampled host root even though no native executor will initialize.
    options.processingPhasePolicy = std::make_shared<bloom::runtime::GpuProcessingPhasePolicy>();
    auto sample = bloom::runtime::memoryBudgetSample();
    bloom::runtime::MemoryBudgetLedger ledger(sample.physicalBytes,
                                              sample.machine.availableBytes.value_or(0));
    ledger.beginProcessSampling();
    const auto at = bloom::runtime::GpuProcessingPhasePolicy::Clock::now();
    const auto state = ledger.applySample(std::move(sample), at);
    options.processingPhasePolicy->applyHostSample(state, ledger.physicalByteBudget(), at);
    GpuPreviewDisplayService service(
        scheduler,
        packedStageFunction(plan, std::make_shared<CpuGpuSceneBuilder>(), programs, processor),
        generalCpuStageFunction(plan, processor, std::make_shared<CpuCompositionEvaluator>()),
        generalFallback(processor), options);
    const bloom::runtime::TaskOwner owner{.kind = bloom::runtime::TaskOwnerKind::Composition,
                                          .id = bloom::runtime::TaskOwnerId::fromRaw(7)};
    auto submission = service.submit(TaskRequest("explicit unavailable GPU", owner), snapshot,
                                     makeIdentity(*plan, 800, {}, {}, {}), kBudget, {});
    checks.expect(submission.accepted(), "disabled GPU has explicit supported CPU fallback");
    const auto result = awaitResult(submission.handle);
    auto frame = packedResult(result);
    if (!frame && result) {
        for (const auto& diagnostic : result->diagnostics())
            std::cerr << diagnostic.code << ": " << diagnostic.summary << " / " << diagnostic.detail
                      << '\n';
    }
    checks.expect(
        frame && frame->provenance().provider == bloom::runtime::PreviewDisplayProvider::CpuOcio &&
            frame->processIdentity().provider == bloom::runtime::EvaluationProvider::CpuReference &&
            !frame->provenance().gpuPreparedPacked &&
            service.status().state == GpuPreviewDisplayServiceState::Unavailable &&
            service.status().counters.packedTransfers == 0,
        "unavailable GPU never labels CPU frame GPU-prepared or transfers it");
}

void heldTransferProof(
    Expectations& checks, const GpuPreviewDisplayServiceOptions& options,
    const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
    const std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>& processor,
    const std::shared_ptr<const GpuDisplayProgramService>& programs,
    const bloom::document::Snapshot& snapshot, const bool shutdownProof) {
    PackedRetirementScope retirement;
    auto scheduler = std::make_unique<TaskScheduler>(schedulerConfig());
    auto configured = options;
    auto builder = std::make_shared<CpuGpuSceneBuilder>();
    configured.processingPhasePolicy =
        std::make_shared<bloom::runtime::GpuProcessingPhasePolicy>(4, retirement.owner);
    auto sample = bloom::runtime::memoryBudgetSample();
    bloom::runtime::MemoryBudgetLedger ledger(sample.physicalBytes,
                                              sample.machine.availableBytes.value_or(0));
    ledger.beginProcessSampling();
    const auto at = bloom::runtime::GpuProcessingPhasePolicy::Clock::now();
    const auto state = ledger.applySample(std::move(sample), at);
    configured.processingPhasePolicy->applyHostSample(state, ledger.physicalByteBudget(), at);
    builder = std::make_shared<CpuGpuSceneBuilder>(
        nullptr, bloom::runtime::GpuSceneMediaContext{}, bloom::runtime::GpuSceneOcioContext{},
        bloom::runtime::GpuScenePreparationPhase{
            .policy = configured.processingPhasePolicy,
            .envelope = {.metadataBytes = 1024ULL * 1024,
                         .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                         .requestedExecutorBytes = kBudget,
                         .deviceWorkingBytes = 832ULL * 1024 * 1024}});
    auto service = std::make_unique<GpuPreviewDisplayService>(
        *scheduler, packedStageFunction(plan, builder, programs, processor),
        generalCpuStageFunction(plan, processor, std::make_shared<CpuCompositionEvaluator>()),
        generalFallback(processor), configured);
    checks.expect(
        waitUntil(
            [&] { return service->status().state != GpuPreviewDisplayServiceState::Initializing; },
            30s),
        "held transfer service startup terminal");
    if (service->status().state != GpuPreviewDisplayServiceState::Ready) {
        checks.expect(false, "held proof requires genuine GPU service");
        return;
    }
    auto core = bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(*service);
    auto programStorage = std::make_shared<PackedProgramStorageProbe>();
    observePackedProgramStorage(core, configured.processingPhasePolicy, programStorage);
    auto hold = std::make_shared<std::atomic_bool>(true);
    auto teardownObserved = std::make_shared<std::atomic_bool>(false);
    auto allowTeardown = std::make_shared<std::atomic_bool>(false);
    struct TeardownGateRelease final {
        std::shared_ptr<std::atomic_bool> gate;
        ~TeardownGateRelease() { gate->store(true); }
    } gateRelease{allowTeardown};
    if (shutdownProof) {
        core->testBeforePackedUnprovenTeardown = [teardownObserved, allowTeardown] {
            teardownObserved->store(true);
            while (!allowTeardown->load())
                std::this_thread::sleep_for(1ms);
        };
    }
    auto polls = std::make_shared<std::atomic<std::uint64_t>>(0);
    struct ContextProbe {
        std::mutex mutex;
        std::weak_ptr<bloom::runtime::detail::PreviewDisplayStageRecord> stage;
    };
    auto contextProbe = std::make_shared<ContextProbe>();
    std::weak_ptr<bloom::runtime::detail::PreviewDisplayServiceCore> weakCore = core;
    // Installed before admission. The override holds observation only: source/copy submission
    // remain production Vulkan work, and unblocking still requires the real fence to retire.
    core->packedTransferPollOverride = [hold, polls, weakCore,
                                        contextProbe](bloom::render::GpuDisplayTransfer& transfer) {
        if (auto current = weakCore.lock()) {
            std::lock_guard lock(contextProbe->mutex);
            contextProbe->stage = current->residentNativeActive;
        }
        if (hold->load()) {
            ++*polls;
            return bloom::render::GpuDisplayTransferState::Pending;
        }
        return transfer.poll();
    };
    const bloom::runtime::TaskOwner owner{.kind = bloom::runtime::TaskOwnerKind::Composition,
                                          .id = bloom::runtime::TaskOwnerId::fromRaw(7)};
    auto identity = makeIdentity(*plan, 900, {}, {}, {});
    // Two 256 MiB admissions fit the unchanged 1 GiB scheduler capacity. Reserving the whole
    // capacity per request would correctly route the replacement to CPU admission instead.
    const auto requestLimit = shutdownProof ? kBudget : (std::size_t{1} << 28U);
    auto submission = service->submit(TaskRequest("held packed transfer", owner), snapshot,
                                      identity, requestLimit, {});
    checks.expect(submission.accepted(), "held transfer admitted");
    if (!submission.accepted())
        return;
    checks.expect(
        waitUntil(
            [&] { return service->status().counters.packedTransfers == 1 && polls->load() > 0; },
            30s),
        "real GPU final copy submitted before hold");
    std::shared_ptr<const bloom::render::GpuDisplayTransferRetention> retention;
    {
        std::lock_guard lock(core->stateMutex);
        retention = core->packedRetention;
    }
    if (!retention) {
        checks.expect(false, "transfer owns independent resource reservation");
        hold->store(false);
        return;
    }
    const auto initial = retention->snapshot();
    checks.expect(
        initial.sourceBytes > 0 && initial.stagingBytes > 0 && initial.hostBytes > 0 &&
            initial.reservedBytes >= initial.sourceBytes + initial.stagingBytes + initial.hostBytes,
        "admitted reservation covers every actual source/staging/preallocated host allocation");
    const auto initialRoot = configured.processingPhasePolicy->snapshot().reservations;
    const auto retainedProgram = programStorage->snapshot();
    checkPackedProgramStorage(checks, retainedProgram, initial.hostBytes);
    checks.expect(initialRoot.ownedPackedHostAllocations == 1 &&
                      initialRoot.ownedPackedHostBytes == initial.hostBytes &&
                      initialRoot.ownedNativeBuffers ==
                          retainedProgram.root.ownedNativeBuffers + 1 &&
                      initialRoot.ownedNativeBufferBytes ==
                          retainedProgram.root.ownedNativeBufferBytes + initial.stagingBytes &&
                      initialRoot.admittedPhases == 1 && initialRoot.unprovenPhases == 0,
                  "real submitted copy retains exact rooted staging and provisional packed claims");
    if (shutdownProof) {
        using namespace bloom::render::display_transfer_detail;
        // Real submit happened above. The observation override cannot prove its native fence;
        // the private owner gate observes the sticky phase before any pipeline/backing teardown.
        setTransferFaultForTest(TransferFault::ForceFenceTimeout);
        service->beginShutdown();
        const bool observed = waitUntil([&] { return teardownObserved->load(); }, 30s);
        checks.expect(observed, "actual unproven packed owner boundary is reached");
        if (!observed) {
            setTransferFaultForTest(TransferFault::None);
            hold->store(false);
            allowTeardown->store(true);
            return;
        }
        const auto unproven = configured.processingPhasePolicy->snapshot().reservations;
        checks.expect(
            unproven.ownedNativeBuffers == initialRoot.ownedNativeBuffers &&
                unproven.ownedNativeBufferBytes == initialRoot.ownedNativeBufferBytes &&
                unproven.ownedNonlocalNativeBufferHostBytes ==
                    initialRoot.ownedNonlocalNativeBufferHostBytes &&
                unproven.admittedPhases == 1 && unproven.unprovenPhases == 1 &&
                service->status().state != GpuPreviewDisplayServiceState::Stopped &&
                !waitUntil(
                    [&] {
                        return service->status().state == GpuPreviewDisplayServiceState::Stopped;
                    },
                    50ms),
            "real staging and sticky exact phase survive incomplete shutdown before native proof");
        std::cout << "PACKED_UNPROVEN bufferBytes=" << unproven.ownedNativeBufferBytes
                  << " buffers=" << unproven.ownedNativeBuffers
                  << " nonlocalHost=" << unproven.ownedNonlocalNativeBufferHostBytes
                  << " phases=" << unproven.admittedPhases
                  << " unproven=" << unproven.unprovenPhases << '\n';
        // No latch/fuse/quota mutation: remove only test observations and allow the exact owner
        // to prove its real fence, free backing, and destroy the sticky allocator generation.
        setTransferFaultForTest(TransferFault::None);
        hold->store(false);
        allowTeardown->store(true);
        checks.expect(
            waitUntil(
                [&] { return service->status().state == GpuPreviewDisplayServiceState::Stopped; },
                30s),
            "Stopped follows actual native owner/generation teardown");
        const auto retired = configured.processingPhasePolicy->snapshot().reservations;
        checks.expect(
            retired.ownedNativeBufferBytes == 0 && retired.ownedNativeBuffers == 0 &&
                retired.ownedNonlocalNativeBufferHostBytes == 0 &&
                retired.ownedNativeImageBytes == 0 && retired.ownedNativeImages == 0 &&
                retired.ownedNonlocalNativeHostBytes == 0 && retired.ownedPackedHostBytes == 0 &&
                retired.ownedPackedHostMetadataBytes == 0 &&
                retired.ownedPackedHostAllocations == 0 && retired.admittedPhases == 0 &&
                retired.unprovenPhases == 0 && retired.totalHostClaimBytes == 0 &&
                retired.totalDeviceClaimBytes == 0 && retention->snapshot().reservedBytes == 0,
            "covered staging/image/packed and phase claims release only after native destruction");
        service.reset();
        core.reset();
        scheduler.reset();
        {
            std::lock_guard lock(contextProbe->mutex);
            checks.expect(contextProbe->stage.expired(),
                          "retired native ownership does not retain stage/snapshot/parent context");
        }
        std::cout << "PASS: production submitted packed staging sticky phase then actual "
                     "generation retirement\n";
        return;
    }
    submission.handle.cancel();
    identity.requestGeneration = 901;
    auto newest = service->submit(TaskRequest("newest packed transfer", owner), snapshot, identity,
                                  requestLimit, {});
    checks.expect(newest.accepted(), "newest request can queue behind retiring old transfer");
    const auto firstPoll = polls->load();
    checks.expect(waitUntil([&] { return polls->load() >= firstPoll + 525; }, 8s),
                  "cancel remains owner-pumped beyond the original 512-pump retirement bound");
    checks.expect(!submission.handle.tryTakeResult() && retention->snapshot().reservedBytes > 0,
                  "logical cancellation does not complete/refund unproved resources");
    checks.expect(!newest.handle.tryTakeResult() && service->status().counters.packedTransfers == 1,
                  "superseding work cannot replace an unretired transfer or publish stale pixels");
    hold->store(false);
    const auto cancelled = awaitResult(submission.handle, 10s);
    checks.expect(cancelled && cancelled->state() == TaskState::Cancelled &&
                      retention->snapshot().reservedBytes == 0,
                  "late genuine retirement completes cancel and releases all physical reservation");
    std::cout << "LATE_CANCEL polls=" << polls->load()
              << " released=" << retention->snapshot().reservedBytes << '\n';
    auto replacement = packedResult(awaitResult(newest.handle, 10s));
    checks.expect(replacement && replacement->desiredIdentity() == identity &&
                      replacement->provenance().provider ==
                          bloom::runtime::PreviewDisplayProvider::GpuPreparedPacked,
                  "newest coherent GPU product publishes only after old copy retires");
    if (replacement)
        compareCpu(checks, *replacement, plan, *processor);
    checks.expect(
        replacement &&
            replacement->displayOnlyFrame()->packedHostOwnership() ==
                bloom::render::PackedRgba8Ownership::RootedPublished &&
            waitUntil(
                [&] {
                    const auto root = configured.processingPhasePolicy->snapshot().reservations;
                    return root.admittedPhases == 0 && root.hostReservedBytes == 0 &&
                           root.ownedPackedHostAllocations == 1 &&
                           root.ownedPackedHostBytes ==
                               replacement->provenance()
                                   .gpuPreparedPacked->transfer.hostStorageBytes;
                },
                2s),
        "cancelled provisional copy folds back while only newest published storage remains");

    checks.expect(service->status().counters.cpuFallbacks == 0,
                  "fitting queued newest stays on genuine GPU scene and final transfer route");
    hold->store(true);
    identity.requestGeneration = 902;
    auto detached = service->submit(TaskRequest("detached packed transfer", owner), snapshot,
                                    identity, requestLimit, {});
    checks.expect(detached.accepted(), "second transfer admitted after genuine retirement");
    if (!detached.accepted())
        return;
    checks.expect(waitUntil([&] { return service->status().counters.packedTransfers == 3; }, 30s),
                  "second actual copy submitted");
    {
        std::lock_guard lock(core->stateMutex);
        retention = core->packedRetention;
    }
    core->lease->detach();
    const auto forced = awaitResult(detached.handle, 10s);
    const auto retained = retention->snapshot();
    checks.expect(forced && forced->state() == TaskState::Failed && retained.sourceBytes > 0 &&
                      retained.stagingBytes > 0 && retained.hostBytes > 0 &&
                      retained.reservedBytes > 0,
                  "forced scheduler detach cannot erase still-owned transfer allocation charge");
    checks.expect(
        forced &&
            std::all_of(forced->diagnostics().begin(), forced->diagnostics().end(),
                        [](const auto& diagnostic) { return !diagnostic.admissionRetry; }) &&
            configured.processingPhasePolicy->snapshot().reservations.ownedPackedHostAllocations ==
                2,
        "forced terminal carries no capacity retry and retains unpublished host backing");
    hold->store(false);
    checks.expect(waitUntil([&] { return retention->snapshot().reservedBytes == 0; }, 10s),
                  "live exact owner still retires detached transfer and releases charge");
    std::cout << "DETACH retained=" << retained.reservedBytes
              << " later=" << retention->snapshot().reservedBytes << '\n';
    checks.expect(
        waitUntil(
            [&] {
                const auto root = configured.processingPhasePolicy->snapshot().reservations;
                return root.admittedPhases == 0 && root.hostReservedBytes == 0 &&
                       root.ownedPackedHostAllocations == 1;
            },
            2s),
        "forced terminal retirement releases its provisional host allocation exactly once");
    replacement.reset();
    checks.expect(waitUntil(
                      [&] {
                          return configured.processingPhasePolicy->snapshot()
                                     .reservations.ownedPackedHostBytes == 0;
                      },
                      2s),
                  "worker free after last successful frame alias releases the remaining published "
                  "host allocation");
    service->beginShutdown();
}
#endif
} // namespace
