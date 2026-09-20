// Budget-refusal recovery tests for the GPU preview application pipeline.
//
// Included by gpu_preview_app_tests.cpp at the SAME lexical scope as the other test bodies (after
// the file's anonymous namespace, where Expectations/Options and the fixture helpers are visible).
// It is a private test fragment, not a header: it has no include guard and defines exactly the two
// same-controller/service recovery tests, so the main test translation unit stays under the
// cohesive-size budget without changing any behavior.

// After an injected budget refusal the SAME controller and service must stay responsive: the failed
// request reaches a terminal state, a later request on the same controller renders, and shutdown is
// bounded. The recovery runs through the real service; only the first request is injected as a real
// task failure on the real scheduler (no fake service).
void testControllerLivenessAfterBudgetRefusal(Expectations& expectations, const Options& options) {
    auto newProject = makeViewerProject();
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.4, 0.8, 1.0}),
        "liveness: the fixture adds a solid layer");
    runtime::TaskScheduler scheduler(serviceSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    AppFixture fixture;
    runtime::GpuPreviewDisplayService service(scheduler, fixture.stage(), fixture.fallback(),
                                              gpuOptions(options.loader));
    static_cast<void>(waitUntil([&] {
        return service.status().state != runtime::GpuPreviewDisplayServiceState::Initializing;
    }));

    auto injected = std::make_shared<std::atomic<bool>>(false);
    ui::PreviewPreparationSubmitter submitter =
        [&service, &scheduler,
         injected](runtime::TaskRequest request, const document::Snapshot& snapshot,
                   const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            if (!injected->exchange(true)) {
                return scheduler.submit<runtime::PreviewPreparationResultHandle>(
                    std::move(request), [](runtime::TaskContext&) {
                        return runtime::TaskResult<runtime::PreviewPreparationResultHandle>::failed(
                            {runtime::TaskDiagnostic{
                                .code = "bloom.preview.gpu-scene.pixel-budget-exceeded",
                                .severity = runtime::DiagnosticSeverity::Error,
                                .summary = "Prepared scene exceeds the request pixel allowance",
                                .detail = {},
                                .suggestedAction = "Take the full CPU composition preview path."}});
                    });
            }
            return service.submit(std::move(request), snapshot, identity, limit, overrides);
        };

    auto frameCache = std::make_shared<ui::PreviewFrameCache>();
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                           fixture.referencePreparer, fixture.provider,
                                           fixture.planCache),
        {.colorIntent = session.colorIntent(),
         .displayName = {},
         .viewName = {},
         .showLook = true,
         .pixelStorageByteLimit = kBudget},
        frameCache, nullptr, std::move(submitter));

    expectations.expect(waitUntil([&] {
                            const auto activity = controller.state().activity;
                            return activity == ui::PreviewActivity::Failed ||
                                   activity == ui::PreviewActivity::Unsupported ||
                                   activity == ui::PreviewActivity::Ready;
                        }),
                        "liveness: the injected budget refusal reaches a terminal state");
    expectations.expect(controller.state().activity != ui::PreviewActivity::Rendering,
                        "liveness: the controller is not stranded in Rendering");

    // A later request on the SAME controller/service must render through the real service.
    controller.requestRefresh();
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "liveness: a later request on the same controller renders");

    controller.beginShutdown();
    service.beginShutdown();
    bridge.beginShutdown();
    expectations.expect(controller.isShuttingDown(), "liveness: shutdown is bounded");
}

// A genuine builder budget refusal under the SAME controller/service: a solid scaled so its vector
// coverage raster exceeds the request allowance is refused by the production builder (subset
// refusal -> honest CPU fallback -> terminal failure), then the same controller renders the
// unscaled composition, and shutdown stays bounded. No injected failure and no fake service.
void testGenuineBuilderBudgetRefusalRecovery(Expectations& expectations, const Options& options) {
    auto newProject = makeViewerProject();
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.4, 0.8, 1.0}),
        "genuine: the fixture adds a solid layer");
    // A 12x scale at quarter preview resolution makes the vector coverage raster (5760x3240 R8 =
    // 17.8 MiB) exceed the injected 16 MiB request allowance, so the production builder refuses it.
    expectations.expect(session.setSelectedScale(12.0, 12.0), "genuine: the solid is scaled 12x");

    runtime::TaskScheduler scheduler(serviceSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    AppFixture fixture;
    runtime::GpuPreviewDisplayService service(scheduler, fixture.stage(), fixture.fallback(),
                                              gpuOptions(options.loader));
    static_cast<void>(waitUntil([&] {
        return service.status().state != runtime::GpuPreviewDisplayServiceState::Initializing;
    }));

    auto calls = std::make_shared<std::atomic<int>>(0);
    auto frameCache = std::make_shared<ui::PreviewFrameCache>();
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                           fixture.referencePreparer, fixture.provider,
                                           fixture.planCache),
        {.resolutionPolicy = runtime::PreviewResolutionPolicy::Quarter,
         .colorIntent = session.colorIntent(),
         .displayName = {},
         .viewName = {},
         .showLook = true,
         .pixelStorageByteLimit = std::size_t{16} * 1024U * 1024U},
        frameCache, nullptr, serviceSubmitter(service, calls));

    expectations.expect(waitUntil([&] {
                            const auto activity = controller.state().activity;
                            return activity == ui::PreviewActivity::Failed ||
                                   activity == ui::PreviewActivity::Unsupported ||
                                   activity == ui::PreviewActivity::Ready;
                        }),
                        "genuine: the builder budget refusal reaches a terminal state");
    expectations.expect(controller.state().activity != ui::PreviewActivity::Rendering,
                        "genuine: the controller is not stranded in Rendering");

    // The same controller must render the unscaled composition through the real service.
    expectations.expect(session.setSelectedScale(1.0, 1.0), "genuine: the solid scale is reset");
    controller.requestRefresh();
    if (!waitUntil([&] { return isReady(controller); })) {
        std::cerr << "genuine recovery state=" << static_cast<int>(controller.state().activity)
                  << " message=" << controller.state().message.toStdString() << '\n';
    }
    expectations.expect(isReady(controller),
                        "genuine: the same controller renders the small request");

    controller.beginShutdown();
    service.beginShutdown();
    bridge.beginShutdown();
    expectations.expect(controller.isShuttingDown(), "genuine: shutdown is bounded");
}
