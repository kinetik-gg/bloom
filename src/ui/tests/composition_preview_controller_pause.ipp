void testPauseReplacesCancelledSameTime(Expectations& expectations) {
    using namespace bloom;
    auto project = makeTestProject("Pause recovery");
    const auto composition = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, composition);
    expectations.expect(session.addSolidLayer(QStringLiteral("Pause solid"), {0.5, 0.0, 0.0, 1.0}),
                        "pause regression authors a red source");
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler); // Use production delivery default.
    PipelineFixture fixture;
    WorkerGate playback;
    std::atomic<int> invocations = 0;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&playback, &invocations, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot, const runtime::PreviewRequestIdentity& identity,
            const std::size_t bytes,
            const std::vector<runtime::SnapshotParameterOverride>& overrides,
            runtime::TaskContext& context) {
            if (invocations.fetch_add(1) == 1)
                playback.enterAndWait();
            return pipeline(snapshot, identity, bytes, overrides, context);
        });
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "initial red frame is ready");
    controller.setPlaybackActive(true);
    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(1)),
                        "real playback time changes");
    controller.presentPlaybackFrame(1s);
    const bool entered = waitUntil([&] { return playback.entered(); });
    expectations.expect(entered, "playback request is held before terminal delivery");
    const auto beforePause = generation(controller.state());
    controller.setPlaybackActive(false);
    expectations.expect(generation(controller.state()) > beforePause &&
                            controller.state().activity == ui::PreviewActivity::Rendering,
                        "pause same desired time queues Visible newest intent behind cancellation");
    expectations.expect(session.setSelectedSolidColor({0.0, 0.5, 0.0, 1.0}),
                        "newest stopped source is green while cancelled task drains");
    playback.release();
    expectations.expect(
        waitUntil([&] {
            return isReady(controller) && controller.state().frame &&
                   controller.state().frame->desiredIdentity().time == session.currentTime() &&
                   controller.state().frame->desiredIdentity().sourceRevision ==
                       session.snapshot().revision();
        }),
        "stopped newest green source becomes Current after cancelled playback drains");
    const auto frame = controller.state().frame;
    const auto pixels = frame ? frame->displayBufferView() : std::nullopt;
    expectations.expect(pixels && !pixels->pixels.empty() &&
                            pixels->pixels.front().green > pixels->pixels.front().red &&
                            invocations.load() == 3,
                        "one newest replacement actually publishes green pixels with bounded work");
    const auto readyGeneration = generation(controller.state());
    controller.setPlaybackActive(true);
    controller.setPlaybackActive(false);
    QCoreApplication::processEvents();
    expectations.expect(generation(controller.state()) == readyGeneration &&
                            invocations.load() == 3,
                        "unchanged genuinely Current cached pause performs no additional work");
    controller.beginShutdown();
    controller.setPlaybackActive(true);
    controller.setPlaybackActive(false);
    expectations.expect(invocations.load() == 3, "shutdown pause cannot admit replacement work");
    reachQuiescence(controller, bridge, scheduler, expectations);
}

void testShutdownCannotRecoverStalePlayback(Expectations& expectations) {
    using namespace bloom;
    auto project = makeTestProject("Shutdown held playback");
    const auto composition = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, composition);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler);
    PipelineFixture fixture;
    WorkerGate held;
    std::atomic<int> invocations = 0;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&held, &invocations, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot, const runtime::PreviewRequestIdentity& identity,
            const std::size_t bytes,
            const std::vector<runtime::SnapshotParameterOverride>& overrides,
            runtime::TaskContext& context) {
            if (invocations.fetch_add(1) == 1)
                held.enterAndWait();
            return pipeline(snapshot, identity, bytes, overrides, context);
        });
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "shutdown control starts Current");
    controller.playbackController().play();
    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(1)),
                        "held playback time changes");
    controller.presentPlaybackFrame(1s);
    expectations.expect(waitUntil([&] { return held.entered(); }),
                        "shutdown playback is held on real worker");
    expectations.expect(controller.state().freshness == ui::FrameFreshness::Stale,
                        "shutdown starts with previous pixels Stale, not readyCurrent dedup");
    const auto beforeShutdown = generation(controller.state());
    controller.beginShutdown(); // Calls the real Playing controller's pause.
    expectations.expect(generation(controller.state()) == beforeShutdown,
                        "shutdown closes recovery domain before pausing transport");
    controller.setPlaybackActive(true);
    controller.setPlaybackActive(false);
    expectations.expect(generation(controller.state()) == beforeShutdown,
                        "direct Stale playback toggles after shutdown cannot queue recovery");
    held.release();
    reachQuiescence(controller, bridge, scheduler, expectations);
    expectations.expect(invocations.load() == 2,
                        "shutdown admits no replacement behind cancelled held task");
}

void testColdPlaybackCarriesUsefulFrames(Expectations& expectations) {
    using namespace bloom;
    auto project = makeTestProject("Useful cold playback");
    const auto composition = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, composition);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler);
    PipelineFixture fixture;
    WorkerGate first, newest, wrapped;
    std::atomic<int> invocations = 0;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&first, &newest, &wrapped, &invocations, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot, const runtime::PreviewRequestIdentity& identity,
            const std::size_t bytes,
            const std::vector<runtime::SnapshotParameterOverride>& overrides,
            runtime::TaskContext& context) {
            const auto index = invocations.fetch_add(1);
            if (index == 1)
                first.enterAndWait();
            if (index == 2)
                newest.enterAndWait();
            if (index == 3)
                wrapped.enterAndWait();
            return pipeline(snapshot, identity, bytes, overrides, context);
        });
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "cold control starts with real frame0");
    controller.setPlaybackActive(true);
    controller.beginDroppedFrameCounting();
    const auto ask = [&](int second) {
        expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(second)),
                            "cold playback target changes");
        controller.presentPlaybackFrame(
            1ns); // Force deadline miss without changing production clock.
    };
    ask(1);
    expectations.expect(waitUntil([&] { return first.entered(); }),
                        "one cold producer is held across ticks");
    ask(2);
    ask(3);
    first.release();
    expectations.expect(
        waitUntil([&] {
            return newest.entered() && controller.state().frame &&
                   controller.state().frame->desiredIdentity().time ==
                       core::RationalTime::fromInteger(1);
        }),
        "valid old completion makes visible progress and only newest pending starts");
    expectations.expect(controller.state().freshness == ui::FrameFreshness::Stale &&
                            controller.state().desiredIdentity &&
                            controller.state().desiredIdentity->time ==
                                core::RationalTime::fromInteger(3) &&
                            invocations.load() == 3 && controller.droppedFrameCount() == 2,
                        "own-time old pixels stay Stale, desired stays newest, drops count once");
    const auto ownKey = controller.cacheKeyForTime(core::RationalTime::fromInteger(1));
    expectations.expect(ownKey && controller.frameCache().contains(*ownKey),
                        "valid late product warms its own exact cache key");
    ask(2); // An uncached backward/looped target: generation ordering, not time ordering.
    newest.release();
    expectations.expect(
        waitUntil([&] {
            return wrapped.entered() && controller.state().frame &&
                   controller.state().frame->desiredIdentity().time ==
                       core::RationalTime::fromInteger(3);
        }),
        "useful newer-generation frame3 is visible while newest intent moves back to2");
    expectations.expect(controller.state().desiredIdentity &&
                            controller.state().desiredIdentity->time ==
                                core::RationalTime::fromInteger(2) &&
                            controller.state().freshness == ui::FrameFreshness::Stale,
                        "backward target keeps truthful desired2 versus displayed3");
    ask(1); // Immediate warm hit must beat older producer2 when that producer retires.
    const auto winningGeneration = generation(controller.state());
    expectations.expect(isReady(controller),
                        "newer cached exact-time frame wins immediately without evaluation");
    wrapped.release();
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                        "cancelled older producer retires after warm hit");
    QCoreApplication::processEvents();
    expectations.expect(
        isReady(controller) && generation(controller.state()) == winningGeneration &&
            controller.state().frame->desiredIdentity().time ==
                core::RationalTime::fromInteger(1) &&
            invocations.load() == 4,
        "older loop completion cannot rewind newer cached display and no extra worker is admitted");
    controller.setPlaybackActive(false);
    controller.endDroppedFrameCounting();
    reachQuiescence(controller, bridge, scheduler, expectations);
}

void testLatePlaybackCountsFailureOnce(Expectations& expectations) {
    using namespace bloom;
    for (const bool unsupported : {false, true}) {
        auto project = makeTestProject("Late failure accounting");
        const auto composition = project.initialCompositionId;
        document::Document document(std::move(project.project));
        commands::CommandStack commands(document);
        ui::CompositionSession session(document, commands, composition);
        runtime::TaskScheduler scheduler(testSchedulerConfig());
        ui::TaskUiBridge bridge(scheduler);
        PipelineFixture fixture;
        WorkerGate held;
        std::atomic<int> invocations = 0;
        ui::CompositionPreviewController controller(
            session, scheduler, bridge,
            [&held, &invocations, unsupported, pipeline = fixture.pipeline](
                const document::Snapshot& snapshot, const runtime::PreviewRequestIdentity& identity,
                const std::size_t bytes,
                const std::vector<runtime::SnapshotParameterOverride>& overrides,
                runtime::TaskContext& context) {
                if (invocations.fetch_add(1) == 0)
                    return pipeline(snapshot, identity, bytes, overrides, context);
                held.enterAndWait();
                if (unsupported)
                    return PipelineResult::succeeded(
                        std::make_shared<const runtime::PreviewPreparationResult>(
                            runtime::PreviewPreparationResult::unsupported()));
                return PipelineResult::failed({.code = "bloom.preview.held-failure",
                                               .severity = runtime::DiagnosticSeverity::Error,
                                               .summary = "Held failure",
                                               .detail = {},
                                               .suggestedAction = {}});
            });
        expectations.expect(waitUntil([&] { return isReady(controller); }),
                            "late accounting starts ready");
        controller.beginDroppedFrameCounting();
        controller.setPlaybackActive(true);
        expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(1)),
                            "late failure target changes");
        controller.presentPlaybackFrame(1ns);
        expectations.expect(waitUntil([&] { return held.entered(); }),
                            "late failure is held past deadline");
        held.release();
        const auto outcome =
            unsupported ? ui::PreviewActivity::Unsupported : ui::PreviewActivity::Failed;
        expectations.expect(waitUntil([&] { return controller.state().activity == outcome; }) &&
                                controller.droppedFrameCount() == 1,
                            "one expired latest failure or Unsupported records exactly one drop");
        reachQuiescence(controller, bridge, scheduler, expectations);
    }
}

void testLatePlaybackPurgeObserverRejectsOldDelivery(Expectations& expectations) {
    using namespace bloom;
    auto project = makeTestProject("Late delivery purge barrier");
    const auto composition = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, composition);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler);
    PipelineFixture fixture;
    WorkerGate held;
    std::atomic<int> invocations = 0;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&held, &invocations, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot, const runtime::PreviewRequestIdentity& identity,
            const std::size_t bytes,
            const std::vector<runtime::SnapshotParameterOverride>& overrides,
            runtime::TaskContext& context) {
            if (invocations.fetch_add(1) == 1)
                held.enterAndWait();
            return pipeline(snapshot, identity, bytes, overrides, context);
        });
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "purge barrier starts ready");
    controller.beginDroppedFrameCounting();
    controller.setPlaybackActive(true);
    expectations.expect(session.setCurrentTime(core::RationalTime::fromInteger(1)),
                        "purge barrier target changes");
    controller.presentPlaybackFrame(1ns);
    expectations.expect(waitUntil([&] { return held.entered(); }),
                        "old successful delivery is held before expiry accounting");
    bool purged = false;
    const auto connection = QObject::connect(
        &controller, &ui::CompositionPreviewController::droppedFrameCountChanged, &controller, [&] {
            if (!purged) {
                purged = true;
                controller.purgePreviewCache();
            }
        });
    held.release();
    expectations.expect(waitUntil([&] { return purged; }),
                        "lateness signal synchronously purges old delivery domain");
    const auto key = controller.cacheKeyForTime(core::RationalTime::fromInteger(1));
    expectations.expect(
        key && !controller.frameCache().contains(*key) && controller.state().frame &&
            controller.state().frame->desiredIdentity().time == core::RationalTime::fromInteger(0),
        "old completion cannot refill purged cache or replace picture after observer mutation");
    QObject::disconnect(connection);
    reachQuiescence(controller, bridge, scheduler, expectations);
}
