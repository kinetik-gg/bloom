#pragma once
#include "gpu_preview_display_service_packed_support.hpp"

namespace {
#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE
void unsupportedPackedProof(
    Expectations& checks, GpuPreviewDisplayServiceOptions options,
    const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
    const std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>& processor,
    const std::shared_ptr<const GpuDisplayProgramService>& programs,
    const bloom::document::Snapshot& snapshot) {
    TaskScheduler scheduler(schedulerConfig());
    options.enabled = false;
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
    auto frame = packedResult(awaitResult(submission.handle));
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
    auto scheduler = std::make_unique<TaskScheduler>(schedulerConfig());
    auto service = std::make_unique<GpuPreviewDisplayService>(
        *scheduler,
        packedStageFunction(plan, std::make_shared<CpuGpuSceneBuilder>(), programs, processor),
        generalCpuStageFunction(plan, processor, std::make_shared<CpuCompositionEvaluator>()),
        generalFallback(processor), options);
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
    auto hold = std::make_shared<std::atomic_bool>(true);
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
    if (shutdownProof) {
        using namespace bloom::render::display_transfer_detail;
        // Make the destructor's nonblocking fence observation explicitly unproved too. This
        // never proves retirement and is confined to this final, separate fixture process.
        setTransferFaultForTest(TransferFault::ForceFenceTimeout);
        service->beginShutdown();
        checks.expect(
            waitUntil(
                [&] { return service->status().state == GpuPreviewDisplayServiceState::Stopped; },
                30s),
            "shutdown reaches bounded terminal teardown without a GUI/queue wait");
        const auto beforeDetach = retention->snapshot();
        checks.expect(
            beforeDetach.sourceBytes == initial.sourceBytes &&
                beforeDetach.stagingBytes == initial.stagingBytes && beforeDetach.hostBytes == 0 &&
                beforeDetach.reservedBytes > 0,
            "unproved shutdown quarantine retains native charge and releases host capacity");
        service.reset();
        core.reset();
        scheduler.reset();
        const auto afterDetach = retention->snapshot();
        {
            std::lock_guard lock(contextProbe->mutex);
            checks.expect(
                contextProbe->stage.expired(),
                "quarantine does not retain stage/snapshot/media/parent completion context");
        }
        checks.expect(
            afterDetach.reservedBytes == beforeDetach.reservedBytes &&
                afterDetach.sourceBytes == beforeDetach.sourceBytes &&
                afterDetach.stagingBytes == beforeDetach.stagingBytes && afterDetach.hostBytes == 0,
            "resource reservation outlives service and scheduler forced bookkeeping release");
        std::cout << "SHUTDOWN_QUARANTINE reserved=" << afterDetach.reservedBytes
                  << " source=" << afterDetach.sourceBytes
                  << " staging=" << afterDetach.stagingBytes << " host=" << afterDetach.hostBytes
                  << " exact-owner-exited; no false recovery promise\n";
        setTransferFaultForTest(TransferFault::None);
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
    hold->store(false);
    checks.expect(waitUntil([&] { return retention->snapshot().reservedBytes == 0; }, 10s),
                  "live exact owner still retires detached transfer and releases charge");
    std::cout << "DETACH retained=" << retained.reservedBytes
              << " later=" << retention->snapshot().reservedBytes << '\n';
    service->beginShutdown();
}
#endif
} // namespace
