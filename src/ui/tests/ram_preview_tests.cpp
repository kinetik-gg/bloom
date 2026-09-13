// Task PERF1: the RAM preview cache, the compiled-plan cache, and the RAM Preview command.
//
// Everything here is driven offscreen with a manually advanced clock and a preparation function
// that counts what it was asked to render -- never by waiting on real wall time and never by
// measuring speed. What is pinned is not how fast a frame is but WHETHER a frame was rendered at
// all: a cache that works shows up as an invocation count that stops moving.
#include <bloom/commands/command_stack.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
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

// An animated 24-frame composition: one solid layer whose position is keyed at the first and last
// frame, so every frame of the range is a different picture and a cache that served the wrong frame
// would be visible rather than coincidentally right.
[[nodiscard]] bool animateSolidLayer(ui::CompositionSession& session) {
    if (!session.addSolidLayer(QStringLiteral("Moving Solid"), core::Color4d{0.2, 0.4, 0.8, 1.0})) {
        return false;
    }
    if (session.currentTime() != time(0) && !session.setCurrentTime(time(0))) {
        return false;
    }
    if (!session.toggleKeyframe("position")) {
        return false;
    }
    if (!session.setCurrentTime(time(23, 25))) {
        return false;
    }
    if (!session.setSelectedPosition(12.0, 9.0)) {
        return false;
    }
    return session.setCurrentTime(time(0));
}

struct ManualClock final {
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

    void advance(const std::chrono::nanoseconds delta) { now += delta; }
};

// ---------------------------------------------------------------------------------------------

void testCompiledPlanCacheCompilesOncePerRevision(Expectations& expectations) {
    auto newProject = makeTestProject("Plan Cache", time(1));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);

    runtime::NodeDefinitionRegistry definitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(definitions),
                        "the plan-cache fixture registers the built-in node definitions");
    definitions.freeze();
    const runtime::SnapshotCompiler compiler(definitions);
    ui::CompiledPlanCache cache;

    const auto first = cache.compile(
        compiler, {.snapshot = session.snapshot(), .compositionId = compositionId}, {});
    const auto second = cache.compile(
        compiler, {.snapshot = session.snapshot(), .compositionId = compositionId}, {});
    expectations.expect(first.status == runtime::SnapshotCompileStatus::Compiled &&
                            first.plan != nullptr && second.plan == first.plan,
                        "two requests at one revision share one compiled plan object");
    expectations.expect(cache.statistics() ==
                            ui::CompiledPlanCache::Statistics{.compiles = 1, .hits = 1},
                        "the second request at one revision compiles nothing");

    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.5, 0.5, 0.5, 1.0}),
        "a command advances the document revision");
    const auto third = cache.compile(
        compiler, {.snapshot = session.snapshot(), .compositionId = compositionId}, {});
    expectations.expect(third.plan != nullptr && third.plan != first.plan &&
                            cache.statistics().compiles == 2,
                        "a new revision recompiles, and produces a different plan");
    expectations.expect(cache.size() == 2,
                        "both revisions are retained inside the cache's capacity");

    // An overridden request is this gesture frame's plan, never the revision's.
    const auto* layer = session.selectedNode();
    expectations.expect(layer != nullptr, "the new solid layer is selected");
    const auto overridden = cache.compile(
        compiler,
        {.snapshot = session.snapshot(),
         .compositionId = compositionId,
         .parameterOverride = runtime::SnapshotParameterOverride{session.snapshot().revision(),
                                                                 document::ParameterId{},
                                                                 document::Vec2d{1.0, 1.0}}},
        {});
    expectations.expect(cache.statistics().compiles == 3 && cache.size() == 2,
                        "an overridden request compiles directly and is never retained");
    expectations.expect(overridden.status != runtime::SnapshotCompileStatus::Compiled ||
                            overridden.plan != third.plan,
                        "an overridden request never hands back the revision's own plan");
}

void testCacheHitPublishesWithoutEvaluating(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Frame Cache Hit", time(1)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the first frame renders");
    const auto afterFirstFrame = fixture.preparationCount.load();

    expectations.expect(fixture.session.setCurrentTime(time(1, 25)), "move to frame one");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "frame one renders");
    const auto afterSecondFrame = fixture.preparationCount.load();
    expectations.expect(afterSecondFrame == afterFirstFrame + 1,
                        "a new exact time is a new frame and is evaluated once");

    // Back to frame zero: the answer is already in the cache, so it has to be published by the time
    // setCurrentTime() returns -- no task, no event loop, no waiting.
    expectations.expect(fixture.session.setCurrentTime(time(0)), "move back to frame zero");
    expectations.expect(isReady(fixture.controller) &&
                            fixture.controller.state().freshness == ui::FrameFreshness::Current,
                        "a cached key publishes a current frame synchronously");
    expectations.expect(fixture.controller.state().taskId == std::nullopt,
                        "a cached frame is published with no task at all");
    expectations.expect(fixture.preparationCount.load() == afterSecondFrame,
                        "a cache hit evaluates nothing");
    expectations.expect(fixture.controller.frameCache().statistics().hits == 1,
                        "the cache counts the hit it served");
    expectations.expect(fixture.controller.state().frame != nullptr &&
                            fixture.controller.state().frame->desiredIdentity() ==
                                fixture.controller.state().desiredIdentity,
                        "the frame published from the cache carries this request's own identity");

    finishFixture(fixture, expectations);
}

void testFrameCacheEvictsUnderBudgetAndDropsStaleRevisions(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Frame Cache Budget", time(1)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the budget fixture renders its first frame");

    std::vector<ui::PreparedPreviewFrameHandle> frames;
    // From frame one: frame zero is where the session already is, and setCurrentTime() reports "no
    // change" rather than success for a time it is already at.
    for (std::int64_t frameIndex = 1; frameIndex <= 3; ++frameIndex) {
        expectations.expect(fixture.session.setCurrentTime(time(frameIndex, 25)),
                            "the budget fixture reaches each frame");
        expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                            "each frame of the budget fixture renders");
        frames.push_back(fixture.controller.state().frame);
    }
    expectations.expect(frames.size() == 3 && frames[0] != nullptr && frames[1] != nullptr &&
                            frames[2] != nullptr,
                        "three distinct prepared frames are available to retain");
    if (frames.size() != 3 || frames[2] == nullptr) {
        finishFixture(fixture, expectations);
        return;
    }

    const auto frameBytes = ui::PreviewFrameCache::frameByteCost(*frames[2]);
    expectations.expect(frameBytes > 0, "a prepared frame has a measurable memory cost");
    ui::PreviewFrameCache cache(frameBytes * 2);
    for (const auto& frame : frames) {
        cache.insert(frame);
    }
    expectations.expect(cache.size() == 2 && cache.residentBytes() == frameBytes * 2 &&
                            cache.statistics().evictions == 1,
                        "the budget holds two frames and evicts the least recently used third");
    expectations.expect(
        !cache.contains(ui::PreviewFrameCacheKey::forIdentity(frames[0]->desiredIdentity())),
        "the evicted entry is the one used longest ago");

    // Shrinking the budget evicts immediately rather than at the next insert.
    cache.setByteBudget(frameBytes);
    expectations.expect(cache.size() == 1 && cache.statistics().evictions == 2,
                        "a smaller budget evicts at once");
    // A frame larger than the whole budget is refused rather than emptying the cache for itself.
    cache.setByteBudget(frameBytes - 1);
    expectations.expect(cache.size() == 0, "a budget below one frame holds nothing");
    cache.insert(frames[2]);
    expectations.expect(cache.size() == 0 && cache.statistics().rejections == 1,
                        "a frame that cannot fit the budget is refused, not forced in");

    // A newer revision makes every retained entry unreachable, and inserting one drops them.
    cache.setByteBudget(frameBytes * 8);
    for (const auto& frame : frames) {
        cache.insert(frame);
    }
    const auto beforeEdit = cache.size();
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Edit"), core::Color4d{1.0, 0.0, 0.0, 1.0}),
        "an edit advances the document revision");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the edited revision renders a frame");
    const auto newRevisionFrame = fixture.controller.state().frame;
    expectations.expect(beforeEdit == 3 && newRevisionFrame != nullptr,
                        "the edited revision produced a frame to insert");
    if (newRevisionFrame != nullptr) {
        cache.insert(newRevisionFrame);
        expectations.expect(cache.size() == 1 && cache.statistics().staleDrops == 3,
                            "a frame of a newer revision drops every entry of the older one");
    }

    finishFixture(fixture, expectations);
}

void testRamPreviewCachesTheRangeThenPlaysEveryFrame(Expectations& expectations) {
    // Twenty-four frames at 25 fps: [0, 24/25) holds exactly frame indices 0..23.
    SessionFixture fixture(makeTestProject("RAM Preview Range", time(24, 25)));
    expectations.expect(animateSolidLayer(fixture.session),
                        "the fixture composition is animated across its range");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the animated composition renders its first frame");

    ui::RamPreviewController ramPreview(fixture.session, fixture.controller, fixture.scheduler,
                                        fixture.bridge, fixture.countingPipeline());
    ramPreview.start();
    expectations.expect(ramPreview.isCaching() && ramPreview.totalFrameCount() == 24,
                        "a RAM preview of this composition covers exactly twenty-four frames");
    expectations.expect(waitUntil([&] { return !ramPreview.isCaching(); }),
                        "the RAM preview finishes caching its range");
    expectations.expect(ramPreview.cachedFrameCount() == 24,
                        "every frame of the range reached the cache");
    expectations.expect(fixture.controller.frameCache().size() >= 24,
                        "the cache holds at least the twenty-four frames of the range");
    expectations.expect(fixture.controller.ramPreviewProgress() == std::nullopt,
                        "the footer's progress readout goes silent when the run ends");

    // Playback from here must not evaluate anything at all.
    const auto afterCaching = fixture.preparationCount.load();
    ManualClock clock;
    ui::PlaybackController playback(
        fixture.session, fixture.controller, [&clock] { return clock.now; }, 16ms);
    std::vector<core::RationalTime> presented;
    QObject::connect(&fixture.session, &ui::CompositionSession::currentTimeChanged,
                     &fixture.session, [&] { presented.push_back(fixture.session.currentTime()); });
    playback.play();
    for (int tickIndex = 0; tickIndex < 24; ++tickIndex) {
        clock.advance(40'000'000ns);
        playback.tick();
    }
    playback.pause();

    std::vector<core::RationalTime> expected;
    for (std::int64_t frameIndex = 1; frameIndex < 24; ++frameIndex) {
        expected.push_back(time(frameIndex, 25));
    }
    expected.push_back(time(0)); // the loop wrap back to the first frame
    expectations.expect(presented == expected,
                        "playback presents every frame of the cached range in order, and wraps");
    expectations.expect(fixture.preparationCount.load() == afterCaching,
                        "playing a fully cached range evaluates nothing");
    expectations.expect(fixture.controller.droppedFrameCount() == 0,
                        "a fully cached playback run drops no frames");

    // A second RAM preview of an unedited range renders nothing at all.
    const auto beforeSecondRun = fixture.preparationCount.load();
    ramPreview.start();
    expectations.expect(waitUntil([&] { return !ramPreview.isCaching(); }),
                        "the second RAM preview finishes");
    expectations.expect(ramPreview.cachedFrameCount() == 24 &&
                            fixture.preparationCount.load() == beforeSecondRun,
                        "a RAM preview of an already cached range evaluates nothing");

    finishFixture(fixture, expectations);
}

// A range that cannot fit the cache's budget is cached as the prefix that does fit, and the run
// stops there rather than spending the rest of the range evicting its own beginning.
void testRamPreviewStopsWhenTheRangeOutgrowsTheBudget(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM Preview Budget", time(24, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the budget-limited fixture renders its first frame");
    const auto frame = fixture.controller.state().frame;
    expectations.expect(frame != nullptr, "the budget-limited fixture has a frame to measure");
    if (frame == nullptr) {
        finishFixture(fixture, expectations);
        return;
    }
    // Room for three frames of this composition, against a range of twenty-four.
    fixture.controller.frameCache().setByteBudget(ui::PreviewFrameCache::frameByteCost(*frame) * 3);

    ui::RamPreviewController ramPreview(fixture.session, fixture.controller, fixture.scheduler,
                                        fixture.bridge, fixture.countingPipeline());
    bool finishedCompleted = false;
    QObject::connect(&ramPreview, &ui::RamPreviewController::cachingFinished, &ramPreview,
                     [&finishedCompleted](const bool completed) { finishedCompleted = completed; });
    ramPreview.start();
    expectations.expect(waitUntil([&] { return !ramPreview.isCaching(); }),
                        "the budget-limited run ends on its own");
    expectations.expect(ramPreview.cachedFrameCount() == 4 && ramPreview.totalFrameCount() == 24,
                        "the run keeps the prefix that fits the budget and stops there");
    expectations.expect(fixture.controller.frameCache().size() == 3,
                        "the cache holds exactly what its budget allows");
    expectations.expect(finishedCompleted,
                        "a budget-limited run still finishes, so the cached prefix is played");

    finishFixture(fixture, expectations);
}

void testRamPreviewCancellationKeepsWhatItCached(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM Preview Cancel", time(24, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the cancellation fixture renders its first frame");
    ui::ViewerEditor viewer(fixture.session, fixture.controller);

    // Pause the run's FIRST preparation on the worker. Frame zero is already cached -- the
    // fixture's own opening frame -- so the run counts that one without rendering it and pauses on
    // frame one, which is exactly one frame into a twenty-four frame range.
    fixture.gateAtCall = fixture.preparationCount.load();
    ui::RamPreviewController ramPreview(fixture.session, fixture.controller, fixture.scheduler,
                                        fixture.bridge, fixture.countingPipeline());
    ramPreview.start();
    expectations.expect(waitUntil([&] { return fixture.gate.entered(); }),
                        "the run reaches its first uncached frame on the worker");
    expectations.expect(ramPreview.isCaching() && ramPreview.cachedFrameCount() == 1,
                        "one frame of the range is cached at the rendezvous");
    expectations.expect(viewer.statusBarRamPreviewTextForTest() == QStringLiteral("Caching 1/24"),
                        "the Viewer footer reports the run's own progress while it caches");

    ramPreview.cancel();
    fixture.gate.release();
    expectations.expect(!ramPreview.isCaching(), "cancelling ends the run at once");
    expectations.expect(viewer.statusBarRamPreviewTextForTest().isEmpty(),
                        "the footer says nothing about a run that is not caching");
    expectations.expect(ramPreview.cachedFrameCount() == 1,
                        "a cancelled run keeps every frame it had already cached");
    expectations.expect(waitUntil([&] {
                            return fixture.scheduler.isQuiescent() ||
                                   !fixture.controller.state().taskId.has_value();
                        }),
                        "the cancelled frame's task reaches terminal");

    finishFixture(fixture, expectations);
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    // Same shape as cpu_composition_evaluator_tests.cpp's main(): the fixtures here compare
    // std::variant-carrying identities, so the standard library's own throwing paths are reachable
    // in principle and main() must not be the frame they escape from.
    try {
        testCompiledPlanCacheCompilesOncePerRevision(expectations);
        testCacheHitPublishesWithoutEvaluating(expectations);
        testFrameCacheEvictsUnderBudgetAndDropsStaleRevisions(expectations);
        testRamPreviewCachesTheRangeThenPlaysEveryFrame(expectations);
        testRamPreviewStopsWhenTheRangeOutgrowsTheBudget(expectations);
        testRamPreviewCancellationKeepsWhatItCached(expectations);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    return expectations.failures() == 0 ? 0 : 1;
}
