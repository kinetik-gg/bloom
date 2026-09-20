// PERF-RAM-2: the RAM preview fill's TWO-deep pipeline -- behavior.
//
// The contract under test is throughput without losing correctness: a RAM preview keeps the next
// frame's CPU preparation in flight while the previous frame is still being prepared/displayed,
// bounded at exactly two frames, and preserves the accepted intelligent-interval invalidation.
// Everything here is driven offscreen against real schedulers and real evaluation; the barrier
// tests prove ordering (preparation 2 starts before preparation 1 finishes) rather than measuring
// speed, and the out-of-order test proves frame 2's earlier completion cannot hide frame 1.
//
// The real GpuPreviewDisplayService submission seam lives in ram_preview_service_pipeline_tests.cpp
// so this unit stays focused on the controller's own behavior.
#include "ram_preview_pipeline_test_support.hpp"

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/ram_preview_pipeline.hpp>

#include <QApplication>
#include <QObject>

#include <algorithm>

namespace {

using namespace bloom;
using namespace bloom::ui::ram_pipeline_test;
using namespace std::chrono_literals;

// The helper itself: a hard bound of two, identity-aware duplicate detection, out-of-order
// collection in submission order, and cancel-all.
void testPipelineHelperBoundAndCancellation(Expectations& expectations) {
    runtime::TaskScheduler scheduler(twoWorkerConfig());
    ui::RamPreviewPipeline pipeline;
    auto gate = std::make_shared<std::atomic<bool>>(false);

    const auto submit = [&](const std::uint64_t frameIndex) {
        runtime::TaskRequest request(
            "helper frame",
            {.kind = runtime::TaskOwnerKind::Composition, .id = runtime::TaskOwnerId::fromRaw(1)},
            runtime::TaskPriority::Foreground);
        auto submission = scheduler.submit<ui::PreviewPreparationResultHandle>(
            std::move(request), [gate, frameIndex](runtime::TaskContext& context) {
                static_cast<void>(context);
                if (frameIndex == 0) {
                    while (!gate->load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                }
                return runtime::TaskResult<ui::PreviewPreparationResultHandle>::cancelled();
            });
        expectations.expect(submission.accepted(), "the helper fixture task is admitted");
        runtime::PreviewRequestIdentity identity;
        identity.time = timeAt(static_cast<std::int64_t>(frameIndex), 25);
        identity.requestGeneration = frameIndex + 1;
        pipeline.add(ui::RamPreviewPipeline::InFlight{.frameIndex = frameIndex,
                                                      .identity = std::move(identity),
                                                      .handle = std::move(submission.handle)});
    };

    submit(0);
    submit(1);
    expectations.expect(pipeline.inFlightCount() == 2 && !pipeline.hasCapacity() &&
                            pipeline.peakInFlight() == 2,
                        "the helper never holds more than two frames in flight");
    runtime::PreviewRequestIdentity time0;
    time0.time = timeAt(0, 25);
    time0.requestGeneration = 99;
    const auto key0 = ui::PreviewFrameCacheKey::forIdentity(time0);
    expectations.expect(pipeline.isInFlight(key0),
                        "the helper recognizes a frame identity already in flight");

    // Frame 1 completes while frame 0 is still blocked: it must be collectible alone, and the
    // held frame must be collected afterwards. Since both slot identities are distinct, an
    // out-of-order completion cannot cancel or hide its sibling.
    auto first = std::vector<ui::RamPreviewPipeline::Ready>{};
    expectations.expect(waitUntil([&] {
                            first = pipeline.takeReady();
                            return !first.empty();
                        }) &&
                            first.size() == 1 && first.front().frameIndex == 1,
                        "the unblocked frame is collectible while its sibling is held");
    gate->store(true, std::memory_order_release);
    auto second = std::vector<ui::RamPreviewPipeline::Ready>{};
    expectations.expect(waitUntil([&] {
                            second = pipeline.takeReady();
                            return !second.empty();
                        }) &&
                            second.size() == 1 && second.front().frameIndex == 0,
                        "the held frame is collected last, in submission order");
    expectations.expect(pipeline.empty(), "collecting every frame empties the helper");

    // Cancel-all detaches every held frame.
    submit(2);
    submit(3);
    expectations.expect(pipeline.inFlightCount() == 2, "two more frames are in flight");
    pipeline.cancelAllAndDetach();
    expectations.expect(pipeline.empty() && pipeline.inFlightCount() == 0,
                        "cancel-all detaches every in-flight frame");
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                        "the helper fixture quiesces after cancellation");
}

// The barrier proof: with two workers, the second RAM preparation starts while the first is still
// inside its preparation, and the bound holds at two.
void testControllerOverlapsTwoPreparations(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM Overlap", timeAt(3, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the foreground frame settles");
    fixture.frameCache->clear();

    const int base = fixture.coordinator.calls();
    fixture.coordinator.armArrivalBarrier(base, base + 1);

    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge,
                                 fixture.pipelineFixture.preparation(fixture.coordinator));
    ram.start();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) &&
                            ram.cachedFrameCount() == 3 && ram.totalFrameCount() == 3,
                        "the overlapping run caches the whole range");
    expectations.expect(fixture.coordinator.peak() >= 2,
                        "preparation 2 started while preparation 1 was still running");
    expectations.expect(ram.peakInFlightFrames() == ui::RamPreviewPipeline::kMaxInFlight,
                        "the controller reached exactly the two-frame bound, never more");
    for (std::int64_t frame = 0; frame < 3; ++frame) {
        const auto key = fixture.controller.cacheKeyForTime(timeAt(frame, 25));
        expectations.expect(key && fixture.frameCache->contains(*key),
                            "every frame of the overlapping range is cached");
    }
    finishFixture(fixture, expectations);
}

// Out-of-order: frame 2 finishes before frame 1, and both are still validated and cached in range
// order, so the early frame 2 cannot advance progress past an unfinished frame 1.
void testControllerCachesOutOfOrderResults(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM Out Of Order", timeAt(3, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the foreground frame settles");
    fixture.frameCache->clear();

    const int base = fixture.coordinator.calls();
    // Preparation of the FIRST submitted frame (ordinal base) waits until the SECOND (base + 1)
    // has fully finished.
    fixture.coordinator.armFinishBarrier(base, base + 1);

    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge,
                                 fixture.pipelineFixture.preparation(fixture.coordinator));
    ram.start();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) &&
                            ram.cachedFrameCount() == 3 && ram.totalFrameCount() == 3,
                        "the out-of-order run still caches the whole range exactly once");
    for (std::int64_t frame = 0; frame < 3; ++frame) {
        const auto key = fixture.controller.cacheKeyForTime(timeAt(frame, 25));
        expectations.expect(key && fixture.frameCache->contains(*key),
                            "the later-completed frame did not displace the earlier frame's slot");
    }
    expectations.expect(fixture.coordinator.peak() <= 2,
                        "out-of-order completion never exceeds the two-frame bound");
    finishFixture(fixture, expectations);
}

// cancel() and beginShutdown() both detach BOTH in-flight frames rather than only the newest.
void testControllerCancelAndShutdownDetachBoth(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM Cancel Both", timeAt(4, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the foreground frame settles");
    fixture.frameCache->clear();

    const int base = fixture.coordinator.calls();
    fixture.coordinator.hold(base);
    fixture.coordinator.hold(base + 1);
    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge,
                                 fixture.pipelineFixture.preparation(fixture.coordinator));
    ram.start();
    expectations.expect(waitUntil([&] { return fixture.coordinator.heldArrived(2); }) &&
                            ram.peakInFlightFrames() == 2,
                        "both frames are preparing before cancellation");
    const int callsBeforeCancel = fixture.coordinator.calls();
    ram.cancel();
    expectations.expect(!ram.isCaching(), "cancel ends the run immediately");
    fixture.coordinator.releaseHeld();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "both cancelled frames reach terminal");
    expectations.expect(fixture.coordinator.calls() == callsBeforeCancel,
                        "cancel did not start a replacement frame");

    // beginShutdown() on a fresh controller detaches both frames too.
    fixture.coordinator.resetForNextRun();
    fixture.frameCache->clear();
    fixture.coordinator.hold(0);
    fixture.coordinator.hold(1);
    ui::RamPreviewController shutdownRam(fixture.session, fixture.controller, fixture.scheduler,
                                         fixture.bridge,
                                         fixture.pipelineFixture.preparation(fixture.coordinator));
    shutdownRam.start();
    expectations.expect(waitUntil([&] { return fixture.coordinator.heldArrived(2); }),
                        "both frames are preparing before shutdown");
    shutdownRam.beginShutdown();
    expectations.expect(!shutdownRam.isCaching() && shutdownRam.isShuttingDown(),
                        "beginShutdown detaches both frames and ends the run");
    fixture.coordinator.releaseHeld();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "both shutdown-detached frames reach terminal");
    finishFixture(fixture, expectations);
}

// A work-area shrink while both frames are preparing adapts the run: out-of-range completions are
// refused and uncounted, and only the new range is filled.
void testControllerShrinkAdaptsBothInflight(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM Shrink Both", timeAt(7, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the foreground frame settles");
    fixture.frameCache->clear();

    const int base = fixture.coordinator.calls();
    fixture.coordinator.hold(base);
    fixture.coordinator.hold(base + 1);
    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge,
                                 fixture.pipelineFixture.preparation(fixture.coordinator));
    ram.start();
    expectations.expect(waitUntil([&] { return fixture.coordinator.heldArrived(2); }),
                        "both frames of the wide range are preparing");

    commands::Transaction range("Work area", fixture.session.snapshot().revision());
    range.emplace<commands::SetWorkArea>(fixture.session.compositionId(), timeAt(2, 25),
                                         timeAt(4, 25));
    expectations.expect(fixture.session.executeTransaction(std::move(range)).changed(),
                        "the work area shrinks while both frames are in flight");
    fixture.coordinator.releaseHeld();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) &&
                            ram.cachedFrameCount() == 2 && ram.totalFrameCount() == 2,
                        "the run adapts to the shrunken two-frame range");
    for (std::int64_t frame = 0; frame < 7; ++frame) {
        const auto key = fixture.controller.cacheKeyForTime(timeAt(frame, 25));
        const bool inRange = frame >= 2 && frame < 4;
        expectations.expect(key && fixture.frameCache->contains(*key) == inRange,
                            "only the in-range frames survive the mid-run shrink");
    }
    expectations.expect(fixture.coordinator.peak() <= 2,
                        "the shrink adaptation never exceeded the bound");
    finishFixture(fixture, expectations);
}

// A work-area expansion while both frames are preparing fills only the newly entered frames.
void testControllerExpandAdaptsBothInflight(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM Expand Both", timeAt(6, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the foreground frame settles");

    commands::Transaction narrow("Work area", fixture.session.snapshot().revision());
    narrow.emplace<commands::SetWorkArea>(fixture.session.compositionId(), timeAt(0),
                                          timeAt(2, 25));
    expectations.expect(fixture.session.executeTransaction(std::move(narrow)).changed(),
                        "the work area starts narrow");
    fixture.frameCache->clear();

    const int base = fixture.coordinator.calls();
    fixture.coordinator.hold(base);
    fixture.coordinator.hold(base + 1);
    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge,
                                 fixture.pipelineFixture.preparation(fixture.coordinator));
    ram.start();
    expectations.expect(waitUntil([&] { return fixture.coordinator.heldArrived(2); }),
                        "both frames of the narrow range are preparing");

    commands::Transaction widen("Work area", fixture.session.snapshot().revision());
    widen.emplace<commands::SetWorkArea>(fixture.session.compositionId(), timeAt(0), timeAt(6, 25));
    expectations.expect(fixture.session.executeTransaction(std::move(widen)).changed(),
                        "the work area expands while both frames are in flight");
    fixture.coordinator.releaseHeld();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) &&
                            ram.cachedFrameCount() == 6 && ram.totalFrameCount() == 6,
                        "the run adapts to the expanded six-frame range");
    for (std::int64_t frame = 0; frame < 6; ++frame) {
        const auto key = fixture.controller.cacheKeyForTime(timeAt(frame, 25));
        expectations.expect(key && fixture.frameCache->contains(*key),
                            "every frame of the expanded range is cached");
    }
    finishFixture(fixture, expectations);
}

// A finite clip-range edit invalidates exactly the frames whose time changed: the unaffected half
// of the in-flight pair is retained, and only the changed interval is re-prepared.
void testControllerFiniteEditKeepsRetainedHalf(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM Finite Edit Both", timeAt(7, 25)));
    expectations.expect(animateSolidLayer(fixture.session),
                        "the finite-edit fixture is animated across its range");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the foreground frame settles");
    const auto layerId = fixture.session.composition()->graph().layerOutputs().front().layerId;
    fixture.frameCache->clear();

    // Let frames 0 and 1 land, then hold the third and fourth preparations (frames 2 and 3). The
    // trim below keeps frame 2 unchanged and re-derives frame 3.
    const int base = fixture.coordinator.calls();
    fixture.coordinator.hold(base + 2);
    fixture.coordinator.hold(base + 3);
    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge,
                                 fixture.pipelineFixture.preparation(fixture.coordinator));
    ram.start();
    expectations.expect(waitUntil([&] { return fixture.coordinator.heldArrived(2); }),
                        "frames two and three are preparing under the old provenance");
    const int callsBeforeTrim = fixture.coordinator.calls();

    commands::Transaction trim("Trim", fixture.session.snapshot().revision());
    trim.emplace<commands::SetLayerRange>(fixture.session.compositionId(), layerId,
                                          core::RationalTime{}, timeAt(3, 25));
    expectations.expect(fixture.session.executeTransaction(std::move(trim)).changed(),
                        "the finite clip-range edit publishes");
    fixture.coordinator.releaseHeld();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) &&
                            ram.cachedFrameCount() == 7 && ram.totalFrameCount() == 7,
                        "the finite edit rescan completes the whole range");
    expectations.expect(fixture.coordinator.calls() == callsBeforeTrim + 4,
                        "only the four invalidated frames were re-prepared");
    for (std::int64_t frame = 0; frame < 7; ++frame) {
        const auto key = fixture.controller.cacheKeyForTime(timeAt(frame, 25));
        expectations.expect(key && fixture.frameCache->contains(*key),
                            "every frame resolves to its current per-time evaluation snapshot");
    }
    finishFixture(fixture, expectations);
}

// The memory-budget stop is bounded: the first eviction ends the run at the prefix that fits, the
// sibling already in flight is detached rather than allowed to churn the prefix, and the pipeline
// never oversubscribes.
void testControllerBudgetStopsBounded(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM Budget Both", timeAt(24, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the foreground frame settles");
    const auto frame = fixture.controller.state().frame;
    expectations.expect(frame != nullptr, "the budget fixture has a frame to measure");
    if (frame == nullptr) {
        finishFixture(fixture, expectations);
        return;
    }
    fixture.frameCache->setByteBudget(ui::PreviewFrameCache::frameByteCost(*frame) * 3);

    const int base = fixture.coordinator.calls();
    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge,
                                 fixture.pipelineFixture.preparation(fixture.coordinator));
    bool finishedCompleted = false;
    QObject::connect(&ram, &ui::RamPreviewController::cachingFinished, &ram,
                     [&finishedCompleted](const bool completed) { finishedCompleted = completed; });
    ram.start();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }),
                        "the budget-limited run ends on its own");
    expectations.expect(ram.cachedFrameCount() == 4 && ram.totalFrameCount() == 24,
                        "the run keeps the prefix that fits the budget and stops there");
    expectations.expect(fixture.frameCache->size() == 3,
                        "the cache holds exactly what its budget allows");
    expectations.expect(finishedCompleted, "a budget-limited run still finishes");
    expectations.expect(ram.peakInFlightFrames() <= ui::RamPreviewPipeline::kMaxInFlight,
                        "the budget stop never overscheduled past the two-frame bound");
    expectations.expect(fixture.coordinator.calls() == base + 4,
                        "at most the four frames that fit were ever prepared");
    finishFixture(fixture, expectations);
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    try {
        testPipelineHelperBoundAndCancellation(expectations);
        testControllerOverlapsTwoPreparations(expectations);
        testControllerCachesOutOfOrderResults(expectations);
        testControllerCancelAndShutdownDetachBoth(expectations);
        testControllerShrinkAdaptsBothInflight(expectations);
        testControllerExpandAdaptsBothInflight(expectations);
        testControllerFiniteEditKeepsRetainedHalf(expectations);
        testControllerBudgetStopsBounded(expectations);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    return expectations.failures() == 0 ? 0 : 1;
}
