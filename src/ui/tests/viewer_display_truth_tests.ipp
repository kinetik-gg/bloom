// Actual CPU publication and per-viewer display failure/cover contract. Runs only offscreen.
void testViewerActualAdjustmentFailureAndCover(Expectations& checks) {
    QSettings().remove("viewer/analysis/default");
    auto project = makeTestProject("Display truth");
    const auto compositionId = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    checks.expect(session.addSolidLayer("Marker", {0.125, 0.25, 0.5, 1.0}),
                  "a distinct marker is authored");
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    runtime::NodeDefinitionRegistry definitions;
    checks.expect(runtime::registerBuiltInNodeDefinitions(definitions), "definitions register");
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer preparer;
    runtime::QualifiedDisplayProcessorProvider qualified;
    auto resolution = color::resolveBloomNeutralV1BuiltIn(
        color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
        color::kBloomNeutralV1ConfigDigest);
    auto resolved = std::move(resolution).takeResolved();
    checks.expect(resolved.has_value(), "the qualified processor resolves");
    if (!resolved)
        return;
    auto built = color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    auto handle = std::move(built).takeHandle();
    checks.expect(handle.has_value(), "the qualified processor builds");
    if (!handle)
        return;
    qualified.publish(runtime::QualifiedDisplayProcessorBuildResult::ready(
        std::make_shared<const color::PreparedCpuDisplayProcessorHandle>(std::move(*handle))));
    const auto pipeline =
        ui::makeCompositionPreviewPipeline(compiler, evaluator, preparer, qualified);
    std::atomic<bool> blocked{true}, fail{true};
    std::atomic<bool> allowLateCompletion{false};
    std::atomic<bool> latePreparedSucceeded{false};
    std::atomic<int> lateCompletions{0};
    std::atomic<int> adjustmentCalls{0};
    ui::PreviewPreparationFunction controlled =
        [&](const document::Snapshot& snapshot, const runtime::PreviewRequestIdentity& identity,
            std::size_t budget, const std::vector<runtime::SnapshotParameterOverride>& overrides,
            runtime::TaskContext& context) {
            if (!identity.viewAdjust.neutral()) {
                ++adjustmentCalls;
                if (allowLateCompletion) {
                    auto completed = pipeline(snapshot, identity, budget, overrides, context);
                    latePreparedSucceeded = completed.state() == runtime::TaskState::Succeeded;
                    while (blocked)
                        std::this_thread::sleep_for(1ms);
                    ++lateCompletions;
                    return completed;
                }
                while (blocked && !context.isCancellationRequested())
                    std::this_thread::sleep_for(1ms);
                if (context.isCancellationRequested())
                    return runtime::TaskResult<ui::PreviewPreparationResultHandle>::cancelled();
                if (fail)
                    return runtime::TaskResult<ui::PreviewPreparationResultHandle>::failed(
                        {.code = "bloom.test.adjustment-refused",
                         .severity = runtime::DiagnosticSeverity::Error,
                         .summary = "Requested viewer adjustment was refused",
                         .detail = {},
                         .suggestedAction = {}});
            }
            return pipeline(snapshot, identity, budget, overrides, context);
        };
    ui::CompositionPreviewController controller(session, scheduler, bridge, controlled);
    ui::ViewerEditor viewer(session, controller);
    viewer.resize(400, 300);
    checks.expect(
        waitUntil([&] { return controller.state().activity == ui::PreviewActivity::Ready; }),
        "a qualified candidate prepares without a paint");
    checks.expect(!viewer.displayedFrameForTest() && !viewer.mappingForTest() &&
                      viewer.displayedPreviewState().freshness == ui::FrameFreshness::None &&
                      viewer.displayedPreviewState().activity != ui::PreviewActivity::Ready,
                  "an unpainted hidden viewer cannot claim displayed readiness or hit geometry");
    viewer.show();
    checks.expect(waitUntil([&] { return viewer.displayedFrameForTest() != nullptr; }),
                  "the actual CPU paint publishes the qualified frame");
    const auto original = viewer.displayedFrameForTest();
    checks.expect(original && original->isOcioQualified(), "the last-good frame is qualified");
    ui::WindowStatusBar status(session, &controller);
    status.setDisplayedViewer(&viewer);
    auto* exposure = viewer.findChild<ui::kit::KValueField*>("viewerExposure");
    checks.expect(exposure != nullptr, "the real adjustment control exists");
    if (!exposure)
        return;
    exposure->stepBy(10);
    checks.expect(waitUntil([&] { return adjustmentCalls.load() == 1; }),
                  "the adjusted task is held off-thread");
    const auto pending = viewer.displayedPreviewState();
    checks.expect(
        pending.frame && pending.frame == original &&
            pending.freshness == ui::FrameFreshness::Stale && pending.desiredIdentity &&
            pending.desiredIdentity->viewAdjust.exposure == 1.0 &&
            pending.frame->desiredIdentity().viewAdjust.neutral(),
        "pending desired EV retains actual neutral last-good with separate stale identity");
    checks.expect(status.colorChipTextForTest() == "Qualified · Bloom Neutral",
                  "pending work preserves qualification");
    blocked = false;
    checks.expect(waitUntil([&] {
                      return viewer.displayedPreviewState().activity == ui::PreviewActivity::Failed;
                  }),
                  "the actual adjustment refusal is published");
    checks.expect(
        viewer.displayedFrameForTest() == original &&
            status.colorChipTextForTest() == "Qualified · Bloom Neutral" &&
            status.previewStateTextForTest().contains("Requested viewer adjustment was refused"),
        "failure keeps qualified last-good provenance and concrete diagnostic");
    const auto failedCalls = adjustmentCalls.load();
    for (int index = 0; index < 20; ++index) {
        viewer.pollGpuResidentForTest();
        QCoreApplication::processEvents();
    }
    checks.expect(adjustmentCalls == failedCalls,
                  "the identical failed adjustment is not retried per poll");
    fail = false;
    exposure->stepBy(10);
    checks.expect(waitUntil([&] {
                      const auto shown = viewer.displayedFrameForTest();
                      return shown && shown->desiredIdentity().viewAdjust.exposure == 2.0 &&
                             viewer.displayedPreviewState().freshness ==
                                 ui::FrameFreshness::Current;
                  }),
                  "a changed desired adjustment recovers through real CPU publication");
    const auto adjusted = viewer.displayedFrameForTest();
    checks.expect(
        controller.state().frame->desiredIdentity().viewAdjust.neutral() &&
            status.previewStateTextForTest().contains("Shown EV 2"),
        "shared preparation remains neutral while selected-viewer status names actual EV");
    const auto coverMatchesFrame = [&](const QPixmap& snapshot,
                                       const ui::ViewerDisplayDescriptor& descriptor) {
        const auto pixels = descriptor.frame ? descriptor.frame->displayBufferView() : std::nullopt;
        if (snapshot.isNull() || !descriptor.mapping || !pixels)
            return false;
        const auto& rect = descriptor.mapping->displayRect;
        // Interior of the solid, away from frame/selection edges and the centre anchor.
        const auto point = rect.topLeft() + QPointF(rect.width() * 0.25, rect.height() * 0.25);
        const auto local = (point - descriptor.captureRect.topLeft()) * descriptor.devicePixelRatio;
        const auto image = snapshot.toImage();
        const auto expected =
            QImage(reinterpret_cast<const uchar*>(pixels->pixels.data()),
                   static_cast<int>(pixels->displayWindow.extent().width()),
                   static_cast<int>(pixels->displayWindow.extent().height()),
                   static_cast<qsizetype>(pixels->layout.rowStrideBytes), QImage::Format_RGBA8888)
                .pixelColor(0, 0);
        if (!image.rect().contains(local.toPoint()))
            return false;
        const auto actual = image.pixelColor(local.toPoint());
        return std::abs(actual.red() - expected.red()) <= 2 &&
               std::abs(actual.green() - expected.green()) <= 2 &&
               std::abs(actual.blue() - expected.blue()) <= 2 && actual.alpha() == expected.alpha();
    };
    const auto cover = viewer.renderCpuCoverSnapshotForTest();
    checks.expect(!cover.isNull() && viewer.cpuCoverDescriptorForTest() &&
                      viewer.cpuCoverDescriptorForTest()->frame == adjusted &&
                      viewer.cpuCoverDescriptorForTest()->mapping,
                  "a cover capture records its exact frame and geometry");
    checks.expect(viewer.cpuCoverDescriptorForTest() &&
                      coverMatchesFrame(cover, *viewer.cpuCoverDescriptorForTest()),
                  "the adjusted cover actually paints the prepared adjusted solid pixels");
    viewer.setCpuCoverByteBudgetForTest(0);
    const auto unavailable = viewer.renderCpuCoverSnapshotForTest();
    checks.expect(unavailable.isNull() && viewer.cpuCoverDescriptorForTest() &&
                      !viewer.cpuCoverDescriptorForTest()->frame &&
                      !viewer.cpuCoverDescriptorForTest()->mapping,
                  "a refused cover capture installs unavailable metadata instead of old pixels");
    viewer.setCpuCoverByteBudgetForTest(ui::kResidentOverlayByteBudget);
    allowLateCompletion = true;
    blocked = true;
    exposure->stepBy(10);
    checks.expect(waitUntil([&] {
                      return adjustmentCalls.load() > failedCalls + 1 && latePreparedSucceeded;
                  }),
                  "the next genuine adjusted result is held before completion delivery");
    const auto retainedCover = viewer.renderCpuCoverSnapshotForTest();
    const auto& retainedDescriptor = viewer.cpuCoverDescriptorForTest();
    checks.expect(
        !retainedCover.isNull() && retainedDescriptor && retainedDescriptor->frame &&
            retainedDescriptor->frame == adjusted &&
            retainedDescriptor->frame->desiredIdentity().viewAdjust.exposure == 2.0,
        "a pending adjustment cover describes the captured previous intent, not desired EV 3");
    checks.expect(retainedDescriptor && coverMatchesFrame(retainedCover, *retainedDescriptor),
                  "pending adjustment retains the prior adjusted solid pixels in the cover");
    document::Document sameIds(session.snapshot().project());
    commands::CommandStack sameIdsCommands(sameIds);
    ui::CompositionSession replacementSession(sameIds, sameIdsCommands, compositionId);
    const auto* marker = std::get_if<document::LayerId>(&session.selection().primary);
    checks.expect(marker != nullptr, "the marker layer has a selected stable ID");
    if (marker) {
        replacementSession.selectLayer(*marker);
        checks.expect(replacementSession.setSelectedSolidColor({0.0, 0.0, 0.5, 1.0}),
                      "the colliding document owns distinct blue solid pixels");
    }
    checks.expect(sameIds.snapshot().project().id() == session.snapshot().project().id() &&
                      sameIds.snapshot().revision() == session.snapshot().revision() &&
                      replacementSession.compositionId() == session.compositionId(),
                  "the replacement collides in project, composition and revision numeric IDs");
    session.rebind(sameIds, sameIdsCommands, compositionId);
    checks.expect(
        !viewer.displayedFrameForTest() && !viewer.mappingForTest() &&
            !viewer.cpuCoverDescriptorForTest(),
        "explicit document rebind invalidates same-ID retained display and cover ownership");
    allowLateCompletion = false;
    blocked = false;
    checks.expect(
        waitUntil([&] {
            const auto shown = viewer.displayedFrameForTest();
            return lateCompletions == 1 && shown && shown != adjusted &&
                   shown->desiredIdentity().viewAdjust.exposure == 3.0 &&
                   viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current;
        }),
        "a detached old adjustment completion cannot replace the new same-ID actual frame");
    const auto replacementFrame = viewer.displayedFrameForTest();
    bool replacementIsBlue = false;
    if (replacementFrame && replacementFrame->hasProcessFrame() && adjusted->hasProcessFrame()) {
        const auto pixel = replacementFrame->processImage().read(0, 0);
        const auto oldPixel = adjusted->processImage().read(0, 0);
        replacementIsBlue = pixel && oldPixel && pixel.value()->blue() > pixel.value()->red() &&
                            pixel.value()->red() < oldPixel.value()->red();
    }
    checks.expect(
        replacementIsBlue,
        "the current same-ID adjusted display contains the replacement document's blue pixels");
    exposure->stepBy(-30);
    checks.expect(
        waitUntil([&] {
            const auto shown = viewer.displayedFrameForTest();
            return shown && shown->desiredIdentity().viewAdjust.neutral() &&
                   scheduler.isQuiescent();
        }),
        "neutral transition cancels pending work and publishes the real neutral candidate");
    document::Document blank(document::Project(document::ProjectId::fromRaw(77), "Other domain"));
    commands::CommandStack blankCommands(blank);
    viewer.simulateNativeRetireInFlightForTest();
    const auto retirement = viewer.internalNativeRetireGenerationForTest();
    session.rebind(blank, blankCommands, {});
    checks.expect(
        viewer.internalNativeRetirePendingForTest() &&
            viewer.internalNativeRetireGenerationForTest() == retirement &&
            !viewer.cpuCoverDescriptorForTest() && !viewer.displayedFrameForTest(),
        "a domain rebind clears foreign cover ownership before the retirement early return");
    viewer.finishSimulatedNativeRetireForTest(true);
    QCoreApplication::processEvents();
    const auto blankCover = viewer.renderCpuCoverSnapshotForTest();
    checks.expect(!viewer.displayedFrameForTest() && !viewer.mappingForTest() &&
                      !blankCover.isNull() && viewer.cpuCoverDescriptorForTest() &&
                      !viewer.cpuCoverDescriptorForTest()->frame &&
                      controller.currentLayerBounds(original).empty() &&
                      controller.selectedLayerBounds(original).empty(),
                  "domain clear publishes blank metadata and refuses foreign retained bounds/hits");
    exposure->stepBy(10);
    checks.expect(viewer.displayedPreviewState().activity == controller.state().activity &&
                      viewer.displayedPreviewState().message == controller.state().message,
                  "non-neutral controls cannot mask the no-composition state as adjustment work");
    controller.beginShutdown();
    bridge.beginShutdown();
    checks.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                  "display truth fixture drains");
    QSettings().remove("viewer/analysis/default");
}

void testViewerSharedFailureWithoutCandidate(Expectations& checks) {
    QSettings().remove("viewer/analysis/default");
    auto project = makeTestProject("Shared failure truth");
    const auto compositionId = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    std::atomic<bool> unsupported{false};
    ui::PreviewPreparationFunction refuse =
        [&](const document::Snapshot&, const runtime::PreviewRequestIdentity&, std::size_t,
            const std::vector<runtime::SnapshotParameterOverride>&, runtime::TaskContext&) {
            const runtime::TaskDiagnostic diagnostic{.code = "bloom.test.shared-display-refused",
                                                     .severity = runtime::DiagnosticSeverity::Error,
                                                     .summary =
                                                         "Shared display configuration refused",
                                                     .detail = {},
                                                     .suggestedAction = {}};
            if (unsupported)
                return runtime::TaskResult<ui::PreviewPreparationResultHandle>::succeeded(
                    std::make_shared<const runtime::PreviewPreparationResult>(
                        runtime::PreviewPreparationResult::unsupported()),
                    {diagnostic});
            return runtime::TaskResult<ui::PreviewPreparationResultHandle>::failed(diagnostic);
        };
    ui::CompositionPreviewController controller(session, scheduler, bridge, refuse);
    ui::ViewerEditor viewer(session, controller);
    checks.expect(
        waitUntil([&] { return controller.state().activity == ui::PreviewActivity::Failed; }),
        "the shared pipeline fails before any candidate exists");
    auto* exposure = viewer.findChild<ui::kit::KValueField*>("viewerExposure");
    if (exposure)
        exposure->stepBy(10);
    checks.expect(exposure && !viewer.displayedFrameForTest() &&
                      viewer.displayedPreviewState().activity == ui::PreviewActivity::Failed &&
                      viewer.displayedPreviewState().message == controller.state().message,
                  "non-neutral missing candidate preserves the concrete shared failure");
    unsupported = true;
    controller.requestRefresh();
    checks.expect(
        waitUntil([&] { return controller.state().activity == ui::PreviewActivity::Unsupported; }),
        "the shared unsupported result is observed");
    checks.expect(viewer.displayedPreviewState().activity == ui::PreviewActivity::Unsupported &&
                      viewer.displayedPreviewState().message == controller.state().message,
                  "local pending text never overwrites shared Unsupported diagnostics");
    controller.beginShutdown();
    bridge.beginShutdown();
    checks.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                  "shared failure fixture drains");
    QSettings().remove("viewer/analysis/default");
}
