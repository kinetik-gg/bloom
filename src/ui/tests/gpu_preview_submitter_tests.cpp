// GPU-APP-1: the preview submit seam on all three preview controllers.
//
// These tests inject a submitter that performs the SAME real scheduler submission the controller
// would otherwise perform itself -- a genuine TaskHandle, a genuine CPU evaluation -- and prove
// three things without fabricating a GPU success: that the foreground, RAM, and background
// controllers route through the submitter; that the controller-owned CPU preparation function is
// left untouched on the preview submit path (it remains the viewer-analysis path); and that the
// frame cache still answers a cached request with no submission at all. A final case with no
// submitter pins the unchanged CPU legacy path.
//
// Nothing here needs a GPU or a loader. The device-dependent application smoke/bench is an
// integration step and is deliberately not claimed by this preparation.

#include <bloom/commands/command_stack.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
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
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
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
    while (timer.elapsed() < 15'000) {
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

// 25 fps, so one frame is exactly 40 ms and the times below land on frame boundaries; a tiny
// extent keeps a whole range of real evaluation cheap.
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

// The real compile/evaluate/display pipeline every submitter below delegates to, so a routed
// request produces a genuine display-only frame rather than a fabricated success.
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

// Counters shared by the submitter and the controller-owned preparation function, so a test can
// say which of the two the controller actually used.
struct SubmitProbe final {
    std::atomic<int> submitted{0};
    std::atomic<int> legacy{0};
    std::atomic<std::uint64_t> lastTaskId{0};
    std::atomic<bool> lastAccepted{false};
};

// A submitter shaped exactly like a runtime service's own submit: it calls the real scheduler and
// returns that submission's genuine TaskHandle.
[[nodiscard]] ui::PreviewPreparationSubmitter
makeSubmitter(runtime::TaskScheduler& scheduler, const ui::PreviewPreparationFunction& pipeline,
              SubmitProbe& probe) {
    return [&scheduler, pipeline,
            &probe](runtime::TaskRequest request, const document::Snapshot& snapshot,
                    const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                    const std::vector<runtime::SnapshotParameterOverride>& overrides) {
        probe.submitted.fetch_add(1);
        auto submission = scheduler.submit<ui::PreviewPreparationResultHandle>(
            std::move(request),
            [snapshot, identity, limit, overrides, pipeline](runtime::TaskContext& context) {
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<ui::PreviewPreparationResultHandle>::cancelled();
                }
                return pipeline(snapshot, identity, limit, overrides, context);
            });
        if (submission.accepted()) {
            probe.lastAccepted.store(true);
            probe.lastTaskId.store(submission.handle.id().value());
        }
        return submission;
    };
}

[[nodiscard]] ui::PreviewPreparationFunction
countingPreparation(const ui::PreviewPreparationFunction& pipeline, SubmitProbe& probe) {
    return
        [pipeline, &probe](const document::Snapshot& snapshot,
                           const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                           const std::vector<runtime::SnapshotParameterOverride>& overrides,
                           runtime::TaskContext& context) {
            probe.legacy.fetch_add(1);
            return pipeline(snapshot, identity, limit, overrides, context);
        };
}

[[nodiscard]] bool isReady(const ui::CompositionPreviewController& controller) {
    return controller.state().activity == ui::PreviewActivity::Ready;
}

void finish(runtime::TaskScheduler& scheduler, ui::TaskUiBridge& bridge, Expectations& expectations,
            const std::initializer_list<std::function<void()>>& shutdowns) {
    for (const auto& shutdown : shutdowns) {
        shutdown();
    }
    bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                        "the preview fixture reaches asynchronous scheduler quiescence");
}

// The foreground controller must hand its preview request to the injected submitter; the
// controller's own CPU preparation function stays the viewer-analysis path and is not used here.
void testForegroundRoutesThroughSubmitter(Expectations& expectations) {
    auto newProject = makeTestProject("Foreground submitter", time(3, 25));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    SubmitProbe probe;
    auto frameCache = std::make_shared<ui::PreviewFrameCache>();
    ui::CompositionPreviewController controller(
        session, scheduler, bridge, countingPreparation(fixture.pipeline, probe), {}, frameCache,
        nullptr, makeSubmitter(scheduler, fixture.pipeline, probe));

    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the foreground frame becomes ready through the submitter");
    expectations.expect(probe.submitted.load() >= 1,
                        "the foreground preview request was routed to the submitter");
    expectations.expect(probe.legacy.load() == 0,
                        "the controller-owned CPU preparation is not used for preview submission");
    expectations.expect(controller.state().frame != nullptr &&
                            controller.state().frame->displayBufferView().has_value(),
                        "the routed request produced a real evaluated display frame");
    const auto rawTaskId = probe.lastTaskId.load();
    expectations.expect(
        rawTaskId != 0 && scheduler.snapshot(runtime::TaskId::fromRaw(rawTaskId)).has_value(),
        "the submitter returned a genuine scheduler TaskHandle for the controller to gate on");

    finish(scheduler, bridge, expectations, {[&] { controller.beginShutdown(); }});
}

// The RAM preview controller must route every cached-range frame through the same submitter.
void testRamPreviewRoutesThroughSubmitter(Expectations& expectations) {
    auto newProject = makeTestProject("RAM submitter", time(3, 25));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    SubmitProbe probe;
    auto frameCache = std::make_shared<ui::PreviewFrameCache>();
    ui::CompositionPreviewController controller(
        session, scheduler, bridge, countingPreparation(fixture.pipeline, probe), {}, frameCache,
        nullptr, makeSubmitter(scheduler, fixture.pipeline, probe));
    ui::RamPreviewController ramPreview(session, controller, scheduler, bridge,
                                        countingPreparation(fixture.pipeline, probe), nullptr,
                                        makeSubmitter(scheduler, fixture.pipeline, probe));

    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the foreground frame settles before the RAM run");
    ramPreview.start();
    expectations.expect(ramPreview.isCaching(), "the RAM preview run starts");
    expectations.expect(waitUntil([&] { return !ramPreview.isCaching(); }),
                        "the RAM preview run completes");
    expectations.expect(probe.submitted.load() >= 1 && probe.legacy.load() == 0,
                        "every RAM preview frame was routed through the submitter");
    expectations.expect(ramPreview.totalFrameCount() > 0 &&
                            ramPreview.cachedFrameCount() == ramPreview.totalFrameCount() &&
                            frameCache->size() >= 1,
                        "the routed RAM run cached the whole range");

    finish(scheduler, bridge, expectations,
           {[&] { ramPreview.beginShutdown(); }, [&] { controller.beginShutdown(); }});
}

// The background (speculative) controller must route through the submitter too, and its completed
// frame must still enter the shared frame cache.
void testBackgroundRoutesThroughSubmitter(Expectations& expectations) {
    auto newProject = makeTestProject("Background submitter", time(3, 25));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    SubmitProbe probe;
    auto frameCache = std::make_shared<ui::PreviewFrameCache>();
    ui::CompositionPreviewController controller(
        session, scheduler, bridge, countingPreparation(fixture.pipeline, probe), {}, frameCache,
        nullptr, makeSubmitter(scheduler, fixture.pipeline, probe));
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the foreground frame settles before background fill");
    const int submittedBefore = probe.submitted.load();

    ui::BackgroundPreviewController background(
        session, controller, scheduler, bridge, countingPreparation(fixture.pipeline, probe),
        nullptr, makeSubmitter(scheduler, fixture.pipeline, probe));
    background.fillNextFrame();
    expectations.expect(waitUntil([&] { return frameCache->size() >= 1; }),
                        "the background fill caches a frame");
    expectations.expect(probe.submitted.load() > submittedBefore && probe.legacy.load() == 0,
                        "the background fill was routed through the submitter");

    finish(scheduler, bridge, expectations,
           {[&] { background.beginShutdown(); }, [&] { controller.beginShutdown(); }});
}

// The frame cache answers a cached time with no submission at all: the same controller returns to a
// time the background controller already filled and is served from the cache instead of the
// submitter.
void testCachedRequestBypassesSubmitter(Expectations& expectations) {
    auto newProject = makeTestProject("Submitter cache hit", time(3, 25));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    SubmitProbe probe;
    auto frameCache = std::make_shared<ui::PreviewFrameCache>();
    ui::CompositionPreviewController controller(
        session, scheduler, bridge, countingPreparation(fixture.pipeline, probe), {}, frameCache,
        nullptr, makeSubmitter(scheduler, fixture.pipeline, probe));
    ui::BackgroundPreviewController background(
        session, controller, scheduler, bridge, countingPreparation(fixture.pipeline, probe),
        nullptr, makeSubmitter(scheduler, fixture.pipeline, probe));
    expectations.expect(waitUntil([&] { return isReady(controller); }), "opening frame ready");

    background.fillNextFrame();
    expectations.expect(waitUntil([&] {
                            const auto key = controller.cacheKeyForTime(time(0));
                            return key.has_value() && frameCache->contains(*key);
                        }),
                        "the background controller caches the opening frame");
    background.beginShutdown();

    expectations.expect(session.setCurrentTime(time(1, 25)) &&
                            waitUntil([&] { return isReady(controller); }),
                        "a different time is evaluated through the submitter");
    const int afterEvaluation = probe.submitted.load();
    expectations.expect(afterEvaluation >= 2, "the uncached time submitted a real evaluation");

    expectations.expect(session.setCurrentTime(time(0)) &&
                            waitUntil([&] { return isReady(controller); }),
                        "returning to the cached time publishes a frame");
    expectations.expect(probe.submitted.load() == afterEvaluation,
                        "a cached time is answered with no submitter call");
    expectations.expect(controller.state().freshness == ui::FrameFreshness::Current &&
                            controller.state().frame != nullptr,
                        "the cache-served frame is the current display frame");

    finish(scheduler, bridge, expectations, {[&] { controller.beginShutdown(); }});
}

// With no submitter the controller keeps its exact CPU legacy path: it runs its own preparation
// function on its own scheduler submission.
void testLegacyPathWithoutSubmitter(Expectations& expectations) {
    auto newProject = makeTestProject("Legacy path", time(3, 25));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    SubmitProbe probe;
    auto frameCache = std::make_shared<ui::PreviewFrameCache>();
    ui::CompositionPreviewController controller(
        session, scheduler, bridge, countingPreparation(fixture.pipeline, probe), {}, frameCache);

    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the untouched CPU path still prepares the opening frame");
    expectations.expect(probe.legacy.load() >= 1 && probe.submitted.load() == 0,
                        "without a submitter the controller runs its own CPU preparation");
    expectations.expect(controller.state().frame != nullptr &&
                            controller.state().frame->displayBufferView().has_value(),
                        "the legacy path still publishes a real display frame");

    finish(scheduler, bridge, expectations, {[&] { controller.beginShutdown(); }});
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    try {
        testForegroundRoutesThroughSubmitter(expectations);
        testRamPreviewRoutesThroughSubmitter(expectations);
        testBackgroundRoutesThroughSubmitter(expectations);
        testCachedRequestBypassesSubmitter(expectations);
        testLegacyPathWithoutSubmitter(expectations);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return expectations.failures() == 0 ? 0 : 1;
}
