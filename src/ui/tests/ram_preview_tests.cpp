// Task PERF1: the RAM preview cache, the compiled-plan cache, and the RAM Preview command.
//
// Everything here is driven offscreen with a manually advanced clock and a preparation function
// that counts what it was asked to render -- never by waiting on real wall time and never by
// measuring speed. What is pinned is not how fast a frame is but WHETHER a frame was rendered at
// all: a cache that works shows up as an invocation count that stops moving.
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
#include <bloom/ui/composition_plan_cache.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QTemporaryDir>

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
    ui::PreviewFrameCacheHandle frameCache = std::make_shared<ui::PreviewFrameCache>();
    std::atomic<int> preparationCount = 0;
    std::atomic<std::size_t> operationHits = 0;
    std::atomic<std::size_t> operationMisses = 0;
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
            auto result = pipelineFixture.pipeline(snapshot, desiredIdentity, pixelStorageByteLimit,
                                                   interactionOverride, context);
            if (result.value() && *result.value() && (*result.value())->frame() &&
                (*result.value())->frame()->processFrame()) {
                const auto& statistics =
                    (*result.value())->frame()->processFrame()->operationCacheStatistics();
                operationHits.fetch_add(statistics.hits);
                operationMisses.fetch_add(statistics.misses);
            }
            return result;
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
         .parameterOverrides = {runtime::SnapshotParameterOverride{
             session.snapshot().revision(), document::ParameterId{}, document::Vec2d{1.0, 1.0}}}},
        {});
    expectations.expect(cache.statistics().compiles == 3 && cache.size() == 2,
                        "an overridden request compiles directly and is never retained");
    expectations.expect(overridden.status != runtime::SnapshotCompileStatus::Compiled ||
                            overridden.plan != third.plan,
                        "an overridden request never hands back the revision's own plan");
}

void testCacheHitPublishesWithoutEvaluating(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Frame Cache Hit", time(1)));
    expectations.expect(fixture.session.addSolidLayer("Bounds", {0.2, 0.4, 0.8, 1}),
                        "cache fixture creates selected content");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the first frame renders");
    const auto afterFirstFrame = fixture.preparationCount.load();
    const auto freshFrame = fixture.controller.state().frame;
    const auto freshBounds = fixture.controller.selectedLayerBounds();
    expectations.expect(freshBounds.size() == 1, "fresh frame has selected layer geometry");
    const auto freshView = freshFrame == nullptr ? std::nullopt : freshFrame->displayBufferView();
    expectations.expect(freshFrame != nullptr && freshFrame->hasProcessFrame() &&
                            freshView.has_value() && !freshView->pixels.empty(),
                        "the freshly evaluated frame carries its process frame and its pixels");
    const bool freshQualified = freshView.has_value() && freshView->isOcioQualified;
    const std::vector<render::Rgba8> freshPixels =
        freshView.has_value()
            ? std::vector<render::Rgba8>(freshView->pixels.begin(), freshView->pixels.end())
            : std::vector<render::Rgba8>{};

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

    // FORMAL AMENDMENT 1: what comes back from the cache is the packed display buffer and nothing
    // else. It is the same picture -- byte for byte the pixels the evaluation published -- but it
    // carries no process frame, and says so rather than handing one back that is silently null.
    const auto cachedFrame = fixture.controller.state().frame;
    expectations.expect(
        fixture.controller.selectedLayerBounds() == freshBounds,
        "cache hit retains exact selected geometry without evaluating or retaining process pixels");
    expectations.expect(cachedFrame != nullptr && !cachedFrame->hasProcessFrame(),
                        "a frame served from the cache carries no process frame");
    expectations.expect(cachedFrame != nullptr && cachedFrame->processFrame() == nullptr,
                        "its process-frame handle is null rather than dangling");
    const auto cachedView =
        cachedFrame == nullptr ? std::nullopt : cachedFrame->displayBufferView();
    expectations.expect(cachedView.has_value() && freshPixels.size() == cachedView->pixels.size() &&
                            std::ranges::equal(freshPixels, cachedView->pixels),
                        "a cache hit paints exactly the pixels the evaluation published");
    expectations.expect(cachedFrame != nullptr && freshFrame != nullptr &&
                            cachedFrame->isOcioQualified() == freshFrame->isOcioQualified() &&
                            cachedView.has_value() && cachedView->isOcioQualified == freshQualified,
                        "stripping the process image does not relabel which transform made the "
                        "pixels");

    finishFixture(fixture, expectations);
}

// The amendment's own promise, stated as a lifetime: once a frame is in the cache, nothing keeps
// its Float32 process image alive.
void testCachingReleasesTheProcessImage(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Cache Footprint", time(1)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the footprint fixture renders its first frame");
    auto firstFrame = fixture.controller.state().frame;
    expectations.expect(firstFrame != nullptr && firstFrame->hasProcessFrame(),
                        "a freshly evaluated frame does carry its process frame");
    if (firstFrame == nullptr) {
        finishFixture(fixture, expectations);
        return;
    }
    const auto firstKey = ui::PreviewFrameCacheKey::forIdentity(firstFrame->desiredIdentity());
    const std::weak_ptr<const runtime::ProcessFrame> processFrame = firstFrame->processFrame();
    const auto displayBytes = ui::PreviewFrameCache::frameByteCost(*firstFrame);
    const auto processBytes = firstFrame->processImage().pixels().size_bytes();
    const auto geometryBytes = firstFrame->evaluatedBounds().size_bytes();
    // Released here, so that from this line on the cache is the only thing that could still be
    // holding that frame -- which is exactly what the weak handle below is asking about.
    firstFrame.reset();
    expectations.expect(displayBytes > 0 && processBytes == (displayBytes - geometryBytes) * 4,
                        "this composition's process image is four times its display buffer");
    expectations.expect(
        fixture.controller.frameCache().residentBytes() == displayBytes,
        "the cache accounts for display pixels and geometry, never the process image");

    // Move the session on twice, so nothing but the cache still refers to that first frame: the
    // controller's state holds a newer one and no request retains an older.
    expectations.expect(fixture.session.setCurrentTime(time(1, 25)) &&
                            waitUntil([&] { return isReady(fixture.controller); }),
                        "the footprint fixture reaches a second frame");
    expectations.expect(fixture.session.setCurrentTime(time(2, 25)) &&
                            waitUntil([&] { return isReady(fixture.controller); }),
                        "the footprint fixture reaches a third frame");

    expectations.expect(fixture.controller.frameCache().contains(firstKey),
                        "the first frame is still cached");
    expectations.expect(processFrame.expired(),
                        "caching a frame does not keep its Float32 process image alive");
    expectations.expect(
        fixture.controller.frameCache().residentBytes() == displayBytes * 3,
        "three cached frames cost three display buffers with their evaluated geometry");

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
    // CACHEFIX-1: the refused frame is still a frame. Retaining it was an optimization; the handle
    // the controller published is untouched and the viewer can still paint from it.
    expectations.expect(frames[2] != nullptr && frames[2]->displayBufferView().has_value(),
                        "a frame the cache refused is still renderable by its owner");

    // CACHEFIX-1 memory pressure: trimToBytes() evicts without surrendering the budget, so the
    // cache refills once the machine recovers instead of staying permanently halved.
    cache.setByteBudget(frameBytes * 4);
    for (const auto& frame : frames) {
        cache.insert(frame);
    }
    const auto beforeTrim = cache.residentBytes();
    const auto trimBudget = cache.byteBudget();
    const auto beforeDrops = cache.statistics().pressureDrops;
    cache.trimToBytes(frameBytes);
    expectations.expect(cache.residentBytes() <= frameBytes && beforeTrim > frameBytes,
                        "a pressure trim evicts down to the limit it was given");
    expectations.expect(cache.byteBudget() == trimBudget,
                        "a pressure trim leaves the configured budget alone");
    expectations.expect(cache.statistics().pressureDrops > beforeDrops,
                        "trimmed frames are counted apart from ordinary budget eviction");
    cache.insert(frames[0]);
    cache.insert(frames[1]);
    expectations.expect(cache.residentBytes() > frameBytes,
                        "the cache fills back up after a trim, because the budget survived");
    cache.clear();

    {
        constexpr auto gib = std::size_t{1024} * 1024 * 1024;
        runtime::MemoryBudgetLedger ledger(16 * gib, 16 * gib);
        ledger.setConfiguredTotal(frameBytes * 4);
        ui::PreviewFrameCache participating(frameBytes * 4, ledger);
        for (const auto& frame : frames)
            participating.insert(frame);
        const auto state = ledger.poll({.availableBytes = gib});
        expectations.expect(state.retentionPercent == 25 &&
                                participating.residentBytes() <= frameBytes &&
                                participating.byteBudget() <= frameBytes,
                            "the shared ledger trims preview storage and admission together");
        for (const auto& frame : frames)
            participating.insert(frame);
        static_cast<void>(ledger.poll({.availableBytes = gib}));
        expectations.expect(participating.residentBytes() == 0 &&
                                participating.byteBudget() < frameBytes,
                            "two pressure polls prevent the preview cache from refilling");
    }

    // A standalone cache with no temporal provenance policy keeps the original conservative rule: a
    // frame of a newer revision drops every older-revision entry of the project. (The shared
    // preview cache installs the session's time-indexed policy instead, which retains unaffected
    // segments.)
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
                            "a standalone cache drops every older-revision entry by construction");
    }

    finishFixture(fixture, expectations);
}

void testRamPreviewWorkArea(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM work area", time(7, 25)));
    commands::Transaction range("Work area", fixture.session.snapshot().revision());
    range.emplace<commands::SetWorkArea>(fixture.session.compositionId(), time(2, 25), time(5, 25));
    expectations.expect(fixture.session.executeTransaction(std::move(range)).changed(),
                        "work area accepted");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "foreground settled");
    fixture.frameCache->clear();
    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge, fixture.countingPipeline());
    ram.start();
    expectations.expect(ram.totalFrameCount() == 3,
                        "RAM progress counts work-area frames, excluding its offset");
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) && ram.cachedFrameCount() == 3,
                        "RAM caches exactly its work area");
    for (int frame = 0; frame < 7; ++frame) {
        const auto key = fixture.controller.cacheKeyForTime(time(frame, 25));
        expectations.expect(key && fixture.frameCache->contains(*key) == (frame >= 2 && frame < 5),
                            "RAM uses half-open work-area frame bounds");
    }
    ram.beginShutdown();
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
    // ADAPTED for task VIEW-1: RAM preview progress reports in the WINDOW STATUS BAR now, not the
    // viewer footer -- it is application state, and it has to stay visible whether or not a Viewer
    // is open. The free function the strip's own cell calls is what is read here.
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
    expectations.expect(ui::previewCacheText(fixture.controller) == QStringLiteral("Caching 1/24"),
                        "the status bar reports the run's own progress while it caches");

    ramPreview.cancel();
    fixture.gate.release();
    expectations.expect(!ramPreview.isCaching(), "cancelling ends the run at once");
    expectations.expect(
        !ui::previewCacheText(fixture.controller).startsWith(QStringLiteral("Caching")),
        "and stops claiming a run once one is no longer caching");
    expectations.expect(ramPreview.cachedFrameCount() == 1,
                        "a cancelled run keeps every frame it had already cached");
    expectations.expect(waitUntil([&] {
                            return fixture.scheduler.isQuiescent() ||
                                   !fixture.controller.state().taskId.has_value();
                        }),
                        "the cancelled frame's task reaches terminal");

    finishFixture(fixture, expectations);
}

void testRamPreviewSharesResolutionAndCachesByPolicy(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Resolution cache", time(3, 25)));
    fixture.controller.setDisplayedCompositionScale(0.25);
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "Auto Quarter becomes ready");
    ui::RamPreviewController ramPreview(fixture.session, fixture.controller, fixture.scheduler,
                                        fixture.bridge, fixture.countingPipeline());
    ramPreview.start();
    expectations.expect(waitUntil([&] { return !ramPreview.isCaching(); }) &&
                            ramPreview.cachedFrameCount() == 3,
                        "RAM preview fills the Auto Quarter range");
    const auto count = fixture.preparationCount.load();
    expectations.expect(fixture.session.setCurrentTime(time(1, 25)),
                        "advance to a cached proxy time");
    expectations.expect(isReady(fixture.controller) && fixture.preparationCount.load() == count &&
                            !fixture.controller.state().frame->hasProcessFrame(),
                        "ordinary playback consumes RAM preview's proxy buffer without evaluation");
    const auto autoKey = fixture.controller.cacheKeyForTime(time(1, 25));
    fixture.controller.setResolutionPolicy(runtime::PreviewResolutionPolicy::Quarter);
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }) &&
                            fixture.preparationCount.load() > count,
                        "fixed Quarter has its own request policy identity");
    expectations.expect(autoKey.has_value() && fixture.controller.frameCache().contains(*autoKey),
                        "switching policy preserves the reusable Auto cache entry");
    const auto afterFixed = fixture.preparationCount.load();
    fixture.controller.setResolutionPolicy(runtime::PreviewResolutionPolicy::Auto);
    expectations.expect(isReady(fixture.controller) &&
                            fixture.preparationCount.load() == afterFixed,
                        "returning to Auto serves its cache with the new request generation");
    fixture.controller.setResolutionPolicy(runtime::PreviewResolutionPolicy::Half);
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "Half becomes ready");
    ramPreview.start();
    expectations.expect(waitUntil([&] { return !ramPreview.isCaching(); }) &&
                            ramPreview.cachedFrameCount() == 3,
                        "RAM preview fills the separate Half range");
    const auto halfCount = fixture.preparationCount.load();
    ramPreview.start();
    expectations.expect(!ramPreview.isCaching() && fixture.preparationCount.load() == halfCount,
                        "repeating Half RAM preview is entirely cached");
    finishFixture(fixture, expectations);
}

void testResolutionChangeCancelsAnActiveRamPreview(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Resolution change during RAM preview", time(3, 25)));
    fixture.controller.setDisplayedCompositionScale(0.25);
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "Auto Quarter becomes ready");
    ui::RamPreviewController ramPreview(fixture.session, fixture.controller, fixture.scheduler,
                                        fixture.bridge, fixture.countingPipeline());
    fixture.gateAtCall = fixture.preparationCount.load();
    ramPreview.start();
    expectations.expect(waitUntil([&] { return fixture.gate.entered(); }),
                        "a RAM preview proxy is in flight");
    bool cancelled = false;
    QObject::connect(&ramPreview, &ui::RamPreviewController::cachingFinished, &fixture.bridge,
                     [&cancelled](const bool completed) { cancelled = !completed; });
    fixture.controller.setDisplayedCompositionScale(0.5);
    expectations.expect(!ramPreview.isCaching() && cancelled &&
                            !fixture.controller.ramPreviewProgress().has_value(),
                        "an Auto factor change cancels the old range without starting playback");
    fixture.gate.release();
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the new Half preview reaches ready");
    expectations.expect(ramPreview.cachedFrameCount() == 1,
                        "the cancelled old factor does not count or publish its in-flight frame");
    ramPreview.start();
    expectations.expect(waitUntil([&] { return !ramPreview.isCaching(); }) &&
                            ramPreview.cachedFrameCount() == 3,
                        "a fresh RAM preview uses Half throughout");
    finishFixture(fixture, expectations);
}

void testDefaultBudgetFollowsPhysicalMemory(Expectations& expectations) {
    const auto physical = ui::physicalMemoryBytes();
    const auto budget = ui::defaultPreviewFrameCacheByteBudget();
    const auto allocation = runtime::MemoryBudgetLedger(physical).allocate();
    expectations.expect(budget >= ui::kMinimumPreviewFrameCacheByteBudget,
                        "the default RAM preview budget never drops below the floor");
    expectations.expect(budget == allocation.previewFrameCacheByteBudget,
                        "the preview cache uses the ledger's effective allocation");
    expectations.expect(allocation.operationCacheByteBudget +
                                allocation.previewFrameCacheByteBudget <=
                            allocation.usableByteBudget,
                        "the two default caches stay within the usable budget");
    if (physical != 0)
        expectations.expect(allocation.usableByteBudget <= physical,
                            "the machine-derived budget never exceeds physical memory");
    QTemporaryDir directory;
    QSettings settings(directory.filePath("playback.ini"), QSettings::IniFormat);
    expectations.expect(ui::ramPreviewByteBudgetFromSettings(settings) == budget,
                        "an unset budget setting reads as the machine-derived default");
}

void testOperationCacheUnderRamPreview(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Operation Cache", time(24, 25)));
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Static solid"), {0.2, 0.4, 0.8, 1}),
        "static solid is authored");
    expectations.expect(
        fixture.session.addTextLayer(QStringLiteral("Static text"), QStringLiteral("Bloom")),
        "static text is authored");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "static composition is ready");
    const auto beforeMisses = fixture.operationMisses.load();
    const auto beforeHits = fixture.operationHits.load();
    ui::RamPreviewController ramPreview(fixture.session, fixture.controller, fixture.scheduler,
                                        fixture.bridge, fixture.countingPipeline());
    ramPreview.start();
    expectations.expect(waitUntil([&] { return !ramPreview.isCaching(); }),
                        "static RAM preview finishes");
    expectations.expect(ramPreview.cachedFrameCount() == 24 &&
                            fixture.operationMisses.load() == beforeMisses &&
                            fixture.operationHits.load() == beforeHits + 138,
                        "23 frame-cache misses reuse all six operations below the frame cache");
    finishFixture(fixture, expectations);
    QTemporaryDir directory;
    expectations.expect(directory.isValid(), "settings test directory exists");
    if (!directory.isValid())
        return;
    QSettings settings(directory.filePath(QStringLiteral("playback.ini")), QSettings::IniFormat);
    const auto defaultAllocation = ui::cacheMemoryBudgetsFromSettings(settings);
    expectations.expect(ui::operationCacheByteBudgetFromSettings(settings) ==
                            defaultAllocation.operationCacheByteBudget,
                        "missing operation budget uses the machine-derived ledger allocation");
    for (const auto* value : {"0", "-1", "invalid", "18446744073709551616"}) {
        settings.setValue(QStringLiteral("playback/operation-cache-bytes"),
                          QString::fromLatin1(value));
        expectations.expect(ui::operationCacheByteBudgetFromSettings(settings) ==
                                defaultAllocation.operationCacheByteBudget,
                            "invalid operation budget uses the default");
    }
    settings.setValue(QStringLiteral("playback/operation-cache-bytes"), QStringLiteral("4096"));
    expectations.expect(ui::operationCacheByteBudgetFromSettings(settings) == 4096,
                        "saved budget is honored");
}

// LAYOUT-2: a RAM preview run keeps ONE retained evaluation snapshot across a layout-only edit,
// so the frames it caches stay reusable rather than being re-keyed out from under the run. A pixel
// edit changes the evaluation snapshot and cancels the run.
void testLayoutEditRetainsTheRamRun(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM Layout Retain", time(24, 25)));
    expectations.expect(animateSolidLayer(fixture.session),
                        "the fixture composition is animated across its range");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the animated composition renders its first frame");

    const auto evalRevision = fixture.session.evaluationSnapshot().revision();
    const auto nodeId = fixture.session.composition()->graph().nodes().front().id;

    // Pause the run on its first uncached frame so the layout edit lands mid-run.
    fixture.gateAtCall = fixture.preparationCount.load();
    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge, fixture.countingPipeline());
    ram.start();
    expectations.expect(waitUntil([&] { return fixture.gate.entered(); }),
                        "the RAM run reaches a frame on the worker");

    commands::Transaction move("Move Nodes", fixture.session.snapshot().revision());
    move.emplace<commands::MoveNodes>(
        fixture.session.compositionId(),
        std::map<document::NodeId, document::Vec2d>{{nodeId, {4.0, 5.0}}});
    expectations.expect(fixture.session.executeNodeTransaction(std::move(move)).changed(),
                        "the layout edit publishes while the run is in flight");
    expectations.expect(fixture.session.snapshot().revision() != evalRevision &&
                            fixture.session.evaluationSnapshot().revision() == evalRevision &&
                            ram.isCaching(),
                        "the layout edit retains the evaluation snapshot and the run");

    fixture.gate.release();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) &&
                            ram.cachedFrameCount() == ram.totalFrameCount(),
                        "the run completes under one retained evaluation revision");
    for (std::uint64_t frame = 0; frame < ram.totalFrameCount(); ++frame) {
        const auto key =
            fixture.controller.cacheKeyForTime(time(static_cast<std::int64_t>(frame), 25));
        expectations.expect(key.has_value() && key->sourceRevision == evalRevision &&
                                fixture.frameCache->contains(*key),
                            "every cached frame is reusable under the retained revision");
    }

    ram.beginShutdown();
    finishFixture(fixture, expectations);
}

// LAYOUT-2: a pixel edit advances the evaluation snapshot, which cancels a RAM run in flight
// because its cached frames would belong to an evaluation the artist has left behind.
void testPixelEditCancelsTheRamRun(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM Pixel Cancel", time(24, 25)));
    expectations.expect(animateSolidLayer(fixture.session),
                        "the fixture composition is animated across its range");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the animated composition renders its first frame");
    const auto nodeId = fixture.session.composition()->graph().nodes().front().id;

    fixture.gateAtCall = fixture.preparationCount.load();
    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge, fixture.countingPipeline());
    ram.start();
    expectations.expect(waitUntil([&] { return fixture.gate.entered(); }) && ram.isCaching(),
                        "the RAM run is in flight on the worker");

    commands::Transaction mute("Mute Node", fixture.session.snapshot().revision());
    mute.emplace<commands::SetNodeMuted>(fixture.session.compositionId(), nodeId, true);
    expectations.expect(fixture.session.executeNodeTransaction(std::move(mute)).changed() &&
                            !ram.isCaching() &&
                            fixture.session.evaluationSnapshot().revision() ==
                                fixture.session.snapshot().revision(),
                        "a pixel edit advances evaluation and cancels the run");

    fixture.gate.release();
    ram.beginShutdown();
    finishFixture(fixture, expectations);
}

// PROVENANCE-1: the compiled-plan cache binds to the real retained snapshot, not to the numeric
// (project, composition, revision) tuple every New/Open document deliberately reuses. Copies of one
// snapshot hit; a different document with colliding numbers must recompile.
void testCompiledPlanCacheBindsSnapshotIdentity(Expectations& expectations) {
    auto firstProject = makeTestProject("Plan Provenance First", time(1));
    const auto compositionId = firstProject.initialCompositionId;
    document::Document firstDocument(std::move(firstProject.project));
    commands::CommandStack firstCommands(firstDocument);
    commands::Transaction firstAdd("Add Solid", firstDocument.snapshot().revision());
    firstAdd.emplace<commands::AddSolidLayer>(compositionId, std::string("Solid"),
                                              core::Color4d{1.0, 0.0, 0.0, 1.0});
    expectations.expect(firstCommands.execute(std::move(firstAdd)).changed(),
                        "the first document gets a red solid");

    auto secondProject = makeTestProject("Plan Provenance Second", time(1));
    const auto secondCompositionId = secondProject.initialCompositionId;
    document::Document secondDocument(std::move(secondProject.project));
    commands::CommandStack secondCommands(secondDocument);
    commands::Transaction secondAdd("Add Solid", secondDocument.snapshot().revision());
    secondAdd.emplace<commands::AddSolidLayer>(secondCompositionId, std::string("Solid"),
                                               core::Color4d{0.0, 0.0, 1.0, 1.0});
    expectations.expect(secondCommands.execute(std::move(secondAdd)).changed(),
                        "the second document gets a blue solid");
    expectations.expect(
        firstDocument.snapshot().revision() == secondDocument.snapshot().revision() &&
            firstDocument.snapshot().project().id() == secondDocument.snapshot().project().id() &&
            compositionId == secondCompositionId,
        "the two documents deliberately collide in project/composition/revision");

    runtime::NodeDefinitionRegistry definitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(definitions),
                        "the plan-provenance fixture registers node definitions");
    definitions.freeze();
    const runtime::SnapshotCompiler compiler(definitions);
    ui::CompiledPlanCache cache;

    const auto firstSnapshot = firstDocument.snapshot();
    const auto first =
        cache.compile(compiler, {.snapshot = firstSnapshot, .compositionId = compositionId}, {});
    const auto repeated =
        cache.compile(compiler, {.snapshot = firstSnapshot, .compositionId = compositionId}, {});
    expectations.expect(first.plan != nullptr && repeated.plan == first.plan &&
                            cache.statistics() ==
                                ui::CompiledPlanCache::Statistics{.compiles = 1, .hits = 1},
                        "a repeated request of the same snapshot hits");

    const auto copied = cache.compile(
        compiler, {.snapshot = firstDocument.snapshot(), .compositionId = compositionId}, {});
    expectations.expect(copied.plan == first.plan && cache.statistics().hits == 2 &&
                            cache.statistics().compiles == 1,
                        "a copied snapshot of the same document state still hits");

    const auto second = cache.compile(
        compiler, {.snapshot = secondDocument.snapshot(), .compositionId = secondCompositionId},
        {});
    expectations.expect(second.plan != nullptr && second.plan != first.plan &&
                            cache.statistics().compiles == 2 && cache.size() == 2,
                        "a colliding document recompiles instead of reusing the wrong plan");
    const auto secondAgain = cache.compile(
        compiler, {.snapshot = secondDocument.snapshot(), .compositionId = secondCompositionId},
        {});
    expectations.expect(secondAgain.plan == second.plan && cache.statistics().hits == 3,
                        "the second document's own snapshot hits");
}

// SPLIT-2: an output-equivalent split retains the whole RAM-cached range under ONE provenance; a
// subsequent RAM run of the split composition prepares nothing.
void testRamRunReusesBothHalvesAfterSplit(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM Split Reuse", time(7, 25)));
    expectations.expect(animateSolidLayer(fixture.session),
                        "the split-reuse fixture is animated across its seven frames");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the foreground frame is ready");
    const auto boundary = fixture.session.composition()->graph().layerOutputs().front();
    fixture.frameCache->clear();
    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge, fixture.countingPipeline());
    ram.start();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) && ram.cachedFrameCount() == 7,
                        "the whole range caches");
    const auto afterFullFill = fixture.preparationCount.load();
    const auto retainedRevision = fixture.session.evaluationSnapshot().revision();

    // Split at frame 3: [0,3) head, [3,7/25) tail.
    commands::Transaction split("Split", fixture.session.snapshot().revision());
    split.emplace<commands::SplitLayerAtTime>(fixture.session.compositionId(), boundary.layerId,
                                              time(3, 25));
    const auto splitResult = fixture.session.executeTransaction(std::move(split));
    expectations.expect(splitResult.changed() && splitResult.affectedTimes.has_value() &&
                            splitResult.affectedTimes->intervals.empty(),
                        "the split publishes an empty pixel footprint");
    expectations.expect(fixture.session.evaluationSnapshotForTime(time(0, 25)).revision() ==
                            retainedRevision,
                        "both halves keep the retained provenance");
    ram.start();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) &&
                            ram.cachedFrameCount() == 7 &&
                            fixture.preparationCount.load() == afterFullFill,
                        "a RAM run after the split prepares nothing");
    for (std::int64_t frame = 0; frame < 7; ++frame) {
        const auto key = fixture.controller.cacheKeyForTime(time(frame, 25));
        expectations.expect(key && fixture.frameCache->contains(*key),
                            "every frame of both halves is retained");
    }

    ram.beginShutdown();
    finishFixture(fixture, expectations);
}

// WORKAREA-1: a range edit is render-neutral, so it must not advance the evaluation snapshot or
// recompile. Expansion/shift fill only the entering frames; a fully cached shrink evaluates
// nothing; out-of-range entries are released as rangeDrops (never eviction/pressure).
void testWorkAreaRangeManagement(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("Work Area Management", time(7, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the work-area fixture renders its first frame");

    const auto setRange = [&](const core::RationalTime start, const core::RationalTime end) {
        commands::Transaction range("Set Work Area", fixture.session.snapshot().revision());
        range.emplace<commands::SetWorkArea>(fixture.session.compositionId(), start, end);
        return fixture.session.executeTransaction(std::move(range));
    };
    const auto compileCount = [&] {
        return fixture.pipelineFixture.planCache->statistics().compiles;
    };

    // [2,5): warm all three frames through a RAM run.
    expectations.expect(setRange(time(2, 25), time(5, 25)).changed(),
                        "the initial work area is set");
    const auto evalRevision = fixture.session.evaluationSnapshot().revision();
    const auto compilesAfterRange = compileCount();
    fixture.frameCache->clear();
    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge, fixture.countingPipeline());
    ram.start();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) && ram.cachedFrameCount() == 3,
                        "the three-frame work area caches");
    const auto preparationAfterFill = fixture.preparationCount.load();
    const auto residentAfterFill = fixture.frameCache->residentBytes();
    expectations.expect(fixture.session.evaluationSnapshot().revision() == evalRevision &&
                            compileCount() == compilesAfterRange,
                        "range edits compiled no plan and did not advance evaluation");

    // Shrink to [3,5): frame 2 is released as a range drop; frames 3,4 survive.
    const auto rangeDropsBefore = fixture.frameCache->statistics().rangeDrops;
    expectations.expect(setRange(time(3, 25), time(5, 25)).changed(), "the range shrinks");
    const auto key2 = fixture.controller.cacheKeyForTime(time(2, 25));
    const auto key3 = fixture.controller.cacheKeyForTime(time(3, 25));
    const auto key4 = fixture.controller.cacheKeyForTime(time(4, 25));
    expectations.expect(key2 && key3 && key4 && !fixture.frameCache->contains(*key2) &&
                            fixture.frameCache->contains(*key3) &&
                            fixture.frameCache->contains(*key4) &&
                            fixture.frameCache->residentBytes() < residentAfterFill &&
                            fixture.frameCache->statistics().rangeDrops > rangeDropsBefore,
                        "a shrink releases only the departing frame as a range drop");
    ram.start();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) &&
                            fixture.preparationCount.load() == preparationAfterFill &&
                            ram.cachedFrameCount() == 2,
                        "a fully cached shrink evaluates nothing");

    // Expand back to [1,5): the two frames the shrink released (1 and 2) are the only ones
    // evaluated; 3 and 4 stay cached.
    expectations.expect(setRange(time(1, 25), time(5, 25)).changed(), "the range expands");
    const auto beforeExpansion = fixture.preparationCount.load();
    ram.start();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) && ram.cachedFrameCount() == 4,
                        "the expanded range fills");
    const auto key1 = fixture.controller.cacheKeyForTime(time(1, 25));
    if (!key1.has_value() || !key2.has_value() || !key3.has_value() || !key4.has_value()) {
        expectations.expect(false, "the cache key fixtures are available");
        return;
    }
    expectations.expect(
        fixture.preparationCount.load() == beforeExpansion + 2 && key1 &&
            fixture.frameCache->contains(*key1) && fixture.frameCache->contains(*key2) &&
            fixture.frameCache->contains(*key3) && fixture.frameCache->contains(*key4),
        "only the two entering frames are evaluated on expansion");

    // Equal-size shift to [2,6): frame 1 departs, frame 5 enters, 2..4 are retained.
    const auto beforeShift = fixture.preparationCount.load();
    expectations.expect(setRange(time(2, 25), time(6, 25)).changed(), "the range shifts");
    ram.start();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) && ram.cachedFrameCount() == 4,
                        "the shifted range caches four frames");
    const auto key5 = fixture.controller.cacheKeyForTime(time(5, 25));
    if (!key5.has_value()) {
        expectations.expect(false, "the shifted cache key fixture is available");
        return;
    }
    expectations.expect(
        !fixture.frameCache->contains(*key1) && fixture.frameCache->contains(*key2) &&
            fixture.frameCache->contains(*key3) && fixture.frameCache->contains(*key4) && key5 &&
            fixture.frameCache->contains(*key5) &&
            fixture.preparationCount.load() == beforeShift + 1,
        "a shift retains the overlap, drops the departing frame and fills one");

    // No-op: the same range again changes nothing.
    const auto beforeNoop = fixture.preparationCount.load();
    expectations.expect(setRange(time(2, 25), time(6, 25)).status ==
                            commands::CommandStatus::NoChange,
                        "an identical range is a no-op");
    expectations.expect(fixture.preparationCount.load() == beforeNoop,
                        "a no-op range edit evaluates nothing");

    // Clear widens to the whole composition [0,7): frames 0 and 6 enter.
    commands::Transaction clear("Clear Work Area", fixture.session.snapshot().revision());
    clear.emplace<commands::ClearWorkArea>(fixture.session.compositionId());
    expectations.expect(fixture.session.executeTransaction(std::move(clear)).changed(),
                        "clearing the work area publishes");
    const auto beforeClear = fixture.preparationCount.load();
    ram.start();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) && ram.cachedFrameCount() == 7,
                        "the cleared range covers the whole composition");
    expectations.expect(fixture.preparationCount.load() == beforeClear + 3,
                        "clearing fills only the frames outside the previous range");

    // Undo restores the narrower range and drops the frames it excluded, still as range drops.
    const auto dropsBeforeUndo = fixture.frameCache->statistics().rangeDrops;
    const auto evictionsBeforeUndo = fixture.frameCache->statistics().evictions;
    expectations.expect(fixture.session.undo(), "the clear undoes");
    const auto key0 = fixture.controller.cacheKeyForTime(time(0, 25));
    const auto key6 = fixture.controller.cacheKeyForTime(time(6, 25));
    expectations.expect(key0 && key6 && !fixture.frameCache->contains(*key0) &&
                            !fixture.frameCache->contains(*key6) &&
                            fixture.frameCache->statistics().rangeDrops > dropsBeforeUndo &&
                            fixture.frameCache->statistics().evictions == evictionsBeforeUndo,
                        "undoing the clear prunes the re-excluded frames without an eviction");

    // A pixel edit still advances evaluation and invalidates.
    const auto nodeId = fixture.session.composition()->graph().nodes().front().id;
    commands::Transaction mute("Mute Node", fixture.session.snapshot().revision());
    mute.emplace<commands::SetNodeMuted>(fixture.session.compositionId(), nodeId, true);
    expectations.expect(fixture.session.executeTransaction(std::move(mute)).changed() &&
                            fixture.session.evaluationSnapshot().revision() ==
                                fixture.session.snapshot().revision(),
                        "a pixel edit still advances the evaluation snapshot");

    ram.beginShutdown();
    finishFixture(fixture, expectations);
}

// WORKAREA-1: a range edit during an active RAM run adapts the run rather than ending it. The
// in-flight frame is allowed to land, the run rebases onto the new range, and an out-of-range
// completion is neither retained nor counted, so progress cannot skip or duplicate a frame.
void testRamRunAdaptsToRangeEditWhileActive(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM Active Range Edit", time(7, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the foreground frame is ready");
    const auto setRange = [&](const core::RationalTime start, const core::RationalTime end) {
        commands::Transaction range("Set Work Area", fixture.session.snapshot().revision());
        range.emplace<commands::SetWorkArea>(fixture.session.compositionId(), start, end);
        return fixture.session.executeTransaction(std::move(range));
    };
    expectations.expect(setRange(time(0, 25), time(7, 25)).changed(), "the whole range is set");
    fixture.frameCache->clear();
    const auto evalRevision = fixture.session.evaluationSnapshot().revision();

    // Pause the run on its first uncached frame.
    fixture.gateAtCall = fixture.preparationCount.load();
    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge, fixture.countingPipeline());
    ram.start();
    expectations.expect(waitUntil([&] { return fixture.gate.entered(); }) && ram.isCaching(),
                        "the RAM run is in flight");

    // Shrink to [2,4) while the frame is in flight.
    expectations.expect(setRange(time(2, 25), time(4, 25)).changed(), "the range shrinks mid-run");
    expectations.expect(fixture.session.evaluationSnapshot().revision() == evalRevision,
                        "the range edit did not advance evaluation");
    fixture.gate.release();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) &&
                            ram.cachedFrameCount() == 2 && ram.totalFrameCount() == 2,
                        "the run adapts to the new two-frame range and completes");
    for (std::int64_t frame = 0; frame < 7; ++frame) {
        const auto key = fixture.controller.cacheKeyForTime(time(frame, 25));
        const bool inRange = frame >= 2 && frame < 4;
        expectations.expect(key && fixture.frameCache->contains(*key) == inRange,
                            "only in-range frames remain cached after the mid-run shrink");
    }
    expectations.expect(ram.cachedFrameCount() == fixture.controller.frameCache().size(),
                        "no out-of-range frame is counted as progress");

    ram.beginShutdown();
    finishFixture(fixture, expectations);
}

// TEMPORAL-2B: a RAM run over a finite clip-range edit re-derives only the changed frames, keeps
// the retained ones, and the timeline markers span the multiple genuine revisions that result.
void testRamRunFiniteClipRange(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM Finite Clip Range", time(7, 25)));
    expectations.expect(animateSolidLayer(fixture.session),
                        "the finite-range fixture is animated across its seven frames");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the foreground frame is ready");
    const auto layerId = fixture.session.composition()->graph().layerOutputs().front().layerId;
    fixture.frameCache->clear();
    ui::RamPreviewController ram(fixture.session, fixture.controller, fixture.scheduler,
                                 fixture.bridge, fixture.countingPipeline());
    ram.start();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) && ram.cachedFrameCount() == 7,
                        "the whole seven-frame range caches");
    const auto afterFullFill = fixture.preparationCount.load();
    const auto oldRevision = fixture.session.snapshot().revision();

    // Trim to [0,3): frames 3..6 lose the layer; 0..2 are unchanged.
    commands::Transaction trim("Trim", fixture.session.snapshot().revision());
    trim.emplace<commands::SetLayerRange>(fixture.session.compositionId(), layerId,
                                          core::RationalTime{}, time(3, 25));
    expectations.expect(fixture.session.executeTransaction(std::move(trim)).changed(),
                        "the trim publishes");
    expectations.expect(fixture.session.evaluationSnapshotForTime(time(0, 25)).revision() !=
                            fixture.session.snapshot().revision(),
                        "the retained overlap keeps an older revision");
    ram.start();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) &&
                            ram.cachedFrameCount() == 7 &&
                            fixture.preparationCount.load() == afterFullFill + 4,
                        "only the four invalidated frames re-prepare");
    for (std::int64_t frame = 0; frame < 7; ++frame) {
        const auto key = fixture.controller.cacheKeyForTime(time(frame, 25));
        expectations.expect(key && fixture.frameCache->contains(*key),
                            "every frame of the range is retained after the trim");
    }
    // Markers span both revisions: a probe from the OLD retained revision still reports the frames
    // re-derived under the new one.
    const auto probe = fixture.controller.cacheKeyForTime(time(0, 25));
    expectations.expect(probe.has_value() && probe->sourceRevision == oldRevision &&
                            fixture.frameCache->timesFor(*probe).size() == 7,
                        "timesFor spans multiple retained revisions");

    ram.beginShutdown();
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
        testDefaultBudgetFollowsPhysicalMemory(expectations);
        testOperationCacheUnderRamPreview(expectations);
        testRamPreviewSharesResolutionAndCachesByPolicy(expectations);
        testResolutionChangeCancelsAnActiveRamPreview(expectations);
        testCompiledPlanCacheCompilesOncePerRevision(expectations);
        testCompiledPlanCacheBindsSnapshotIdentity(expectations);
        testCacheHitPublishesWithoutEvaluating(expectations);
        testCachingReleasesTheProcessImage(expectations);
        testFrameCacheEvictsUnderBudgetAndDropsStaleRevisions(expectations);
        testRamPreviewWorkArea(expectations);
        testRamPreviewCachesTheRangeThenPlaysEveryFrame(expectations);
        testRamPreviewStopsWhenTheRangeOutgrowsTheBudget(expectations);
        testRamPreviewCancellationKeepsWhatItCached(expectations);
        testLayoutEditRetainsTheRamRun(expectations);
        testPixelEditCancelsTheRamRun(expectations);
        testWorkAreaRangeManagement(expectations);
        testRamRunAdaptsToRangeEditWhileActive(expectations);
        testRamRunFiniteClipRange(expectations);
        testRamRunReusesBothHalvesAfterSplit(expectations);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    return expectations.failures() == 0 ? 0 : 1;
}
