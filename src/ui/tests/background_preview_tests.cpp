#include <bloom/commands/command_stack.hpp>
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
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
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
    ui::PreviewFrameCacheHandle frameCache = std::make_shared<ui::PreviewFrameCache>();
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
                      const std::optional<runtime::SnapshotParameterOverride>& interactionOverride,
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
                       const std::optional<runtime::SnapshotParameterOverride>& override,
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
                       const std::optional<runtime::SnapshotParameterOverride>& override,
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
                       const std::optional<runtime::SnapshotParameterOverride>& override,
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

void testHalfCachedPlaybackNeverWaits(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Half cached playback", time(8, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }), "initial ready");
    for (const auto index : {2, 4, 6, 0}) {
        (void)fixture.session.setCurrentTime(time(index, 25));
        expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                            "seed cached frame");
    }
    fixture.controller.recordPreparationDuration(
        fixture.controller.state().frame->desiredIdentity(), 1h);
    auto now = std::chrono::steady_clock::now();
    ui::PlaybackController playback(fixture.session, fixture.controller, [&] { return now; }, 16ms);
    const auto before = fixture.preparationCount.load();
    playback.play();
    for (int index = 1; index <= 7; ++index) {
        now += 40ms;
        playback.tick();
        expectations.expect(fixture.session.currentTime() == time(index, 25),
                            "clock advances on every tick");
        const auto shown = index % 2 == 0 ? index : index - 1;
        expectations.expect(fixture.controller.state().frame != nullptr &&
                                fixture.controller.state().frame->desiredIdentity().time ==
                                    time(shown, 25),
                            "cached frames show immediately; misses retain the previous picture");
        expectations.expect(fixture.controller.droppedFrameCount() ==
                                static_cast<std::uint64_t>((index + 1) / 2),
                            "each uncached frame is counted once");
    }
    expectations.expect(fixture.preparationCount.load() == before,
                        "slow misses submit no evaluations");
    now += 200ms;
    playback.tick();
    expectations.expect(fixture.controller.droppedFrameCount() == 4,
                        "cached wrap advances one frame without drops even after a host stall");
    now += 40ms;
    playback.tick();
    expectations.expect(fixture.controller.droppedFrameCount() == 9,
                        "uncached catch-up counts four skipped indices and its uncached target");
    playback.pause();
    expectations.expect(fixture.controller.droppedFrameCount() == 9, "pause retains the run total");
    finishFixture(fixture, expectations);
}

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
                       const std::optional<runtime::SnapshotParameterOverride>& override,
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

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    try {
        testPointerPressCancelsBeforePreviewChanges(expectations);
        testHalfCachedPlaybackNeverWaits(expectations);
        testVisibleAdmissionAndSupersession(expectations);
        testBackgroundFillsAheadWhilePlaying(expectations);
        testWorkAreaBoundsBackground(expectations);
        testOutwardOrderBudgetAndRestart(expectations);
        testYieldsAndKeepsCancelledHandleUntilTerminal(expectations);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return expectations.failures() == 0 ? 0 : 1;
}
