// G3A: observable retention, admission failures and completion callback boundaries.
void testRamRetentionAdmissionAndCompletion(Expectations& expectations) {
    for (const int capacity : {0, 1}) {
        SessionFixture fixture(makeTestProject("RAM admission", time(4, 25)));
        expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                            "admission ready");
        const auto cost = ui::PreviewFrameCache::frameByteCost(*fixture.controller.state().frame);
        fixture.frameCache->clear();
        fixture.frameCache->setByteBudget(cost * static_cast<std::size_t>(capacity));
        ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                     fixture.bridge, fixture.countingPipeline());
        ram.start();
        expectations.expect(waitUntil([&] { return !ram.isCaching(); }), "bounded admission ends");
        expectations.expect(
            ram.lastCompletion() == ui::RamPreviewController::Completion::PartialBudget &&
                ram.cachedFrameCount() == static_cast<std::uint64_t>(capacity) &&
                ram.preparedFrameCount() >= 1 && !ram.canAutoPlay(),
            "zero/one capacity reports live retention and declines partial autoplay");
        finishFixture(fixture, expectations);
    }
    SessionFixture fixture(makeTestProject("RAM allocation", time(4, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }), "allocation ready");
    fixture.frameCache->failNextInsertAllocationForTesting(
        ui::PreviewFrameCache::InsertAllocation::Product);
    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge, fixture.countingPipeline());
    ram.start();
    expectations.expect(
        waitUntil([&] { return !ram.isCaching(); }) &&
            ram.lastCompletion() == ui::RamPreviewController::Completion::Failed &&
            ram.cachedFrameCount() == 1 && ram.preparedFrameCount() == 1,
        "failed retention leaves preexisting coverage but reports preparation separately");
    finishFixture(fixture, expectations);
}

void testRamForeignEvictionAndLiveHoles(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM foreign policy", time(2, 25)));
    fixture.controller.setResolutionPolicy(runtime::PreviewResolutionPolicy::Full);
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "Full frame zero ready");
    const auto first = fixture.controller.state().frame;
    expectations.expect(fixture.session.setCurrentTime(time(1, 25)), "advance foreign frame");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "Full frame one ready");
    const auto second = fixture.controller.state().frame;
    fixture.controller.setResolutionPolicy(runtime::PreviewResolutionPolicy::Auto);
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "Auto policy ready");
    fixture.frameCache->clear();
    fixture.frameCache->setByteBudget(ui::PreviewFrameCache::frameByteCost(*first) * 2);
    fixture.frameCache->insert(first);
    fixture.frameCache->insert(second);
    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge, fixture.countingPipeline());
    ram.start();
    expectations.expect(
        waitUntil([&] { return !ram.isCaching(); }) && ram.canAutoPlay() &&
            ram.preparedFrameCount() == 2 && ram.cachedFrameCount() == 2,
        "evicting unrelated policy entries does not prematurely stop a fitting range");
    const auto prepared = ram.preparedFrameCount();
    fixture.frameCache->clear();
    expectations.expect(waitUntil([&] { return ram.cachedFrameCount() == 0; }) &&
                            !ram.playableSpan() && !ram.canAutoPlay() &&
                            ram.preparedFrameCount() == prepared &&
                            ram.lastCompletion() == ui::RamPreviewController::Completion::Complete,
                        "later purge changes live coverage, not historical preparation/completion");
    finishFixture(fixture, expectations);

    SessionFixture holes(makeTestProject("RAM holes", time(4, 25)));
    expectations.expect(waitUntil([&] { return isReady(holes.controller); }), "holes ready");
    ui::RamPreviewController sparse(holes.session, holes.controller, holes.scheduler, holes.bridge,
                                    holes.countingPipeline());
    sparse.start();
    expectations.expect(waitUntil([&] { return !sparse.isCaching(); }), "holes fill complete");
    const auto probe = holes.controller.cacheKeyForTime(time(0));
    expectations.expect(probe.has_value(), "the holes fixture has a valid coverage key");
    if (!probe.has_value()) {
        finishFixture(holes, expectations);
        return;
    }
    holes.frameCache->setRetentionSnapshots(
        {{probe->projectId, probe->compositionId, probe->sourceRevision, time(1, 25), time(2, 25)},
         {probe->projectId, probe->compositionId, probe->sourceRevision, time(3, 25),
          time(4, 25)}});
    const bool retained = waitUntil([&] { return sparse.cachedFrameCount() == 2; });
    const auto span = sparse.playableSpan();
    expectations.expect(retained && span.has_value() &&
                            sparse.retainedFrameIndices() == std::vector<std::uint64_t>{1, 3} &&
                            span->start == time(1, 25) && span->end == time(2, 25) &&
                            span->frameCount == 1 && !sparse.canAutoPlay(),
                        "holes produce exact live set and earliest longest half-open span");
    finishFixture(holes, expectations);
}

void testRamOffGridAndReentrantFinish(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM off-grid", time(2, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }), "off-grid ready");
    fixture.frameCache->clear();
    expectations.expect(fixture.session.setCurrentTime(time(1, 1000)), "seek exact subframe");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "subframe prepared");
    fixture.gateAtCall = fixture.preparationCount.load();
    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge, fixture.countingPipeline());
    ram.start();
    expectations.expect(waitUntil([&] { return fixture.gate.entered(); }) &&
                            ram.cachedFrameCount() == 0,
                        "a retained subframe certifies no exact grid sample");
    fixture.gate.release();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) && ram.cachedFrameCount() == 2,
                        "the exact grid is prepared separately");
    bool restarted = false;
    int completions = 0;
    QObject::connect(&ram, &ui::RamPreviewController::stateChanged, &ram, [&] {
        if (!ram.isCaching() &&
            ram.lastCompletion() == ui::RamPreviewController::Completion::Complete && !restarted) {
            restarted = true;
            ram.start();
        }
    });
    QObject::connect(&ram, &ui::RamPreviewController::cachingFinished, &ram,
                     [&](ui::RamPreviewController::Completion) { ++completions; });
    ram.start();
    expectations.expect(restarted && completions == 1 && ram.preparedFrameCount() == 0,
                        "reentrant warm start suppresses the superseded run completion");
    QObject::connect(&ram, &ui::RamPreviewController::stateChanged, &ram, [&] {
        if (!ram.isCaching())
            fixture.frameCache->clear();
    });
    ram.start();
    expectations.expect(!ram.canAutoPlay(),
                        "synchronous completion purge cannot autoplay stale coverage");
    finishFixture(fixture, expectations);
}
