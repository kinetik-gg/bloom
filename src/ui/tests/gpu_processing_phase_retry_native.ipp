// Production retry witnesses; setup tasks only occupy/release the real native owner slot.
void retryNativeChecks(Expectations& checks, Fixture& fixture, ui::CompositionSession& session,
                       TaskScheduler& scheduler, const std::filesystem::path& loader,
                       const runtime::CpuReferenceDisplayPreparer& referenceDisplay) {
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    const auto makeSample = [](const std::uint64_t serial, const std::uint64_t limit) {
        runtime::MemoryBudgetState sample;
        sample.machine.availableBytes = 60 * gib;
        sample.process.status = runtime::ProcessMemoryStatus::Finite;
        sample.process.source = runtime::ProcessMemorySource::LinuxCgroupV2;
        sample.process.sampledAt = runtime::GpuProcessingPhasePolicy::Clock::now();
        sample.process.domains.push_back({.path = "/retry-fixture",
                                          .kind = runtime::ProcessMemoryLimitKind::Finite,
                                          .limitBytes = limit,
                                          .currentBytes = 0});
        sample.processSampleFresh = true;
        sample.sampleSerial = serial;
        return sample;
    };
    auto policy = std::make_shared<runtime::GpuProcessingPhasePolicy>(1);
    auto sample = makeSample(1, 8 * gib);
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    runtime::GpuScenePreparationPhase phase{
        .policy = policy,
        .envelope = {.metadataBytes = 1024ULL * 1024,
                     .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                     .requestedExecutorBytes = kBudget,
                     .deviceWorkingBytes = 832ULL * 1024 * 1024}};
    runtime::CpuGpuSceneBuilder builder(
        nullptr, runtime::GpuSceneMediaContext::fromEvaluator(fixture.evaluator), {}, phase);
    const auto actualGpu =
        ui::makeCompositionPreviewGpuSceneStage(fixture.compiler, builder, fixture.provider);
    const auto actualCpu = fixture.productionCpuStage();
    {
        auto options = serviceOptions(loader);
        options.processingPhasePolicy = policy;
        // 1080p RGBA32F+RGBA8 cannot fit this device request envelope; the HOST fallback keeps
        // the original request ceiling. Startup qualification remains the real native one.
        options.previewByteAllowance = 32ULL * 1024 * 1024;
        std::atomic_size_t prepared{0};
        std::atomic_size_t cpu{0};
        GpuPreviewDisplayService service(
            scheduler,
            [actualGpu, &prepared](const document::Snapshot& snapshot,
                                   const PreviewRequestIdentity& identity, const std::size_t limit,
                                   const std::vector<runtime::SnapshotParameterOverride>& overrides,
                                   TaskContext& context) {
                auto result = actualGpu(snapshot, identity, limit, overrides, context);
                if (result.value() && *result.value() && (*result.value())->stage)
                    prepared.fetch_add(1);
                return result;
            },
            [actualCpu, &cpu](const document::Snapshot& snapshot,
                              const PreviewRequestIdentity& identity, const std::size_t limit,
                              const std::vector<runtime::SnapshotParameterOverride>& overrides,
                              TaskContext& context) {
                cpu.fetch_add(1);
                return actualCpu(snapshot, identity, limit, overrides, context);
            },
            fixture.productionCpuFallback(referenceDisplay), options);
        checks.expect(waitUntil(
                          [&] {
                              return service.status().state !=
                                     GpuPreviewDisplayServiceState::Initializing;
                          },
                          90s) &&
                          service.status().state == GpuPreviewDisplayServiceState::Ready,
                      "count-one fallback owns a real ready native service");
        if (checks.failures() != 0)
            return;
        const auto& snapshot = session.snapshot();
        const auto identity = makeIdentity(900, snapshot, session.compositionId());
        auto submission = service.submit(
            TaskRequest("count-one ineligible preparation",
                        {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(900)}),
            snapshot, identity, kBudget, {});
        auto result = submission.accepted() ? awaitResult(submission.handle) : std::nullopt;
        const auto frame = result ? residentFrameOf(*result) : nullptr;
        checks.expect(frame && hasCpuPixels(frame) && !isResident(frame) && prepared.load() == 1 &&
                          cpu.load() == 1 && service.status().counters.residentGraphJobs == 0 &&
                          service.status().residentDetail.find("ineligible") != std::string::npos &&
                          policy->snapshot().reservations.admittedPhases == 0,
                      "actual ineligible prepared scene releases all own aliases before count-one "
                      "CPU fallback");
        service.beginShutdown();
        checks.expect(
            waitUntil(
                [&] { return service.status().state == GpuPreviewDisplayServiceState::Stopped; },
                15s),
            "count-one fallback service stops safely");
    }
    if (checks.failures() != 0)
        return;
    policy = std::make_shared<runtime::GpuProcessingPhasePolicy>(2);
    sample = makeSample(1, 8 * gib);
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    phase.policy = policy;
    runtime::CpuGpuSceneBuilder nativeBuilder(
        nullptr, runtime::GpuSceneMediaContext::fromEvaluator(fixture.evaluator), {}, phase);
    auto invalidPhase = phase;
    invalidPhase.envelope.minimumExecutorBytes = kBudget + 1;
    runtime::CpuGpuSceneBuilder invalidBuilder(
        nullptr, runtime::GpuSceneMediaContext::fromEvaluator(fixture.evaluator), {}, invalidPhase);
    const auto nativeStage =
        ui::makeCompositionPreviewGpuSceneStage(fixture.compiler, nativeBuilder, fixture.provider);
    const auto invalidStage =
        ui::makeCompositionPreviewGpuSceneStage(fixture.compiler, invalidBuilder, fixture.provider);
    std::atomic_bool invalidMode{false};
    std::atomic_size_t preparations{0};
    auto options = serviceOptions(loader);
    options.processingPhasePolicy = policy;
    GpuPreviewDisplayService service(
        scheduler,
        [nativeStage, invalidStage, &invalidMode,
         &preparations](const document::Snapshot& snapshot, const PreviewRequestIdentity& identity,
                        const std::size_t limit,
                        const std::vector<runtime::SnapshotParameterOverride>& overrides,
                        TaskContext& context) {
            preparations.fetch_add(1);
            return (invalidMode.load() ? invalidStage : nativeStage)(snapshot, identity, limit,
                                                                     overrides, context);
        },
        actualCpu, fixture.productionCpuFallback(referenceDisplay), options);
    checks.expect(
        waitUntil(
            [&] { return service.status().state != GpuPreviewDisplayServiceState::Initializing; },
            90s) &&
            service.status().state == GpuPreviewDisplayServiceState::Ready,
        "native retry owns a genuinely ready production service");
    if (checks.failures() != 0)
        return;
    const auto core = runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(service);
    const auto ownerControl = [&](const std::shared_ptr<render::GpuProcessingPhaseLease>& lease,
                                  const bool retain) {
        auto submission = scheduler.submitGpu<int>(
            TaskRequest("native phase slot setup",
                        {.kind = TaskOwnerKind::Application, .id = TaskOwnerId::fromRaw(901)},
                        runtime::TaskPriority::Interactive, runtime::TaskExecutor::Gpu),
            core->generation, runtime::GpuTaskAdmission{0, 0},
            [core, lease, retain](TaskContext&, runtime::GpuTaskCompletion<int> completion) {
                const bool accepted = retain ? core->device->retainProcessingPhase(lease) : true;
                if (!retain)
                    core->device->retireProcessingPhase(lease->id(), true);
                static_cast<void>(
                    std::move(completion).complete(TaskResult<int>::succeeded(accepted ? 1 : 0)));
            });
        auto result = submission.accepted() ? awaitResult(submission.handle) : std::nullopt;
        checks.expect(result && result->value() && *result->value() == 1,
                      "setup executes on actual exclusive native device owner");
    };
    const auto competing = [&] {
        auto grant = policy->reserve({.pixels = 1, .deviceWorkingBytes = 1, .sourceFree = true});
        checks.expect(grant.lease != nullptr, "independent phase can occupy native slot");
        return grant.lease;
    };
    auto held = competing();
    if (!held)
        return;
    ownerControl(held, true);
    ui::TaskUiBridge bridge(scheduler);
    auto cache = std::make_shared<ui::PreviewFrameCache>(128ULL * 1024 * 1024);
    std::size_t submissions = 0;
    runtime::TaskId latestTask;
    const ui::PreviewPreparationSubmitter submitter =
        [&](TaskRequest request, const document::Snapshot& snapshot,
            const PreviewRequestIdentity& identity, const std::size_t limit,
            const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            ++submissions;
            auto submitted =
                service.submit(std::move(request), snapshot, identity, limit, overrides);
            latestTask = submitted.handle.id();
            return submitted;
        };
    const auto pipeline = ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                                             referenceDisplay, fixture.provider);
    ui::CompositionPreviewSettings settings;
    settings.pixelStorageByteLimit = kBudget;
    settings.resolutionPolicy = PreviewResolutionPolicy::Full;
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline, settings,
                                                cache, nullptr, submitter, {}, policy);
    const auto failed = [&] { return controller.state().activity == ui::PreviewActivity::Failed; };
    const auto ready = [&] {
        return controller.state().activity == ui::PreviewActivity::Ready &&
               controller.state().frame && isResident(controller.state().frame);
    };
    checks.expect(waitUntil(failed, 15s) && preparations.load() == 1 && submissions == 1 &&
                      !controller.state().diagnostics.empty() &&
                      controller.state().diagnostics.front().admissionRetry &&
                      controller.state().diagnostics.front().admissionRetry->revision ==
                          policy->snapshot().reservations.admissionRevision &&
                      service.status().counters.residentGraphJobs == 0,
                  "real native retain refusal emits exact own-disposal watermark without dispatch");
    if (checks.failures() != 0)
        return;
    for (int index = 0; index < 20; ++index)
        controller.notifyProcessingCapacityChanged();
    QGuiApplication::processEvents();
    checks.expect(submissions == 1 && preparations.load() == 1,
                  "own failed phase disposal alone cannot create native retry spin");
    ownerControl(held, false);
    held.reset();
    controller.notifyProcessingCapacityChanged();
    checks.expect(waitUntil(ready, 15s) && preparations.load() == 2 && submissions == 2 &&
                      service.status().counters.residentGraphJobs == 1,
                  "external slot release admits genuine native recovery");
    if (checks.failures() != 0)
        return;
    const auto goodFrame = controller.state().frame;
    held = competing();
    ownerControl(held, true);
    controller.requestRefresh();
    // Hold Qt delivery while the real owner finishes the refusal; inspect terminal metadata only.
    std::optional<runtime::TaskSnapshot> terminal;
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
        terminal = scheduler.snapshot(latestTask);
        if (terminal && terminal->state == TaskState::Failed)
            break;
        std::this_thread::yield();
    }
    checks.expect(terminal && terminal->state == TaskState::Failed &&
                      !terminal->diagnostics.empty() &&
                      terminal->diagnostics.front().admissionRetry,
                  "real native failure is terminal before UI delivery");
    if (checks.failures() != 0 || !terminal || terminal->diagnostics.empty())
        return;
    const auto& retry = terminal->diagnostics.front().admissionRetry;
    if (!retry)
        return;
    const auto watermark = retry->revision;
    ownerControl(held, false);
    held.reset();
    checks.expect(policy->snapshot().reservations.admissionRevision > watermark &&
                      controller.state().frame == goodFrame,
                  "external release before delivery is distinct and preserves last good image");
    checks.expect(waitUntil(ready, 15s) && preparations.load() == 4 && submissions == 4,
                  "delivering refusal retains earlier external release and retries exactly once");
    checks.expect(session.setCurrentTime(core::RationalTime::fromInteger(1)) &&
                      waitUntil(ready, 15s),
                  "second native time is prepared and cached");
    const auto warmPreparations = preparations.load();
    const auto warmJobs = service.status().counters.residentGraphJobs;
    sample = makeSample(2, 9 * gib);
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    controller.notifyProcessingCapacityChanged();
    checks.expect(session.setCurrentTime(core::RationalTime::fromInteger(0)) && ready() &&
                      preparations.load() == warmPreparations &&
                      service.status().counters.residentGraphJobs == warmJobs,
                  "capacity revision preserves exact warm native seek with no repeated preparation "
                  "or dispatch");
    invalidMode.store(true);
    controller.requestRefresh();
    checks.expect(
        waitUntil(failed, 15s) && !controller.state().diagnostics.empty() &&
            controller.state().diagnostics.front().code ==
                "bloom.preview.gpu-scene.invalid-request" &&
            !controller.state().diagnostics.front().admissionRetry,
        "actual invalid GPU envelope reaches terminal UI diagnostic without retry metadata");
    const auto invalidPreparations = preparations.load();
    sample = makeSample(3, 10 * gib);
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    for (int index = 0; index < 20; ++index)
        controller.notifyProcessingCapacityChanged();
    QGuiApplication::processEvents();
    checks.expect(preparations.load() == invalidPreparations,
                  "non-capacity native-stage failure cannot retry on fresh capacity");
    controller.beginShutdown();
    service.beginShutdown();
    bridge.stop(); // This helper borrows the scheduler used by the following native fixture.
    checks.expect(
        waitUntil([&] { return service.status().state == GpuPreviewDisplayServiceState::Stopped; },
                  15s),
        "retry native service stops without unproven reservation");
    std::cout << "retry native proof: submissions=" << submissions
              << " preparations=" << preparations.load() << " warmGraphJobs=" << warmJobs << '\n';
}

// Actual retained native intermediates can exceed a peak phase envelope even though each
// operation's live peak fits. This witnesses production typed allocation refusal and UI retry.
void nativeImageCacheRetryChecks(Expectations& checks, Fixture& fixture, TaskScheduler& scheduler,
                                 const std::filesystem::path& loader,
                                 const runtime::CpuReferenceDisplayPreparer& referenceDisplay) {
    constexpr std::uint64_t gib = 1ULL << 30;
    auto project =
        document::makeNewProject("Native image retry", "Main", core::RationalTime::fromInteger(4),
                                 format1920x1080NonSquare());
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, project.initialCompositionId);
    checks.expect(session.addSolidLayer("Last good", {0.125, 0.25, 0.5, 1.0}),
                  "native image retry owns an actual authored baseline");
    auto policy = std::make_shared<runtime::GpuProcessingPhasePolicy>(2);
    runtime::MemoryBudgetState sample;
    sample.machine.availableBytes = 60 * gib;
    sample.process.status = runtime::ProcessMemoryStatus::Finite;
    sample.process.source = runtime::ProcessMemorySource::LinuxCgroupV2;
    sample.process.domains.push_back({.path = "/native-image-retry",
                                      .kind = runtime::ProcessMemoryLimitKind::Finite,
                                      .limitBytes = 8 * gib,
                                      .currentBytes = 0});
    sample.processSampleFresh = true;
    const auto fresh = [&] {
        sample.process.sampledAt = runtime::GpuProcessingPhasePolicy::Clock::now();
        ++sample.sampleSerial;
        policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    };
    fresh();
    runtime::GpuScenePreparationPhase phase{.policy = policy,
                                            .envelope = {.metadataBytes = 1ULL << 20,
                                                         .retainedCoverageByteLimit = 16ULL << 20,
                                                         .requestedExecutorBytes = gib,
                                                         .minimumExecutorBytes = gib}};
    runtime::CpuGpuSceneBuilder highBuilder(
        nullptr, runtime::GpuSceneMediaContext::fromEvaluator(fixture.evaluator), {}, phase);
    phase.envelope.requestedExecutorBytes = 512ULL << 20;
    phase.envelope.minimumExecutorBytes = 512ULL << 20;
    runtime::CpuGpuSceneBuilder lowBuilder(
        nullptr, runtime::GpuSceneMediaContext::fromEvaluator(fixture.evaluator), {}, phase);
    const auto highStage =
        ui::makeCompositionPreviewGpuSceneStage(fixture.compiler, highBuilder, fixture.provider);
    const auto lowStage =
        ui::makeCompositionPreviewGpuSceneStage(fixture.compiler, lowBuilder, fixture.provider);
    std::atomic_bool low{false};
    std::atomic_size_t preparations{0}, cpu{0};
    auto options = serviceOptions(loader);
    options.processingPhasePolicy = policy;
    options.previewByteAllowance = gib;
    options.residentSceneCacheBudgets.maxRetainedBytes = gib;
    options.residentLeaseBudgets.maxBytes = 64ULL << 20;
    const auto actualCpu = fixture.productionCpuStage();
    GpuPreviewDisplayService service(
        scheduler,
        [highStage, lowStage, &low, &preparations](
            const document::Snapshot& snapshot, const PreviewRequestIdentity& identity,
            std::size_t bytes, const std::vector<runtime::SnapshotParameterOverride>& overrides,
            TaskContext& context) {
            ++preparations;
            auto result =
                (low.load() ? lowStage : highStage)(snapshot, identity, bytes, overrides, context);
            for (const auto& diagnostic : result.diagnostics())
                std::cout << "NATIVE_CACHE_STAGE code=" << diagnostic.code
                          << " summary=" << diagnostic.summary << '\n';
            if (result.value() && *result.value())
                for (const auto& diagnostic : (*result.value())->diagnostics)
                    std::cout << "NATIVE_CACHE_SUBSET code=" << diagnostic.code
                              << " summary=" << diagnostic.summary << '\n';
            return result;
        },
        [actualCpu, &cpu](const document::Snapshot& snapshot,
                          const PreviewRequestIdentity& identity, std::size_t bytes,
                          const std::vector<runtime::SnapshotParameterOverride>& overrides,
                          TaskContext& context) {
            ++cpu;
            return actualCpu(snapshot, identity, bytes, overrides, context);
        },
        fixture.productionCpuFallback(referenceDisplay), options);
    checks.expect(
        waitUntil(
            [&] { return service.status().state != GpuPreviewDisplayServiceState::Initializing; },
            90s) &&
            service.status().state == GpuPreviewDisplayServiceState::Ready,
        "native image retry service is genuinely ready");
    if (checks.failures())
        return;
    auto core = runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(service);
    ui::TaskUiBridge bridge(scheduler);
    std::size_t submissions = 0;
    const ui::PreviewPreparationSubmitter submitter =
        [&](TaskRequest request, const document::Snapshot& snapshot,
            const PreviewRequestIdentity& identity, std::size_t bytes,
            const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            ++submissions;
            return service.submit(std::move(request), snapshot, identity, bytes, overrides);
        };
    auto pipeline = ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                                       referenceDisplay, fixture.provider);
    ui::CompositionPreviewSettings settings;
    settings.pixelStorageByteLimit = gib;
    settings.resolutionPolicy = PreviewResolutionPolicy::Full;
    auto cache = std::make_shared<ui::PreviewFrameCache>(64ULL << 20);
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline, settings,
                                                cache, nullptr, submitter, {}, policy);
    const auto ready = [&] {
        return controller.state().activity == ui::PreviewActivity::Ready &&
               isResident(controller.state().frame);
    };
    checks.expect(waitUntil(ready, 15s), "native image retry has real last-good output");
    if (checks.failures())
        return;
    auto good = controller.state().frame;
    const auto baselineCounters = service.status().counters;
    const auto baselineSubmissions = submissions;
    checks.expect(policy->snapshot().reservations.admittedPhases == 0,
                  "last-good baseline is retired before gated graph authoring");
    controller.setCachePurgeGate(true);
    low.store(true);
    fresh();
    for (int index = 0; index < 10; ++index)
        checks.expect(session.addSolidLayer(QString("Retained %1").arg(index),
                                            {0.02 * (index + 1), 0.01 * (index + 1), 0.1, 0.5}),
                      "actual new full-frame solid graph is authored");
    checks.expect(submissions == baselineSubmissions,
                  "gated graph authoring submits no intermediate pixel work");
    controller.setCachePurgeGate(false);
    checks.expect(
        waitUntil([&] { return controller.state().activity == ui::PreviewActivity::Failed; }, 15s),
        "retained native graph reaches a terminal refusal");
    const auto refused = policy->snapshot().reservations;
    auto state = controller.state();
    std::cout << "NATIVE_CACHE_RETRY submissions=" << submissions << " prep=" << preparations.load()
              << " cpu=" << cpu.load() << " nativeOwned=" << refused.ownedNativeImageBytes
              << " count=" << refused.ownedNativeImages << " phases=" << refused.admittedPhases
              << " graphs="
              << service.status().counters.residentGraphJobs - baselineCounters.residentGraphJobs
              << " dispatches="
              << service.status().counters.nativeDispatches - baselineCounters.nativeDispatches
              << " revision=" << refused.admissionRevision
              << " detail=" << service.status().residentDetail << '\n';
    for (const auto& diagnostic : state.diagnostics) {
        std::cout << "NATIVE_CACHE_DIAGNOSTIC code=" << diagnostic.code
                  << " message=" << diagnostic.summary << " retry="
                  << (diagnostic.admissionRetry ? diagnostic.admissionRetry->revision : 0) << '\n';
    }
    if (state.diagnostics.empty()) {
        checks.expect(false, "native allocation refusal has a terminal diagnostic");
        return;
    }
    const auto& diagnostic = state.diagnostics.front();
    const auto& admissionRetry = diagnostic.admissionRetry;
    if (!admissionRetry) {
        checks.expect(false, "native allocation refusal has exact capacity retry metadata");
        return;
    }
    checks.expect(
        diagnostic.code == "preview.processing-phase.budget-refused" &&
            diagnostic.summary == "The native image allocation is awaiting processing capacity." &&
            service.status().counters.residentGraphJobs > baselineCounters.residentGraphJobs &&
            service.status().counters.nativeDispatches > baselineCounters.nativeDispatches &&
            admissionRetry->revision == refused.admissionRevision && refused.admittedPhases == 0 &&
            refused.ownedNativeImages > 0 && cpu.load() == 0 && state.frame == good,
        "actual image allocation refusal keeps cache backing/last-good and exact own "
        "watermark without CPU substitution");
    if (checks.failures())
        return;
    const auto failedSubmissions = submissions;
    for (int index = 0; index < 20; ++index)
        controller.notifyProcessingCapacityChanged();
    QGuiApplication::processEvents();
    checks.expect(submissions == failedSubmissions,
                  "unchanged native revision pulses cannot self-retry");
    low.store(false);
    auto purge = scheduler.submitGpu<int>(
        TaskRequest("native image cache release",
                    {.kind = TaskOwnerKind::Application, .id = TaskOwnerId::fromRaw(1901)},
                    runtime::TaskPriority::Interactive, runtime::TaskExecutor::Gpu),
        core->generation, runtime::GpuTaskAdmission{0, 0},
        [core](TaskContext&, runtime::GpuTaskCompletion<int> completion) {
            core->residentSceneCache->clear();
            static_cast<void>(std::move(completion).complete(TaskResult<int>::succeeded(1)));
        });
    auto purged = purge.accepted() ? awaitResult(purge.handle) : std::nullopt;
    checks.expect(purged && purged->state() == TaskState::Succeeded &&
                      policy->snapshot().reservations.admissionRevision > refused.admissionRevision,
                  "actual native cache release is independently observable");
    fresh();
    controller.notifyProcessingCapacityChanged();
    checks.expect(waitUntil(ready, 15s) && submissions == failedSubmissions + 1 &&
                      cpu.load() == 0 && controller.state().desiredIdentity &&
                      controller.state().frame->desiredIdentity() ==
                          *controller.state().desiredIdentity,
                  "one independent native release recovers newest authored intent on GPU");
    if (checks.failures())
        return;
    checks.expect(session.setCurrentTime(core::RationalTime::fromInteger(1)) &&
                      waitUntil(ready, 15s),
                  "second actual native time prepares a retained output");
    const auto warm = service.status().counters;
    const auto warmClaims = policy->snapshot().reservations;
    const auto warmPreparations = preparations.load();
    checks.expect(
        session.setCurrentTime(core::RationalTime::fromInteger(0)) && ready() &&
            preparations.load() == warmPreparations &&
            service.status().counters.residentGraphJobs == warm.residentGraphJobs &&
            service.status().counters.nativeDispatches == warm.nativeDispatches &&
            service.status().counters.packedTransfers == warm.packedTransfers &&
            controller.state().desiredIdentity &&
            controller.state().frame->desiredIdentity() == *controller.state().desiredIdentity &&
            policy->snapshot().reservations.ownedNativeImageBytes ==
                warmClaims.ownedNativeImageBytes &&
            policy->snapshot().reservations.ownedNativeImages == warmClaims.ownedNativeImages,
        "ordinary warm retained-frame return performs no preparation, graph, dispatch, "
        "or new native allocation");
    controller.beginShutdown();
    controller.releasePublishedFramesForShutdown();
    state.frame.reset();
    good.reset();
    service.beginShutdown();
    bridge.stop();
    checks.expect(
        waitUntil([&] { return service.status().state == GpuPreviewDisplayServiceState::Stopped; },
                  15s) &&
            policy->snapshot().reservations.ownedNativeImageBytes == 0 &&
            policy->snapshot().reservations.totalDeviceClaimBytes == 0,
        "production native cache/controller service fully retires actual backing");
}
