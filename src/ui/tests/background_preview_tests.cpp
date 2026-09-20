#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/background_preview_controller.hpp>
#include <bloom/ui/composition_plan_cache.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QRectF>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace bloom;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 20'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::invoke(predicate)) {
            return true;
        }
        std::this_thread::yield();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return std::invoke(predicate);
}

[[nodiscard]] runtime::TaskSchedulerConfig testSchedulerConfig() {
    return {.cpuWorkerCount = 1,
            .blockingIoWorkerCount = 1,
            .cpuQueueCapacity = 16,
            .blockingIoQueueCapacity = 8,
            .gpuPendingQueueCapacity = 8,
            .gpuAdmittedStateCapacity = 16,
            .gpuLiveContinuationCapacity = 8,
            .terminalHistoryCapacity = 32,
            .diagnosticsPerTask = 8,
            .groupRegistryCapacity = 8};
}

[[nodiscard]] core::RationalTime time(const std::int64_t numerator,
                                      const std::int64_t denominator = 1) {
    const auto value = core::RationalTime::create(numerator, denominator);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

// 25 fps, so one frame is exactly 40,000,000 ns and the manual clock below lands on frame
// boundaries exactly (the same reason playback_controller_tests.cpp chose it), and a small extent
// so twenty-four frames of real evaluation cost microseconds rather than seconds.
[[nodiscard]] document::CompositionFormat testFormat() {
    const auto rate = document::FrameRate::create(25, 1);
    if (!rate.has_value()) {
        std::abort();
    }
    const auto format =
        document::CompositionFormat::create(16, 12, core::PixelAspectRatio::square(), *rate);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
}

[[nodiscard]] document::NewProject makeTestProject(std::string projectName,
                                                   const core::RationalTime duration) {
    return document::makeNewProject(std::move(projectName), "Main", duration, testFormat());
}

// A rendezvous for pausing one preparation on the worker thread, so a cancellation or a progress
// readout can be asserted at an exact point instead of raced for.
class WorkerGate final {
  public:
    void enterAndWait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    [[nodiscard]] bool entered() const {
        std::lock_guard lock(mutex_);
        return entered_;
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

struct PipelineFixture final {
    runtime::NodeDefinitionRegistry definitions;
    runtime::SnapshotCompiler compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    ui::CompiledPlanCacheHandle planCache = std::make_shared<ui::CompiledPlanCache>();
    ui::PreviewPreparationFunction pipeline;

    PipelineFixture() : compiler(definitions) {
        if (!runtime::registerBuiltInNodeDefinitions(definitions)) {
            std::abort();
        }
        definitions.freeze();
        pipeline = ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                      qualifiedProcessorProvider, planCache);
    }
};

// The whole preview stack, with a preparation function that counts every call and can be paused.
struct SessionFixture final {
    document::Document document;
    commands::CommandStack commands;
    ui::CompositionSession session;
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge;
    PipelineFixture pipelineFixture;
    // A private, never-polled ledger keeps the preview budgets here a decision of the test rather
    // than of live host memory pressure: the process ledger can trim an explicit budget mid-test,
    // which then reads as cache churn the background controller never performed.
    runtime::MemoryBudgetLedger ledger{std::size_t{16} * 1024 * 1024 * 1024};
    ui::PreviewFrameCacheHandle frameCache =
        std::make_shared<ui::PreviewFrameCache>(ui::defaultPreviewFrameCacheByteBudget(), ledger);
    std::atomic<int> preparationCount = 0;
    // When set, the preparation whose ordinal (counting from zero) equals this one blocks in
    // `gate`.
    std::optional<int> gateAtCall;
    WorkerGate gate;
    // Declared last, and constructed in the initializer list, so the counting preparation function
    // it holds can capture `this` after every member it reads is already alive.
    ui::CompositionPreviewController controller;

    // The one preparation function BOTH the preview controller and the RAM preview controller use,
    // so every frame either of them renders is counted in the same number -- which is what lets a
    // test say "this evaluated nothing" and mean it.
    [[nodiscard]] ui::PreviewPreparationFunction countingPipeline() {
        return [this](const document::Snapshot& snapshot,
                      const runtime::PreviewRequestIdentity& desiredIdentity,
                      const std::size_t pixelStorageByteLimit,
                      const std::vector<runtime::SnapshotParameterOverride>& interactionOverride,
                      runtime::TaskContext& context) {
            const auto ordinal = preparationCount.fetch_add(1);
            if (gateAtCall.has_value() && ordinal == *gateAtCall) {
                gate.enterAndWait();
            }
            return pipelineFixture.pipeline(snapshot, desiredIdentity, pixelStorageByteLimit,
                                            interactionOverride, context);
        };
    }

    explicit SessionFixture(document::NewProject newProject)
        : document(std::move(newProject.project)), commands(document),
          session(document, commands, newProject.initialCompositionId),
          scheduler(testSchedulerConfig()), bridge(scheduler, nullptr, 1ms),
          controller(session, scheduler, bridge, countingPipeline(),
                     ui::CompositionPreviewSettings{}, frameCache) {}
};

void finishFixture(SessionFixture& fixture, Expectations& expectations) {
    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the RAM preview fixture reaches asynchronous scheduler quiescence");
}

[[nodiscard]] bool isReady(const ui::CompositionPreviewController& controller) {
    return controller.state().activity == ui::PreviewActivity::Ready;
}

void testWorkAreaBoundsBackground(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Background work area", time(7, 25)));
    commands::Transaction range("Work area", fixture.session.snapshot().revision());
    range.emplace<commands::SetWorkArea>(fixture.session.compositionId(), time(2, 25), time(5, 25));
    expectations.expect(fixture.session.executeTransaction(std::move(range)).changed(),
                        "work area accepted");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "foreground settled");
    fixture.frameCache->clear();
    ui::BackgroundPreviewController background(fixture.session, fixture.controller,
                                               fixture.scheduler, fixture.bridge,
                                               fixture.countingPipeline());
    background.fillNextFrame();
    expectations.expect(waitUntil([&] { return fixture.frameCache->size() == 3; }),
                        "only three work-area frames fill");
    for (int frame = 0; frame < 7; ++frame) {
        const auto key = fixture.controller.cacheKeyForTime(time(frame, 25));
        expectations.expect(key && fixture.frameCache->contains(*key) == (frame >= 2 && frame < 5),
                            "background cache respects both exclusive range edges");
    }
    background.beginShutdown();
    finishFixture(fixture, expectations);
}

// A background fill must submit the SAME ROI the cache key carries. Before the fix, fillNextFrame
// built the identity with no `.roi`, so when an ROI was active the completion-key comparison
// (cacheKeyForTime includes roi, forIdentity(activeIdentity) did not) never matched: the completed
// frame was silently discarded and never entered the cache.
void testBackgroundFillHonoursRegionOfInterest(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Background ROI", time(3, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "ROI fixture foreground settled");
    fixture.controller.setRegionOfInterest(QRectF(0.25, 0.25, 0.5, 0.5));
    expectations.expect(fixture.controller.regionOfInterest().has_value(),
                        "the controller exposes the ROI window");
    fixture.frameCache->clear();

    ui::BackgroundPreviewController background(fixture.session, fixture.controller,
                                               fixture.scheduler, fixture.bridge,
                                               fixture.countingPipeline());
    background.fillNextFrame();
    expectations.expect(waitUntil([&] { return fixture.frameCache->size() >= 1; }),
                        "a ROI background frame enters the cache");

    const auto roiKey = fixture.controller.cacheKeyForTime(time(0, 25));
    expectations.expect(roiKey.has_value() && roiKey->roi.has_value(),
                        "the cache key carries the ROI window");
    if (roiKey.has_value()) {
        expectations.expect(fixture.frameCache->contains(*roiKey),
                            "the completed ROI frame is cached under the ROI key");
        // The same time WITHOUT the ROI is a different key; its absence proves the cached entry was
        // the ROI frame, not a wasted full-frame submission that reused the plain key.
        auto plainKey = *roiKey;
        plainKey.roi = std::nullopt;
        expectations.expect(!fixture.frameCache->contains(plainKey),
                            "no full-frame entry is cached for the ROI time");
    }
    background.beginShutdown();
    finishFixture(fixture, expectations);
}

void testOutwardOrderBudgetAndRestart(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Speculation order", time(7, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "opening frame ready");
    (void)fixture.session.setCurrentTime(time(3, 25));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }), "playhead ready");
    fixture.frameCache->clear();
    std::mutex mutex;
    std::vector<core::RationalTime> order;
    auto prepare = [&](const document::Snapshot& snapshot,
                       const runtime::PreviewRequestIdentity& identity, std::size_t limit,
                       const std::vector<runtime::SnapshotParameterOverride>& override,
                       runtime::TaskContext& context) {
        {
            std::lock_guard lock(mutex);
            order.push_back(identity.time);
        }
        return fixture.pipelineFixture.pipeline(snapshot, identity, limit, override, context);
    };
    ui::BackgroundPreviewController background(fixture.session, fixture.controller,
                                               fixture.scheduler, fixture.bridge, prepare);
    background.fillNextFrame();
    expectations.expect(waitUntil([&] { return fixture.frameCache->size() == 7; }),
                        "idle renderer fills the whole bounded composition");
    {
        std::lock_guard lock(mutex);
        expectations.expect(order == std::vector{time(3, 25), time(4, 25), time(2, 25), time(5, 25),
                                                 time(1, 25), time(6, 25), time(0)},
                            "nearest first, alternating forward/backward from the playhead");
    }
    expectations.expect(fixture.session.currentTime() == time(3, 25),
                        "speculation never moves time");
    const auto bytes = ui::PreviewFrameCache::frameByteCost(*fixture.controller.state().frame);
    fixture.frameCache->clear();
    fixture.frameCache->setByteBudget(bytes * 3);
    background.restart();
    expectations.expect(waitUntil([&] { return fixture.frameCache->size() == 3; }), "budget fills");
    for (int i = 0; i < 10; ++i) {
        background.fillNextFrame();
    }
    for (const auto index : {2, 3, 4}) {
        const auto key = fixture.controller.cacheKeyForTime(time(index, 25));
        expectations.expect(key.has_value() && fixture.frameCache->contains(*key),
                            "budget retains the nearest three frames without churn");
    }
    fixture.controller.setResolutionPolicy(runtime::PreviewResolutionPolicy::Quarter);
    expectations.expect(waitUntil([&] {
                            const auto key = fixture.controller.cacheKeyForTime(time(0));
                            return key.has_value() && fixture.frameCache->contains(*key);
                        }),
                        "resolution change restarts the range at the new factor");
    expectations.expect(fixture.session.addSolidLayer(QStringLiteral("Edit"), {1, 0, 0, 1}),
                        "revision changes");
    expectations.expect(waitUntil([&] {
                            const auto key = fixture.controller.cacheKeyForTime(time(6, 25));
                            return key.has_value() && fixture.frameCache->contains(*key);
                        }),
                        "revision change restarts and fills the new revision");
    background.beginShutdown();
    finishFixture(fixture, expectations);
}

void testYieldsAndKeepsCancelledHandleUntilTerminal(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Speculation cancellation", time(7, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }), "opening ready");
    WorkerGate gate;
    std::atomic<bool> cancelled = false;
    auto prepare = [&](const document::Snapshot& snapshot,
                       const runtime::PreviewRequestIdentity& identity, std::size_t limit,
                       const std::vector<runtime::SnapshotParameterOverride>& override,
                       runtime::TaskContext& context) {
        gate.enterAndWait();
        cancelled = context.isCancellationRequested();
        return fixture.pipelineFixture.pipeline(snapshot, identity, limit, override, context);
    };
    ui::BackgroundPreviewController background(fixture.session, fixture.controller,
                                               fixture.scheduler, fixture.bridge, prepare);
    background.fillNextFrame();
    expectations.expect(waitUntil([&] { return gate.entered(); }), "background frame in flight");
    const auto snapshots = fixture.scheduler.snapshots();
    expectations.expect(
        std::ranges::any_of(snapshots,
                            [](const auto& task) {
                                return task.name == "Cache background preview frame" &&
                                       task.priority == runtime::TaskPriority::Background &&
                                       task.state == runtime::TaskState::Running;
                            }),
        "speculation uses Background priority");
    fixture.controller.beginInteractiveScrub();
    (void)fixture.session.setCurrentTime(time(5, 25));
    fixture.controller.notifyScrubEnded();
    background.restart();
    background.fillNextFrame();
    const auto waiting = fixture.scheduler.snapshots();
    expectations.expect(
        std::ranges::count_if(waiting,
                              [](const auto& task) {
                                  return task.name == "Cache background preview frame" &&
                                         (task.state == runtime::TaskState::Running ||
                                          task.state == runtime::TaskState::Queued);
                              }) == 1,
        "restart retains the cancelled one-in-flight gate");
    gate.release();
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }) && cancelled,
                        "interactive request cancels speculation and reaches ready first");
    const auto key = fixture.controller.cacheKeyForTime(time(1, 25));
    expectations.expect(key.has_value() && !fixture.frameCache->contains(*key),
                        "cancelled speculative result never enters cache");
    fixture.controller.beginInteractiveScrub();
    background.fillNextFrame();
    expectations.expect(fixture.frameCache->size() == 2, "no fill while scrub remains armed");
    background.beginShutdown();
    fixture.controller.notifyScrubEnded();
    finishFixture(fixture, expectations);
}

void testPointerPressCancelsBeforePreviewChanges(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Pointer yields speculation", time(4, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }), "opening ready");
    WorkerGate gate;
    std::atomic<bool> cancelled = false;
    auto prepare = [&](const document::Snapshot& snapshot,
                       const runtime::PreviewRequestIdentity& identity, std::size_t limit,
                       const std::vector<runtime::SnapshotParameterOverride>& override,
                       runtime::TaskContext& context) {
        gate.enterAndWait();
        cancelled = context.isCancellationRequested();
        return fixture.pipelineFixture.pipeline(snapshot, identity, limit, override, context);
    };
    ui::BackgroundPreviewController background(fixture.session, fixture.controller,
                                               fixture.scheduler, fixture.bridge, prepare);
    background.fillNextFrame();
    expectations.expect(waitUntil([&] { return gate.entered(); }), "speculation in flight");
    QWidget panel;
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(1, 1), QPointF(1, 1), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&panel, &press);
    gate.release();
    expectations.expect(waitUntil([&] { return cancelled.load(); }),
                        "any panel pointer press cancels speculation before session changes");
    background.beginShutdown();
    finishFixture(fixture, expectations);
}

#include "background_preview_playback.ipp"

void testVisibleAdmissionAndSupersession(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Visible playback admission", time(8, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }), "initial ready");
    // A generous deterministic deadline admits the measured small fixture, without relying on
    // the machine completing it inside a 16ms wall-clock race.
    auto now = std::chrono::steady_clock::now();
    ui::PlaybackController playback(fixture.session, fixture.controller, [&] { return now; }, 1h);
    playback.play();
    now += 40ms;
    playback.tick();
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "admitted miss is displayed");
    const auto taskId = fixture.controller.state().taskId;
    const auto task = taskId.has_value() ? fixture.scheduler.snapshot(*taskId) : std::nullopt;
    expectations.expect(task.has_value() && task->priority == runtime::TaskPriority::Visible,
                        "a predicted fast miss uses Visible, never Interactive");
    fixture.gateAtCall = fixture.preparationCount.load();
    now += 40ms;
    playback.tick();
    expectations.expect(waitUntil([&] { return fixture.gate.entered(); }),
                        "next evaluation held in flight");
    now += 40ms;
    playback.tick();
    expectations.expect(
        fixture.controller.droppedFrameCount() == 2,
        "next tick drops the unfinished request and skips its busy successor once each");
    fixture.gate.release();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "cancelled request terminates");
    expectations.expect(fixture.controller.state().frame != nullptr &&
                            fixture.controller.state().frame->desiredIdentity().time ==
                                time(1, 25) &&
                            fixture.controller.droppedFrameCount() == 2,
                        "late work neither replaces the displayed frame nor counts twice");
    playback.pause();
    finishFixture(fixture, expectations);
}

void testBackgroundFillsAheadWhilePlaying(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Ahead while playing", time(6, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }), "initial ready");
    (void)fixture.session.setCurrentTime(time(3, 25));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }), "anchor ready");
    fixture.frameCache->clear();
    std::mutex mutex;
    std::vector<core::RationalTime> order;
    auto prepare = [&](const document::Snapshot& snapshot,
                       const runtime::PreviewRequestIdentity& identity, std::size_t limit,
                       const std::vector<runtime::SnapshotParameterOverride>& override,
                       runtime::TaskContext& context) {
        {
            std::lock_guard lock(mutex);
            order.push_back(identity.time);
        }
        return fixture.pipelineFixture.pipeline(snapshot, identity, limit, override, context);
    };
    ui::BackgroundPreviewController background(fixture.session, fixture.controller,
                                               fixture.scheduler, fixture.bridge, prepare);
    fixture.controller.setPlaybackActive(true);
    background.fillNextFrame();
    expectations.expect(waitUntil([&] { return fixture.frameCache->size() == 6; }),
                        "background runs during playback");
    {
        std::lock_guard lock(mutex);
        expectations.expect(order == std::vector{time(3, 25), time(4, 25), time(5, 25), time(0),
                                                 time(1, 25), time(2, 25)},
                            "playing prioritizes forward frames and wraps before behind frames");
    }
    background.beginShutdown();
    fixture.controller.setPlaybackActive(false);
    finishFixture(fixture, expectations);
}

// LAYOUT-2: a layout-only edit does not restart the background pass, and it keeps filling under
// the retained evaluation revision, so the key it fills and the snapshot it captured cannot
// disagree. (Deliberate foreground priority still yields through the existing event filter.)
void testLayoutEditKeepsBackgroundOnRetainedRevision(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Background Layout Retain", time(7, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the foreground preview settles before background fills");
    fixture.frameCache->clear();
    const auto evalRevision = fixture.session.evaluationSnapshot().revision();
    const auto nodeId = fixture.session.composition()->graph().nodes().front().id;

    ui::BackgroundPreviewController background(fixture.session, fixture.controller,
                                               fixture.scheduler, fixture.bridge,
                                               fixture.countingPipeline());
    background.fillNextFrame();
    expectations.expect(waitUntil([&] { return fixture.frameCache->size() == 1; }),
                        "the background pass fills its first frame");

    commands::Transaction move("Move Nodes", fixture.session.snapshot().revision());
    move.emplace<commands::MoveNodes>(
        fixture.session.compositionId(),
        std::map<document::NodeId, document::Vec2d>{{nodeId, {6.0, 7.0}}});
    expectations.expect(fixture.session.executeNodeTransaction(std::move(move)).changed() &&
                            fixture.session.evaluationSnapshot().revision() == evalRevision &&
                            fixture.session.snapshot().revision() != evalRevision,
                        "the layout edit retains the evaluation revision");

    background.fillNextFrame();
    expectations.expect(waitUntil([&] { return fixture.frameCache->size() >= 2; }),
                        "the background pass keeps filling after the layout edit");
    for (std::int64_t frame = 0; frame < 7; ++frame) {
        const auto key = fixture.controller.cacheKeyForTime(time(frame, 25));
        if (key.has_value() && fixture.frameCache->contains(*key)) {
            expectations.expect(key->sourceRevision == evalRevision,
                                "a background frame is cached under the retained revision");
        }
    }

    background.beginShutdown();
    finishFixture(fixture, expectations);
}

// WORKAREA-1: background fill follows a range edit. Expanding submits only missing frames (cached
// in-range frames are skipped), and a fully cached shrink submits nothing.
void testBackgroundFillFollowsWorkArea(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Background Work Area", time(7, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the foreground preview settles");
    const auto setRange = [&](const core::RationalTime start, const core::RationalTime end) {
        commands::Transaction range("Set Work Area", fixture.session.snapshot().revision());
        range.emplace<commands::SetWorkArea>(fixture.session.compositionId(), start, end);
        return fixture.session.executeTransaction(std::move(range));
    };
    expectations.expect(setRange(time(0, 25), time(3, 25)).changed(), "the initial range is set");
    fixture.frameCache->clear();
    ui::BackgroundPreviewController background(fixture.session, fixture.controller,
                                               fixture.scheduler, fixture.bridge,
                                               fixture.countingPipeline());
    background.fillNextFrame();
    expectations.expect(waitUntil([&] { return fixture.frameCache->size() == 3; }),
                        "the initial range fills");
    const auto afterInitial = fixture.preparationCount.load();

    // Expand to [0,5): only frames 3 and 4 are missing.
    expectations.expect(setRange(time(0, 25), time(5, 25)).changed(), "the range expands");
    background.fillNextFrame();
    expectations.expect(waitUntil([&] { return fixture.frameCache->size() == 5; }),
                        "the expanded range fills");
    expectations.expect(fixture.preparationCount.load() == afterInitial + 2,
                        "expansion evaluates only the two entering frames");

    // Shrink to [1,3): frames 0 and 4 are pruned, 1 and 2 survive, and no fill is submitted.
    const auto beforeShrink = fixture.preparationCount.load();
    expectations.expect(setRange(time(1, 25), time(3, 25)).changed(), "the range shrinks");
    const auto key0 = fixture.controller.cacheKeyForTime(time(0, 25));
    const auto key1 = fixture.controller.cacheKeyForTime(time(1, 25));
    const auto key2 = fixture.controller.cacheKeyForTime(time(2, 25));
    const auto key4 = fixture.controller.cacheKeyForTime(time(4, 25));
    expectations.expect(key0 && key1 && key2 && key4 && !fixture.frameCache->contains(*key0) &&
                            !fixture.frameCache->contains(*key4) &&
                            fixture.frameCache->contains(*key1) &&
                            fixture.frameCache->contains(*key2) && fixture.frameCache->size() == 2,
                        "a shrink retains the overlap and prunes the departing frames");
    background.fillNextFrame();
    expectations.expect(fixture.preparationCount.load() == beforeShrink &&
                            fixture.frameCache->size() == 2,
                        "a fully cached shrink submits no new fill");

    background.beginShutdown();
    finishFixture(fixture, expectations);
}

// TEMPORAL-2B: background fill after a finite clip-range edit submits only the invalidated frames,
// keeps the retained ones, and the markers span the resulting multiple genuine revisions.
void testBackgroundFillFiniteClipRange(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Background Finite Range", time(7, 25)));
    expectations.expect(fixture.session.addSolidLayer("Fill", core::Color4d{0.2, 0.4, 0.8, 1.0}),
                        "the finite-range background fixture has content");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the foreground frame is ready");
    const auto layerId = fixture.session.composition()->graph().layerOutputs().front().layerId;
    fixture.frameCache->clear();
    ui::BackgroundPreviewController background(fixture.session, fixture.controller,
                                               fixture.scheduler, fixture.bridge,
                                               fixture.countingPipeline());
    const auto fillTo = [&](const std::size_t expected) {
        for (int attempt = 0; attempt < 400 && fixture.frameCache->size() < expected; ++attempt) {
            background.fillNextFrame();
            if (!waitUntil([&] { return fixture.frameCache->size() >= expected; }))
                break;
        }
        return fixture.frameCache->size() >= expected;
    };
    expectations.expect(fillTo(7), "the whole seven-frame range fills");
    const auto afterFullFill = fixture.preparationCount.load();
    const auto oldRevision = fixture.session.snapshot().revision();

    commands::Transaction trim("Trim", fixture.session.snapshot().revision());
    trim.emplace<commands::SetLayerRange>(fixture.session.compositionId(), layerId,
                                          core::RationalTime{}, time(3, 25));
    expectations.expect(fixture.session.executeTransaction(std::move(trim)).changed(),
                        "the trim publishes");
    expectations.expect(fillTo(7) && fixture.preparationCount.load() == afterFullFill + 4,
                        "background fills only the four invalidated frames");
    const auto probe = fixture.controller.cacheKeyForTime(time(0, 25));
    expectations.expect(probe.has_value() && probe->sourceRevision == oldRevision &&
                            fixture.frameCache->timesFor(*probe).size() == 7,
                        "background markers span multiple retained revisions");

    background.beginShutdown();
    finishFixture(fixture, expectations);
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    try {
        testPointerPressCancelsBeforePreviewChanges(expectations);
        testHalfCachedPlaybackAdmitsIdleMissesAndBackpressures(expectations);
        testVisibleAdmissionAndSupersession(expectations);
        testBackgroundFillsAheadWhilePlaying(expectations);
        testWorkAreaBoundsBackground(expectations);
        testBackgroundFillHonoursRegionOfInterest(expectations);
        testOutwardOrderBudgetAndRestart(expectations);
        testYieldsAndKeepsCancelledHandleUntilTerminal(expectations);
        testLayoutEditKeepsBackgroundOnRetainedRevision(expectations);
        testBackgroundFillFollowsWorkArea(expectations);
        testBackgroundFillFiniteClipRange(expectations);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return expectations.failures() == 0 ? 0 : 1;
}
