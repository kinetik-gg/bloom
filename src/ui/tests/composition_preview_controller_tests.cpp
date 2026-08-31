#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
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
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <ranges>
#include <source_location>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using PipelineResult = bloom::runtime::TaskResult<bloom::ui::PreviewPreparationResultHandle>;

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

template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 4'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::invoke(predicate)) {
            return true;
        }
        std::this_thread::yield();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return std::invoke(predicate);
}

bloom::runtime::TaskSchedulerConfig testSchedulerConfig() {
    return {.cpuWorkerCount = 1,
            .blockingIoWorkerCount = 1,
            .cpuQueueCapacity = 16,
            .blockingIoQueueCapacity = 4,
            .terminalHistoryCapacity = 32,
            .diagnosticsPerTask = 8,
            .groupRegistryCapacity = 8};
}

bloom::document::CompositionFormat smallFormat() {
    const auto format = bloom::document::CompositionFormat::create(4, 3);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
}

bloom::document::NewProject makeTestProject(std::string projectName) {
    return bloom::document::makeNewProject(
        std::move(projectName), "Main", bloom::core::RationalTime::fromInteger(10), smallFormat());
}

bloom::document::Composition makeSecondComposition() {
    using namespace bloom::document;
    const auto compositionId = CompositionId::fromRaw(2);
    const auto stackId = NodeId::fromRaw(100);
    const auto outputId = NodeId::fromRaw(101);
    const auto edgeId = EdgeId::fromRaw(100);
    CanonicalGraph graph(stackId);
    const bool built =
        graph.addNode(
            {stackId, std::string(kLayerStackNodeType), {}, kLayerStackNodeSchemaVersion}) &&
        graph.addNode({outputId,
                       std::string(kCompositionOutputNodeType),
                       {},
                       kCompositionOutputNodeSchemaVersion}) &&
        graph.addEdge({edgeId,
                       {stackId, std::string(kLayerStackOutputPort)},
                       NodeInputRef{outputId, std::string(kCompositionOutputInputPort)}});
    graph.setCompositionOutput({outputId, std::string(kCompositionOutputOutputPort)});
    if (!built) {
        std::abort();
    }
    return Composition(compositionId, "Second", bloom::core::RationalTime::fromInteger(10),
                       std::move(graph), smallFormat());
}

struct PipelineFixture final {
    bloom::runtime::NodeDefinitionRegistry definitions;
    bloom::runtime::SnapshotCompiler compiler;
    bloom::runtime::CpuCompositionEvaluator evaluator;
    bloom::runtime::CpuReferenceDisplayPreparer displayPreparer;
    // Pending by default (issue #97, task C3): a test that specifically exercises qualified-display
    // readiness/failure builds its own provider and publishes to it directly (see
    // testQualifiedDisplayReadinessAndFailClosed below) rather than sharing this fixture's, which
    // every other test in this file relies on staying on the unchanged reference path.
    bloom::runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    bloom::ui::PreviewPreparationFunction pipeline;

    PipelineFixture() : compiler(definitions) {
        if (!bloom::runtime::registerBuiltInNodeDefinitions(definitions)) {
            std::abort();
        }
        definitions.freeze();
        pipeline = bloom::ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                             qualifiedProcessorProvider);
    }
};

std::uint64_t generation(const bloom::ui::CompositionPreviewState& state) {
    return state.desiredIdentity.has_value() ? state.desiredIdentity->requestGeneration : 0;
}

bool isReady(const bloom::ui::CompositionPreviewController& controller) {
    return controller.state().activity == bloom::ui::PreviewActivity::Ready;
}

void reachQuiescence(bloom::ui::CompositionPreviewController& controller,
                     bloom::ui::TaskUiBridge& bridge, bloom::runtime::TaskScheduler& scheduler,
                     Expectations& expectations) {
    bool quiescentSignal = false;
    QObject::connect(&bridge, &bloom::ui::TaskUiBridge::shutdownQuiescent, &bridge,
                     [&quiescentSignal] { quiescentSignal = true; });
    controller.beginShutdown();
    bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return quiescentSignal && scheduler.isQuiescent(); }),
                        "preview fixture reaches asynchronous scheduler quiescence");
}

void testRevisionAndPanelSuppression(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Preview Test");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    WorkerGate firstRequest;
    std::atomic<int> invocationCount = 0;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&firstRequest, &invocationCount, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::optional<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) mutable {
            if (invocationCount.fetch_add(1) == 0) {
                firstRequest.enterAndWait();
            }
            return pipeline(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride,
                            context);
        });
    auto* viewer = new ui::ViewerEditor(session, controller);

    expectations.expect(waitUntil([&] { return firstRequest.entered(); }),
                        "first revision starts on the worker");
    const auto oldGeneration = generation(controller.state());
    std::vector<std::uint64_t> publishedReadyGenerations;
    QObject::connect(&controller, &ui::CompositionPreviewController::stateChanged, &controller,
                     [&] {
                         if (isReady(controller)) {
                             publishedReadyGenerations.push_back(generation(controller.state()));
                         }
                     });

    expectations.expect(
        session.addSolidLayer(QStringLiteral("Revision Solid"), core::Color4d{0.2, 0.4, 0.8, 1.0}),
        "document command creates a newer preview revision");
    const auto currentGeneration = generation(controller.state());
    expectations.expect(currentGeneration > oldGeneration &&
                            controller.state().activity == ui::PreviewActivity::Rendering &&
                            controller.state().freshness == ui::FrameFreshness::None,
                        "new revision immediately supersedes the old request without stale pixels");

    delete viewer;
    firstRequest.release();
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "current revision becomes ready after the replaced Viewer is destroyed");
    expectations.expect(controller.state().desiredIdentity.has_value() &&
                            controller.state().desiredIdentity->sourceRevision ==
                                session.snapshot().revision(),
                        "published frame identifies the active document revision");
    expectations.expect(!publishedReadyGenerations.empty() &&
                            std::ranges::all_of(publishedReadyGenerations,
                                                [currentGeneration](const auto value) {
                                                    return value == currentGeneration;
                                                }),
                        "the obsolete revision never publishes Ready");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

void testNewestPendingRequestGate(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Preview Gate Test");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    WorkerGate firstRequest;
    std::mutex invocationMutex;
    std::vector<std::uint64_t> invokedGenerations;
    std::atomic<int> inFlight = 0;
    std::atomic<int> maximumInFlight = 0;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&firstRequest, &invocationMutex, &invokedGenerations, &inFlight, &maximumInFlight,
         pipeline = fixture.pipeline](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::optional<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) mutable {
            const int concurrent = inFlight.fetch_add(1) + 1;
            int previousMaximum = maximumInFlight.load();
            while (concurrent > previousMaximum &&
                   !maximumInFlight.compare_exchange_weak(previousMaximum, concurrent)) {
            }
            std::size_t invocationIndex = 0;
            {
                std::scoped_lock lock(invocationMutex);
                invocationIndex = invokedGenerations.size();
                invokedGenerations.push_back(desiredIdentity.requestGeneration);
            }
            if (invocationIndex == 0) {
                firstRequest.enterAndWait();
            }
            auto result = pipeline(snapshot, desiredIdentity, pixelStorageByteLimit,
                                   interactionOverride, context);
            --inFlight;
            return result;
        });

    expectations.expect(waitUntil([&] { return firstRequest.entered(); }),
                        "the active preview enters preparation before the request storm");
    const auto activeGeneration = generation(controller.state());
    std::vector<std::uint64_t> requestedGenerations;
    for (int request = 0; request < 6; ++request) {
        controller.requestRefresh();
        requestedGenerations.push_back(generation(controller.state()));
    }
    const auto newestGeneration = requestedGenerations.back();

    expectations.expect(
        controller.state().activity == ui::PreviewActivity::Rendering &&
            controller.state().desiredIdentity.has_value() &&
            controller.state().desiredIdentity->requestGeneration == newestGeneration &&
            !controller.state().taskId.has_value(),
        "the desired identity advances immediately while the newest request remains pending");
    expectations.expect(
        scheduler.snapshots().size() == 1,
        "the controller retains one scheduler submission while an active task gates");
    {
        std::scoped_lock lock(invocationMutex);
        expectations.expect(invokedGenerations == std::vector{activeGeneration},
                            "pending request replacement does not invoke preparation");
    }

    firstRequest.release();
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the newest pending preview starts after the active task reaches terminal");
    {
        std::scoped_lock lock(invocationMutex);
        expectations.expect(invokedGenerations == std::vector{activeGeneration, newestGeneration},
                            "only the active and newest pending generations invoke preparation");
        expectations.expect(
            std::ranges::none_of(requestedGenerations |
                                     std::views::take(requestedGenerations.size() - 1),
                                 [&invokedGenerations](const auto intermediate) {
                                     return std::ranges::find(invokedGenerations, intermediate) !=
                                            invokedGenerations.end();
                                 }),
            "intermediate pending generations never invoke preparation");
    }
    expectations.expect(maximumInFlight.load() == 1,
                        "preview preparation never overlaps across the request gate");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

std::optional<bloom::runtime::TaskSnapshot>
snapshotForGeneration(const bloom::runtime::TaskScheduler& scheduler,
                      const std::uint64_t generation) {
    for (const auto& snapshot : scheduler.snapshots()) {
        if (snapshot.sourceVersion.requestGeneration == generation) {
            return snapshot;
        }
    }
    return std::nullopt;
}

// docs/architecture/animation-and-time.md, "Session Time And Scrubbing": a burst of Interactive
// requests inside the injectable trailing cadence window coalesces to only the newest, submitted
// once the window elapses; Visible requests bypass the cadence entirely.
void testInteractiveCadenceCoalescesBurstAndVisibleBypasses(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Interactive Cadence Test");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    std::atomic<int> invocationCount = 0;
    ui::CompositionPreviewSettings settings;
    settings.interactiveTrailingCadence = 40ms;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&invocationCount, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::optional<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) mutable {
            ++invocationCount;
            return pipeline(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride,
                            context);
        },
        settings);

    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "initial frame becomes ready before the cadence burst");
    const auto callsBeforeBurst = invocationCount.load();

    controller.beginInteractiveScrub();
    std::vector<core::RationalTime> times;
    for (std::int64_t numerator = 1; numerator <= 6; ++numerator) {
        const auto time = core::RationalTime::create(numerator, 100);
        expectations.expect(time.has_value(), "burst time fixture is valid");
        times.push_back(time.value_or(core::RationalTime{}));
    }
    for (const auto& time : times) {
        expectations.expect(session.setCurrentTime(time),
                            "each distinct burst time is accepted by the session");
    }
    const auto newestGeneration = generation(controller.state());
    expectations.expect(controller.state().desiredIdentity.has_value() &&
                            controller.state().desiredIdentity->time == times.back(),
                        "the desired identity advances immediately to the newest scrub time");
    expectations.expect(invocationCount.load() == callsBeforeBurst,
                        "no preparation call fires before the trailing cadence window elapses");

    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the coalesced newest Interactive request completes");
    expectations.expect(invocationCount.load() == callsBeforeBurst + 1,
                        "the entire burst submits only once, for the newest request");
    const auto interactiveSnapshot = snapshotForGeneration(scheduler, newestGeneration);
    expectations.expect(interactiveSnapshot.has_value() &&
                            interactiveSnapshot->priority == runtime::TaskPriority::Interactive,
                        "the coalesced burst submits at Interactive priority");

    // A Visible request (discrete typed time entry, key selection, or document refresh) bypasses
    // the cadence entirely, even while a scrub gesture is still armed.
    const auto callsBeforeVisible = invocationCount.load();
    controller.requestRefresh();
    const auto visibleGeneration = generation(controller.state());
    expectations.expect(waitUntil([&] { return invocationCount.load() > callsBeforeVisible; }),
                        "a Visible request submits without waiting for any cadence window");
    const auto visibleSnapshot = snapshotForGeneration(scheduler, visibleGeneration);
    expectations.expect(visibleSnapshot.has_value() &&
                            visibleSnapshot->priority == runtime::TaskPriority::Visible,
                        "the Visible request submits at Visible priority");

    controller.notifyScrubEnded();
    reachQuiescence(controller, bridge, scheduler, expectations);
}

// The one-active/one-newest gate is untouched beneath the cadence: while a task is active, an
// Interactive burst never invokes preparation again; the superseded active request still runs to
// terminal before the newest pending request submits, and notifyScrubEnded() bypasses the
// remaining trailing delay once the gate opens (docs/architecture/animation-and-time.md).
void testActiveGateHoldsAndScrubEndBypassesRemainingCadence(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Interactive Gate Test");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    WorkerGate firstRequest;
    std::atomic<int> invocationCount = 0;
    ui::CompositionPreviewSettings settings;
    // A long cadence window proves notifyScrubEnded() bypasses it rather than merely outlasting it.
    settings.interactiveTrailingCadence = 2000ms;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&firstRequest, &invocationCount, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::optional<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) mutable {
            if (invocationCount.fetch_add(1) == 0) {
                firstRequest.enterAndWait();
            }
            return pipeline(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride,
                            context);
        },
        settings);

    expectations.expect(waitUntil([&] { return firstRequest.entered(); }),
                        "the initial Visible request occupies the active-request gate");

    controller.beginInteractiveScrub();
    std::vector<core::RationalTime> times;
    for (std::int64_t numerator = 1; numerator <= 4; ++numerator) {
        const auto time = core::RationalTime::create(numerator, 100);
        expectations.expect(time.has_value(), "gate burst time fixture is valid");
        times.push_back(time.value_or(core::RationalTime{}));
    }
    for (const auto& time : times) {
        expectations.expect(session.setCurrentTime(time), "each gated burst time is accepted");
    }
    const auto newestGeneration = generation(controller.state());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    expectations.expect(invocationCount.load() == 1,
                        "an Interactive burst behind the active gate never invokes preparation");
    expectations.expect(controller.state().activity == ui::PreviewActivity::Rendering &&
                            !controller.state().taskId.has_value(),
                        "the newest pending Interactive request is not yet submitted");

    firstRequest.release();
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the pending Interactive request submits once the active task terminates");
    expectations.expect(invocationCount.load() == 2,
                        "the superseded active request ran to terminal before the pending request "
                        "submitted, and only the newest pending request was ever prepared");
    const auto interactiveSnapshot = snapshotForGeneration(scheduler, newestGeneration);
    expectations.expect(interactiveSnapshot.has_value() &&
                            interactiveSnapshot->priority == runtime::TaskPriority::Interactive,
                        "the gate-held request still submits at Interactive priority");

    // Now that nothing is active, a fresh Interactive request genuinely starts the (long) trailing
    // cadence timer; notifyScrubEnded() must bypass it rather than merely outlast it.
    const auto callsBeforeSecondScrub = invocationCount.load();
    const auto secondTime = core::RationalTime::create(37, 100);
    expectations.expect(secondTime.has_value() && session.setCurrentTime(*secondTime),
                        "a fresh post-gate scrub time is accepted");
    expectations.expect(invocationCount.load() == callsBeforeSecondScrub,
                        "the fresh Interactive request is held by the 2 second cadence, not yet "
                        "submitted");
    QElapsedTimer bypassTimer;
    bypassTimer.start();
    controller.notifyScrubEnded();
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "notifyScrubEnded() flushes the pending Interactive request");
    expectations.expect(bypassTimer.elapsed() < 1000,
                        "the flush happens immediately, far under the 2 second cadence window");
    expectations.expect(invocationCount.load() == callsBeforeSecondScrub + 1,
                        "exactly one additional preparation call services the bypassed request");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

void testSameRevisionGenerationAndSelection(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Generation Test");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    std::atomic<int> invocationCount = 0;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&invocationCount, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::optional<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) mutable {
            ++invocationCount;
            return pipeline(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride,
                            context);
        });

    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "initial same-revision frame becomes ready");
    const auto halfway = core::RationalTime::create(1, 2);
    if (!halfway.has_value()) {
        expectations.expect(false, "session time fixture is valid");
        reachQuiescence(controller, bridge, scheduler, expectations);
        return;
    }
    const auto revisionBeforeTime = session.snapshot().revision();
    const auto generationBeforeTime = generation(controller.state());
    expectations.expect(session.setCurrentTime(*halfway) &&
                            generation(controller.state()) > generationBeforeTime &&
                            controller.state().desiredIdentity.has_value() &&
                            controller.state().desiredIdentity->time == *halfway &&
                            session.snapshot().revision() == revisionBeforeTime,
                        "session time advances preview intent without dirtying the document");
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "the exact session time reaches the prepared frame");
    const auto callsAfterTime = invocationCount.load();
    const auto generationAfterTime = generation(controller.state());
    expectations.expect(!session.setCurrentTime(*halfway),
                        "setting the same exact session time is a no-op");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    expectations.expect(invocationCount.load() == callsAfterTime &&
                            generation(controller.state()) == generationAfterTime,
                        "an unchanged session time creates no preview churn");

    const auto firstFrame = controller.state().frame;
    if (!controller.state().desiredIdentity.has_value()) {
        expectations.expect(false, "desired identity is populated once the preview is ready");
        reachQuiescence(controller, bridge, scheduler, expectations);
        return;
    }
    const auto revision = controller.state().desiredIdentity->sourceRevision;
    const auto firstGeneration = generation(controller.state());
    controller.requestRefresh();
    const auto secondGeneration = generation(controller.state());
    expectations.expect(controller.state().desiredIdentity.has_value() &&
                            controller.state().desiredIdentity->sourceRevision == revision &&
                            secondGeneration > firstGeneration &&
                            controller.state().activity == ui::PreviewActivity::Rendering &&
                            controller.state().freshness == ui::FrameFreshness::Stale &&
                            controller.state().frame == firstFrame,
                        "same-revision refresh marks the retained frame stale");
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "replacement same-revision generation completes");
    expectations.expect(generation(controller.state()) == secondGeneration &&
                            controller.state().freshness == ui::FrameFreshness::Current,
                        "only the desired generation becomes current");

    const auto callsBeforeSelection = invocationCount.load();
    const auto generationBeforeSelection = generation(controller.state());
    session.clearSelection();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    expectations.expect(invocationCount.load() == callsBeforeSelection &&
                            generation(controller.state()) == generationBeforeSelection,
                        "selection-only changes never request a frame");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

void testLastGoodAndOutcomeMapping(Expectations& expectations) {
    using namespace bloom;
    enum class Outcome { Prepared, SlowFailed, Unsupported, Cancelled, Failed, Mismatch };

    auto newProject = makeTestProject("Outcome Test");
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    WorkerGate slowFailure;
    std::atomic outcome = Outcome::Prepared;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&outcome, &slowFailure, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::optional<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) mutable {
            switch (outcome.load()) {
            case Outcome::Prepared:
                return pipeline(snapshot, desiredIdentity, pixelStorageByteLimit,
                                interactionOverride, context);
            case Outcome::SlowFailed:
                slowFailure.enterAndWait();
                return PipelineResult::failed({.code = "bloom.preview.test-slow-failure",
                                               .severity = runtime::DiagnosticSeverity::Error,
                                               .summary = "The slow proof render failed",
                                               .detail = {},
                                               .suggestedAction = {}});
            case Outcome::Unsupported: {
                auto unsupported = runtime::PreviewPreparationResult::unsupported();
                return PipelineResult::succeeded(
                    std::make_shared<const runtime::PreviewPreparationResult>(
                        std::move(unsupported)),
                    {{.code = "bloom.preview.test-unsupported",
                      .severity = runtime::DiagnosticSeverity::Error,
                      .summary = "The proof graph is unsupported",
                      .detail = {},
                      .suggestedAction = {}}});
            }
            case Outcome::Cancelled:
                return PipelineResult::cancelled();
            case Outcome::Failed:
                return PipelineResult::failed({.code = "bloom.preview.test-failure",
                                               .severity = runtime::DiagnosticSeverity::Error,
                                               .summary = "The proof render failed",
                                               .detail = {},
                                               .suggestedAction = {}});
            case Outcome::Mismatch: {
                auto mismatched = desiredIdentity;
                ++mismatched.requestGeneration;
                return pipeline(snapshot, mismatched, pixelStorageByteLimit, interactionOverride,
                                context);
            }
            }
            return PipelineResult::failed({.code = "bloom.preview.invalid-test-outcome",
                                           .severity = runtime::DiagnosticSeverity::Error,
                                           .summary = "Invalid proof outcome",
                                           .detail = {},
                                           .suggestedAction = {}});
        });

    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "initial prepared frame becomes current");
    const auto lastGood = controller.state().frame;
    expectations.expect(lastGood != nullptr &&
                            controller.state().freshness == ui::FrameFreshness::Current,
                        "Ready owns one current immutable frame");
    expectations.expect(
        lastGood != nullptr && lastGood->processFrame() != nullptr &&
            lastGood->displayFrame() != nullptr &&
            lastGood->displayIdentity().processFrame == lastGood->processIdentity() &&
            lastGood->displayBuffer().isValid(),
        "preview publication retains distinct immutable process and display products");

    outcome.store(Outcome::SlowFailed);
    controller.requestRefresh();
    expectations.expect(waitUntil([&] { return slowFailure.entered(); }),
                        "slow replacement enters worker code");
    expectations.expect(controller.state().activity == ui::PreviewActivity::Rendering &&
                            controller.state().freshness == ui::FrameFreshness::Stale &&
                            controller.state().frame == lastGood,
                        "Rendering retains and visibly marks the previous frame stale");
    bool heartbeat = false;
    QTimer::singleShot(0, &controller, [&heartbeat] { heartbeat = true; });
    expectations.expect(waitUntil([&] { return heartbeat; }),
                        "Qt event loop remains responsive while evaluation is blocked");
    slowFailure.release();
    expectations.expect(
        waitUntil([&] { return controller.state().activity == ui::PreviewActivity::Failed; }),
        "slow failure reaches a terminal state");
    expectations.expect(controller.state().frame == lastGood &&
                            controller.state().freshness == ui::FrameFreshness::Stale &&
                            controller.state().message.contains(QStringLiteral("slow proof")),
                        "Failed retains last-good pixels and diagnostic summary");

    outcome.store(Outcome::Unsupported);
    controller.requestRefresh();
    expectations.expect(
        waitUntil([&] { return controller.state().activity == ui::PreviewActivity::Unsupported; }),
        "semantic rejection maps to Unsupported");
    expectations.expect(controller.state().frame == lastGood &&
                            controller.state().freshness == ui::FrameFreshness::Stale,
                        "Unsupported retains last-good pixels");

    outcome.store(Outcome::Cancelled);
    controller.requestRefresh();
    expectations.expect(
        waitUntil([&] { return controller.state().activity == ui::PreviewActivity::Cancelled; }),
        "task cancellation maps to Cancelled");
    expectations.expect(controller.state().frame == lastGood &&
                            controller.state().freshness == ui::FrameFreshness::Stale,
                        "Cancelled retains last-good pixels");

    outcome.store(Outcome::Mismatch);
    controller.requestRefresh();
    expectations.expect(
        waitUntil([&] { return controller.state().activity == ui::PreviewActivity::Failed; }),
        "mismatched successful frame is rejected");
    expectations.expect(
        controller.state().frame == lastGood &&
            controller.state().message.contains(QStringLiteral("different request")),
        "identity mismatch never replaces last-good pixels");

    outcome.store(Outcome::Prepared);
    controller.requestRefresh();
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "a later valid request recovers Ready");
    expectations.expect(controller.state().frame != lastGood &&
                            controller.state().freshness == ui::FrameFreshness::Current,
                        "recovery atomically replaces the retained frame");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

void testCompositionSwitchClearsPixels(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Composition Switch Test");
    const auto firstCompositionId = newProject.initialCompositionId;
    const auto secondCompositionId = document::CompositionId::fromRaw(2);
    expectations.expect(newProject.project.addComposition(makeSecondComposition()),
                        "test project adds a second valid composition");
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, firstCompositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    ui::CompositionPreviewController controller(session, scheduler, bridge, fixture.pipeline);

    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "first composition frame becomes ready");
    const auto firstFrame = controller.state().frame;
    const auto nonzeroTime = core::RationalTime::create(3, 2);
    expectations.expect(nonzeroTime.has_value() && session.setCurrentTime(*nonzeroTime),
                        "composition switch fixture starts at a nonzero session time");
    expectations.expect(session.setComposition(secondCompositionId),
                        "session switches to the second composition");
    expectations.expect(controller.state().activity == ui::PreviewActivity::Rendering &&
                            controller.state().freshness == ui::FrameFreshness::None &&
                            controller.state().frame == nullptr &&
                            session.currentTime() == core::RationalTime::fromInteger(0) &&
                            controller.state().desiredIdentity.has_value() &&
                            controller.state().desiredIdentity->time ==
                                core::RationalTime::fromInteger(0),
                        "composition switch resets time and clears pixels in one preview intent");
    expectations.expect(waitUntil([&] { return isReady(controller); }),
                        "second composition frame becomes ready");
    expectations.expect(controller.state().frame != firstFrame &&
                            controller.state().frame->desiredIdentity().compositionId ==
                                secondCompositionId,
                        "only pixels for the active composition are published");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

// Issue #97 (task C3): before-readiness/after-readiness routing, stale rejection across the
// qualified path, and the fail-closed contract, using the SAME PipelineFixture-shaped setup as the
// rest of this file but with a caller-owned QualifiedDisplayProcessorProvider this test drives
// directly (PipelineFixture's own provider is deliberately never published to for every other test
// in this file -- see its comment).
void testQualifiedDisplayReadinessAndFailClosed(Expectations& expectations) {
    using namespace bloom;

    // --- Part 1: Pending -> reference-labeled, then Ready -> qualified-flagged with the correct
    // identity, and stale rejection still holds while a qualified frame is the retained one.
    {
        auto newProject = makeTestProject("Qualified Readiness Test");
        const auto compositionId = newProject.initialCompositionId;
        document::Document document(std::move(newProject.project));
        commands::CommandStack commands(document);
        ui::CompositionSession session(document, commands, compositionId);
        runtime::TaskScheduler scheduler(testSchedulerConfig());
        ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);

        runtime::NodeDefinitionRegistry definitions;
        expectations.expect(runtime::registerBuiltInNodeDefinitions(definitions),
                            "readiness fixture registers built-in node definitions");
        definitions.freeze();
        runtime::SnapshotCompiler compiler(definitions);
        const runtime::CpuCompositionEvaluator evaluator;
        const runtime::CpuReferenceDisplayPreparer displayPreparer;
        runtime::QualifiedDisplayProcessorProvider qualifiedProvider;
        auto pipeline = ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                           qualifiedProvider);
        ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline);

        expectations.expect(
            waitUntil([&] { return isReady(controller); }),
            "an initial frame becomes ready before the qualified processor is built");
        expectations.expect(controller.state().frame != nullptr &&
                                !controller.state().frame->isOcioQualified(),
                            "a frame prepared before readiness is reference-labeled, not qualified "
                            "-- the honest startup window, not a fallback from failure");
        const auto referenceFrame = controller.state().frame;

        auto resolution = color::resolveBloomNeutralV1BuiltIn(
            color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
            color::kBloomNeutralV1ConfigDigest);
        expectations.expect(resolution.ready(), "the embedded Bloom Neutral built-in resolves");
        auto resolved = std::move(resolution).takeResolved();
        expectations.expect(resolved.has_value(), "the resolution produces a usable product");
        if (resolved.has_value()) {
            auto built = color::buildBloomNeutralCpuDisplayProcessor(*resolved);
            expectations.expect(static_cast<bool>(built), "the qualified processor builds");
            if (built) {
                auto handleValue = std::move(built).takeHandle();
                expectations.expect(handleValue.has_value(), "the built result carries a handle");
                if (handleValue.has_value()) {
                    auto shared = std::make_shared<const color::PreparedCpuDisplayProcessorHandle>(
                        std::move(*handleValue));
                    qualifiedProvider.publish(
                        runtime::QualifiedDisplayProcessorBuildResult::ready(shared));
                }
            }
        }

        controller.requestRefresh();
        expectations.expect(waitUntil([&] {
                                return controller.state().activity == ui::PreviewActivity::Ready &&
                                       controller.state().frame != referenceFrame;
                            }),
                            "a request after readiness produces a new frame");
        const auto qualifiedFrame = controller.state().frame;
        expectations.expect(qualifiedFrame != nullptr && qualifiedFrame->isOcioQualified(),
                            "the frame prepared after readiness is qualified-flagged");
        expectations.expect(
            qualifiedFrame != nullptr && qualifiedFrame->qualifiedDisplayFrame() != nullptr &&
                qualifiedFrame->qualifiedDisplayFrame()->processFrame() != nullptr &&
                qualifiedFrame->processIdentity() ==
                    qualifiedFrame->qualifiedDisplayFrame()->processFrame()->identity(),
            "the qualified frame carries the correct process identity");
        expectations.expect(controller.state().freshness == ui::FrameFreshness::Current,
                            "the qualified frame is published as the current frame");

        // Stale rejection still holds across the qualified path: a new request immediately marks
        // the retained qualified frame Stale rather than dropping it, exactly as it would for a
        // reference frame -- CompositionPreviewController's own freshness contract is unchanged
        // (design decision 5).
        controller.requestRefresh();
        expectations.expect(controller.state().freshness == ui::FrameFreshness::Stale &&
                                controller.state().frame == qualifiedFrame,
                            "a newly-submitted request immediately marks the retained qualified "
                            "frame stale rather than discarding it");
        expectations.expect(
            waitUntil([&] { return controller.state().activity == ui::PreviewActivity::Ready; }),
            "the superseding request itself reaches Ready");
        expectations.expect(controller.state().frame->isOcioQualified() &&
                                controller.state().freshness == ui::FrameFreshness::Current,
                            "the request that supersedes a qualified frame is itself qualified");

        reachQuiescence(controller, bridge, scheduler, expectations);
    }

    // --- Part 2: fail-closed. A forced resolution failure retains last-good, surfaces a
    // diagnostic, and never substitutes the reference transform for a qualified request.
    {
        auto newProject = makeTestProject("Qualified Fail-Closed Test");
        const auto compositionId = newProject.initialCompositionId;
        document::Document document(std::move(newProject.project));
        commands::CommandStack commands(document);
        ui::CompositionSession session(document, commands, compositionId);
        runtime::TaskScheduler scheduler(testSchedulerConfig());
        ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);

        runtime::NodeDefinitionRegistry definitions;
        expectations.expect(runtime::registerBuiltInNodeDefinitions(definitions),
                            "fail-closed fixture registers built-in node definitions");
        definitions.freeze();
        runtime::SnapshotCompiler compiler(definitions);
        const runtime::CpuCompositionEvaluator evaluator;
        const runtime::CpuReferenceDisplayPreparer displayPreparer;
        runtime::QualifiedDisplayProcessorProvider qualifiedProvider;
        auto pipeline = ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                           qualifiedProvider);
        ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline);

        expectations.expect(waitUntil([&] { return isReady(controller); }),
                            "the honest startup window still produces a reference-labeled frame");
        const auto lastGood = controller.state().frame;
        expectations.expect(lastGood != nullptr && !lastGood->isOcioQualified(),
                            "the retained last-good frame is reference-labeled");

        // Design decision 4's forced-failure seam: an intentionally perturbed expected revision (an
        // all-zero digest, guaranteed to differ from the real embedded payload's digest) through
        // bloom::color::resolveBloomNeutralV1BuiltIn -- the registry API's own typed "Changed"
        // outcome -- rather than fabricating a diagnostic with no real registry call behind it.
        const auto perturbedResolution = color::resolveBloomNeutralV1BuiltIn(
            color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
            core::Sha256Digest{});
        expectations.expect(
            perturbedResolution.outcome() == color::OcioBuiltInRegistryOutcome::Changed,
            "a perturbed expected revision is rejected as Changed by the registry API");
        qualifiedProvider.publish(runtime::QualifiedDisplayProcessorBuildResult::failed(
            {.code = "bloom.test.qualified-display.forced-failure",
             .severity = runtime::DiagnosticSeverity::Error,
             .summary = "The Bloom Neutral display configuration content changed",
             .detail = {},
             .suggestedAction = {}}));

        controller.requestRefresh();
        expectations.expect(
            waitUntil([&] { return controller.state().activity == ui::PreviewActivity::Failed; }),
            "a request after a forced qualification failure reaches Failed");
        expectations.expect(controller.state().frame == lastGood &&
                                controller.state().freshness == ui::FrameFreshness::Stale &&
                                !controller.state().message.isEmpty(),
                            "Failed retains the last-good frame, marks it stale, and surfaces a "
                            "diagnostic message");
        expectations.expect(!controller.state().frame->isOcioQualified(),
                            "the retained frame is never silently relabeled as qualified");

        // A further request never substitutes the reference transform for a qualified one either --
        // it stays Failed with the exact same retained frame.
        controller.requestRefresh();
        expectations.expect(
            waitUntil([&] { return controller.state().activity == ui::PreviewActivity::Failed; }),
            "a subsequent request after the forced failure also reaches Failed");
        expectations.expect(controller.state().frame == lastGood,
                            "no later request silently substitutes a fresh reference frame");

        reachQuiescence(controller, bridge, scheduler, expectations);
    }
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testRevisionAndPanelSuppression(expectations);
    testNewestPendingRequestGate(expectations);
    testInteractiveCadenceCoalescesBurstAndVisibleBypasses(expectations);
    testActiveGateHoldsAndScrubEndBypassesRemainingCadence(expectations);
    testSameRevisionGenerationAndSelection(expectations);
    testLastGoodAndOutcomeMapping(expectations);
    testCompositionSwitchClearsPixels(expectations);
    testQualifiedDisplayReadinessAndFailClosed(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
