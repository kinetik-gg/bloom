// A half-cached range (even frames cached, odd cold) with a measured preparation far slower than
// the tick. The accepted admission policy never starves an IDLE transport: a cold miss submits even
// when the stale estimate cannot fit the tick, so a slow range still progresses and fills the RAM
// cache. A miss that arrives while a request is already in flight is never queued behind it -- it
// is skipped and counted exactly once -- and a cache hit is published immediately without
// evaluation. A deterministic worker gate holds the single in-flight evaluation, so every drop
// count and shown frame is exact rather than raced against a fast fixture worker.
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

    // Tick 2 -> frame 2 (cached). A cached frame is published immediately; the single in-flight
    // evaluation is superseded, never duplicated.
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
    expectations.expect(fixture.controller.droppedFrameCount() == 1,
                        "superseding the outstanding playback request counts exactly one drop");

    // Tick 3 -> frame 3 (cold) while the superseded handle is still the active admission gate.
    // One-active/one-newest: no second evaluation starts and the busy miss is skipped once.
    now += 40ms;
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(3, 25), "the clock advances");
    expectations.expect(fixture.preparationCount.load() == before + 1,
                        "one active request is never queued behind; no second evaluation starts");
    expectations.expect(fixture.controller.droppedFrameCount() == 2,
                        "the busy cold miss is counted exactly once");
    expectations.expect(fixture.controller.state().frame != nullptr &&
                            fixture.controller.state().frame->desiredIdentity().time == time(2, 25),
                        "the busy miss retains the previous picture");

    // Release the held evaluation and let its terminal result clear the active admission gate.
    // backgroundWorkAllowed() is true exactly when no active/pending request remains, so it is the
    // deterministic signal that the superseded handle has been observed and released.
    fixture.gate.release();
    expectations.expect(waitUntil([&] {
                            return fixture.scheduler.isQuiescent() &&
                                   fixture.controller.backgroundWorkAllowed();
                        }),
                        "the superseded request terminates and clears the active gate");

    // Tick 4 -> frame 4 (cached): immediate, no evaluation, no new drops.
    now += 40ms;
    playback.tick();
    expectations.expect(fixture.controller.state().frame != nullptr &&
                            fixture.controller.state().frame->desiredIdentity().time ==
                                time(4, 25) &&
                            fixture.preparationCount.load() == before + 1 &&
                            fixture.controller.droppedFrameCount() == 2,
                        "a cached frame after the gate clears is immediate and uncounted");

    // Tick 5 -> frame 5 (cold) from idle: the transport progresses and the frame fills the cache.
    now += 40ms;
    playback.tick();
    expectations.expect(fixture.controller.state().activity == ui::PreviewActivity::Rendering,
                        "a cold miss after backpressure clears submits again");
    expectations.expect(waitUntil([&] { return fixture.preparationCount.load() == before + 2; }),
                        "exactly one evaluation starts for the progressed cold frame");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the progressed cold frame is displayed");
    expectations.expect(fixture.controller.state().frame != nullptr &&
                            fixture.controller.state().frame->desiredIdentity().time ==
                                time(5, 25) &&
                            fixture.controller.droppedFrameCount() == 2,
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
                            fixture.preparationCount.load() == before + 2 &&
                            fixture.controller.droppedFrameCount() == 2,
                        "the cached second pass adds no evaluation and no drops");

    playback.pause();
    expectations.expect(fixture.controller.droppedFrameCount() == 2, "pause retains the run total");
    finishFixture(fixture, expectations);
}
