// A half-cached range with a deterministic held producer proves the accepted one-active/newest
// policy: cached pixels remain immediate and cancel older producers, whose terminal still gates
// admission; completion dispatches the newest cold request without a Jobs snapshot poll.
void testHalfCachedPlaybackAdmitsIdleMissesAndBackpressures(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Half cached playback", time(8, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }), "initial ready");
    // Cache the even frames 0,2,4,6; the odd frames stay cold.
    for (const auto index : {2, 4, 6, 0}) {
        (void)fixture.session.setCurrentTime(time(index, 25));
        expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                            "seed cached frame");
    }
    // The stale slow estimate the accepted policy no longer uses to refuse an idle admission.
    fixture.controller.recordPreparationDuration(
        fixture.controller.state().frame->desiredIdentity(), 1h);
    // A one-hour tick keeps the playback deadline far away, so completion is decided by the worker
    // gate rather than by a wall-clock race with a fast fixture evaluation.
    auto now = std::chrono::steady_clock::now();
    ui::PlaybackController playback(fixture.session, fixture.controller, [&] { return now; }, 1h);
    playback.play();
    const auto before = fixture.preparationCount.load();

    // Tick 1 -> frame 1 (cold). The controller is idle, so it must submit despite the slow
    // estimate.
    fixture.gateAtCall = before; // the next preparation blocks, holding the one active request.
    now += 40ms;
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(1, 25), "the clock advances");
    expectations.expect(fixture.controller.state().activity == ui::PreviewActivity::Rendering,
                        "an idle cold miss is submitted even with a slow stale estimate");
    expectations.expect(fixture.controller.droppedFrameCount() == 0,
                        "the admitted idle miss is not a drop");
    expectations.expect(fixture.controller.state().frame != nullptr &&
                            fixture.controller.state().frame->desiredIdentity().time == time(0, 25),
                        "the previous picture remains until the admitted frame is ready");
    expectations.expect(waitUntil([&] { return fixture.gate.entered(); }),
                        "the admitted evaluation is held in flight");
    expectations.expect(fixture.preparationCount.load() == before + 1,
                        "exactly one evaluation is in flight for the admitted idle miss");

    const auto heldTask = fixture.controller.state().taskId;

    // Tick 2 -> frame 2 (cached). A cached frame is published immediately; the single in-flight
    // evaluation is cancelled but retains its admission slot until terminal.
    now += 40ms;
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(2, 25), "the clock advances");
    expectations.expect(fixture.controller.state().activity == ui::PreviewActivity::Ready &&
                            fixture.controller.state().frame != nullptr &&
                            fixture.controller.state().frame->desiredIdentity().time == time(2, 25),
                        "a cached frame shows immediately while a slow evaluation is in flight");
    expectations.expect(
        fixture.preparationCount.load() == before + 1,
        "a cache hit starts no evaluation and does not duplicate the in-flight one");
    expectations.expect(
        fixture.controller.droppedFrameCount() == 0,
        "a cached hit cancels the producer without prematurely releasing its admission gate");

    // Tick 3 -> frame 3 (cold): retain the active producer and coalesce one newest request.
    now += 40ms;
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(3, 25), "the clock advances");
    expectations.expect(fixture.preparationCount.load() == before + 1,
                        "one active producer prevents a second evaluation until terminal");
    expectations.expect(
        fixture.controller.droppedFrameCount() == 1,
        "the older outstanding request is counted once while newest remains pending");
    expectations.expect(fixture.controller.state().frame != nullptr &&
                            fixture.controller.state().frame->desiredIdentity().time == time(2, 25),
                        "the busy miss retains the previous picture");

    std::vector<core::RationalTime> shown;
    QObject::connect(&fixture.controller, &ui::CompositionPreviewController::stateChanged,
                     &fixture.controller, [&] {
                         if (fixture.controller.state().frame)
                             shown.push_back(
                                 fixture.controller.state().frame->desiredIdentity().time);
                     });
    fixture.gate.release();
    expectations.expect(
        waitUntil([&] {
            return fixture.controller.backgroundWorkAllowed() && isReady(fixture.controller) &&
                   fixture.controller.state().frame->desiredIdentity().time == time(3, 25);
        }),
        "completion refills newest frame three after cancelled admission reaches terminal");
    const auto oldKey = fixture.controller.cacheKeyForTime(time(1, 25));
    const auto heldSnapshot = heldTask ? fixture.scheduler.snapshot(*heldTask) : std::nullopt;
    expectations.expect(
        heldSnapshot && heldSnapshot->state == runtime::TaskState::Cancelled && oldKey &&
            !fixture.frameCache->contains(*oldKey) &&
            std::ranges::find(shown, time(1, 25)) == shown.end() &&
            fixture.preparationCount.load() == before + 2 &&
            fixture.controller.droppedFrameCount() == 1,
        "cancelled producer cannot cache or rewind cached pixels, and newest starts exactly once");

    // Tick 4 -> frame 4 (cached): immediate, no evaluation, no new drops.
    now += 40ms;
    playback.tick();
    expectations.expect(fixture.controller.state().frame != nullptr &&
                            fixture.controller.state().frame->desiredIdentity().time ==
                                time(4, 25) &&
                            fixture.preparationCount.load() == before + 2 &&
                            fixture.controller.droppedFrameCount() == 1,
                        "a cached frame after the gate clears is immediate and uncounted");

    // Tick 5 -> frame 5 (cold) from idle: the transport progresses and the frame fills the cache.
    now += 40ms;
    playback.tick();
    expectations.expect(fixture.controller.state().activity == ui::PreviewActivity::Rendering,
                        "a cold miss after backpressure clears submits again");
    expectations.expect(waitUntil([&] { return fixture.preparationCount.load() == before + 3; }),
                        "exactly one evaluation starts for the progressed cold frame");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the progressed cold frame is displayed");
    expectations.expect(fixture.controller.state().frame != nullptr &&
                            fixture.controller.state().frame->desiredIdentity().time ==
                                time(5, 25) &&
                            fixture.controller.droppedFrameCount() == 1,
                        "the displayed frame advances without extra drops");
    const auto filled = fixture.controller.cacheKeyForTime(time(5, 25));
    expectations.expect(filled.has_value() && fixture.frameCache->contains(*filled),
                        "the admitted slow frame fills the RAM cache");

    // Tick 6 -> frame 6 (cached): a later pass is a cached hit with no evaluation.
    now += 40ms;
    playback.tick();
    expectations.expect(fixture.controller.state().frame != nullptr &&
                            fixture.controller.state().frame->desiredIdentity().time ==
                                time(6, 25) &&
                            fixture.preparationCount.load() == before + 3 &&
                            fixture.controller.droppedFrameCount() == 1,
                        "the cached second pass adds no evaluation and no drops");

    playback.pause();
    expectations.expect(fixture.controller.droppedFrameCount() == 1, "pause retains the run total");
    finishFixture(fixture, expectations);
}
