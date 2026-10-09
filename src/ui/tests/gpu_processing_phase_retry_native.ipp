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
