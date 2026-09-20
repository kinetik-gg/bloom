#include <bloom/commands/command_stack.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/application_shutdown_coordinator.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/qualified_display_processor_bootstrap.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QKeySequence>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QtGlobal>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;

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

template <typename Predicate> bool waitUntilBounded(Predicate predicate, const int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::invoke(predicate)) {
            return true;
        }
        std::this_thread::yield();
    }
    return std::invoke(predicate);
}

// The 2s bound FORMAL AMENDMENT 1 (S1-A) asks every reproduction shape to assert
// shutdownQuiescent within.
template <typename Predicate> bool waitUntil(Predicate predicate) {
    return waitUntilBounded(std::move(predicate), 2'000);
}

// A manually-advanced fake for PlaybackController::ClockFunction (mirrors playback_controller_
// tests.cpp's own ManualClock exactly): no dependency on real elapsed wall time, so shape (c)
// below drives playback purely by calling advance() then tick().
struct ManualClock final {
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

    void advance(const std::chrono::nanoseconds delta) { now += delta; }
};

// qInstallMessageHandler() requires a plain function (no captures), so the buffer it appends into
// has to live at namespace scope rather than as a lambda capture -- used only by
// testStuckShutdownDiagnosticLogsAfterFiveSeconds() below to observe
// ApplicationShutdownCoordinator's S1-C watchdog through its real qCWarning() output rather than a
// white-box hook.
QString& capturedDiagnosticMessages() {
    static QString storage;
    return storage;
}

void captureDiagnosticMessages(QtMsgType, const QMessageLogContext&, const QString& message) {
    capturedDiagnosticMessages() += message;
    capturedDiagnosticMessages() += QLatin1Char('\n');
}

bloom::runtime::TaskSchedulerConfig testSchedulerConfig() {
    return {.cpuWorkerCount = 1,
            .blockingIoWorkerCount = 1,
            .cpuQueueCapacity = 8,
            .blockingIoQueueCapacity = 4,
            .terminalHistoryCapacity = 16,
            .diagnosticsPerTask = 8,
            .groupRegistryCapacity = 8};
}

void testShutdownAndCloseRouting(Expectations& expectations) {
    using namespace bloom;
    auto newProject =
        document::makeNewProject("Shutdown Test", "Main", core::RationalTime::fromInteger(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    WorkerGate gate;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&gate](const document::Snapshot&, const runtime::PreviewRequestIdentity&, std::size_t,
                const std::vector<runtime::SnapshotParameterOverride>&, runtime::TaskContext&) {
            gate.enterAndWait();
            return runtime::TaskResult<ui::PreviewPreparationResultHandle>::cancelled();
        });
    ui::ApplicationShutdownCoordinator shutdown(controller, bridge);
    auto* application = QCoreApplication::instance();
    application->installEventFilter(&shutdown);

    ui::ProjectHost projectHost(scheduler);
    ui::EditorRegistry registry;
    runtime::NodeDefinitionRegistry nodeDefinitions;
    nodeDefinitions.freeze();
    runtime::SnapshotCompiler snapshotCompiler(nodeDefinitions);
    ui::FrameExportController frameExportController(session, scheduler, bridge, snapshotCompiler,
                                                    projectHost.publicationCoordinator(),
                                                    projectHost.artifactCoordinator());
    ui::MainWindow window(registry, session, projectHost, frameExportController);
    window.resize(913, 577);
    QTemporaryDir settingsDirectory;
    QSettings settings(settingsDirectory.filePath(QStringLiteral("shutdown-state.ini")),
                       QSettings::IniFormat);
    int stateSaveCount = 0;
    QObject::connect(&shutdown, &ui::ApplicationShutdownCoordinator::shutdownStarted, &window, [&] {
        ++stateSaveCount;
        window.saveApplicationState(settings);
    });

    expectations.expect(waitUntil([&] { return gate.entered(); }),
                        "shutdown fixture starts worker preparation");
    int heartbeatCount = 0;
    QTimer heartbeat;
    heartbeat.setInterval(0);
    QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat,
                     [&heartbeatCount] { ++heartbeatCount; });
    heartbeat.start();
    bool shutdownStarted = false;
    bool shutdownQuiescent = false;
    QObject::connect(&shutdown, &ui::ApplicationShutdownCoordinator::shutdownStarted, &shutdown,
                     [&shutdownStarted] { shutdownStarted = true; });
    QObject::connect(&shutdown, &ui::ApplicationShutdownCoordinator::shutdownQuiescent, &shutdown,
                     [&shutdownQuiescent] { shutdownQuiescent = true; });

    QEvent quitEvent(QEvent::Quit);
    QCoreApplication::sendEvent(application, &quitEvent);
    shutdown.beginShutdown();
    for (int iteration = 0; iteration < 8; ++iteration) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    expectations.expect(!quitEvent.isAccepted() && shutdownStarted && shutdown.isShuttingDown() &&
                            bridge.isShuttingDown(),
                        "application Quit is intercepted once and closes task admission");
    expectations.expect(
        settingsDirectory.isValid() && stateSaveCount == 1 &&
            settings.contains(QStringLiteral("window/main/geometry")) &&
            settings.contains(QStringLiteral("workspace/compositing/layout")),
        "staged shutdown captures window and workspace state once without an explicit sync");
    expectations.expect(heartbeatCount > 0 && !shutdownQuiescent,
                        "Qt events continue while cooperative work remains in flight");
    expectations.expect(controller.state().activity == ui::PreviewActivity::Cancelled,
                        "preview publication is cancelled before runtime shutdown");

    gate.release();
    expectations.expect(waitUntil([&] { return shutdownQuiescent; }),
                        "coordinator publishes only bridge-observed quiescence");
    expectations.expect(scheduler.isQuiescent(),
                        "shutdownQuiescent corresponds to scheduler quiescence");
    heartbeat.stop();
    application->removeEventFilter(&shutdown);

    int closeRequests = 0;
    QObject::connect(&window, &ui::MainWindow::shutdownRequested, &window,
                     [&closeRequests] { ++closeRequests; });
    window.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    window.close();
    window.close();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    expectations.expect(closeRequests == 1,
                        "MainWindow routes repeated close events through one shutdown request");
    expectations.expect(window.isVisible(),
                        "MainWindow remains alive while asynchronous shutdown owns completion");
    window.hide();
}

// Task S1: the PO's literal complaint -- "I cant kill the app from the menu" -- rather than the
// close-button path testShutdownAndCloseRouting() above already covers. MainWindow's File menu had
// no Quit/Exit action at all, so nothing the artist could click there ever reached
// MainWindow::shutdownRequested(); the app just kept running, indistinguishable from a genuine
// hang once the supervising session attached and found every thread idle. This asserts the File
// menu exposes a quit action, discoverable the same way every other action test in this codebase
// finds one (objectName + findChild), and that triggering it routes through the exact same
// shutdownRequested() -> ApplicationShutdownCoordinator::beginShutdown() path the window close
// button already uses.
void testFileMenuQuitRoutesThroughShutdown(Expectations& expectations) {
    using namespace bloom;
    auto newProject =
        document::makeNewProject("Menu Quit Test", "Main", core::RationalTime::fromInteger(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [](const document::Snapshot&, const runtime::PreviewRequestIdentity&, std::size_t,
           const std::vector<runtime::SnapshotParameterOverride>&, runtime::TaskContext&) {
            return runtime::TaskResult<ui::PreviewPreparationResultHandle>::cancelled();
        });
    ui::ApplicationShutdownCoordinator shutdown(controller, bridge);

    ui::ProjectHost projectHost(scheduler);
    ui::EditorRegistry registry;
    runtime::NodeDefinitionRegistry nodeDefinitions;
    nodeDefinitions.freeze();
    runtime::SnapshotCompiler snapshotCompiler(nodeDefinitions);
    ui::FrameExportController frameExportController(session, scheduler, bridge, snapshotCompiler,
                                                    projectHost.publicationCoordinator(),
                                                    projectHost.artifactCoordinator());
    ui::MainWindow window(registry, session, projectHost, frameExportController);
    QObject::connect(&window, &ui::MainWindow::shutdownRequested, &shutdown,
                     &ui::ApplicationShutdownCoordinator::beginShutdown);

    auto* quitAction = window.findChild<QAction*>(QStringLiteral("quitAction"));
    expectations.expect(quitAction != nullptr, "the File menu exposes a discoverable Quit action");
    if (quitAction == nullptr) {
        return;
    }
    expectations.expect(quitAction->shortcut() == QKeySequence(QKeySequence::Quit),
                        "the Quit action carries the platform Quit shortcut");

    int shutdownRequests = 0;
    QObject::connect(&window, &ui::MainWindow::shutdownRequested, &window,
                     [&shutdownRequests] { ++shutdownRequests; });
    window.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    quitAction->trigger();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    expectations.expect(shutdownRequests == 1,
                        "triggering the File menu Quit action requests shutdown exactly once");
    expectations.expect(shutdown.isShuttingDown(),
                        "the File menu Quit action reaches the same shutdown coordinator as close");
    window.hide();
}

// Task S1: reproduces the PO-reported hang ("stuck at 'shutdown state'") for the one shape the
// gated-worker fixture above never exercises -- an application that has gone fully idle (every
// startup task, including QualifiedDisplayProcessorBootstrap's one-time blocking-stage build, has
// already reached a terminal state and no new work is in flight) before the window is closed.
// This wires the exact production wiring from apps/bloom/main.cpp (TaskUiBridge,
// QualifiedDisplayProcessorBootstrap, CompositionPreviewController's real pipeline, MainWindow's
// close-event routing, and ApplicationShutdownCoordinator all reacting through their real signals)
// rather than the earlier fixture's synthetic worker gate, and asserts the coordinator reaches
// quiescence -- and the application's quit is actually requested -- within a bounded timeout.
void testIdleApplicationQuitsOnClose(Expectations& expectations) {
    using namespace bloom;
    auto newProject =
        document::makeNewProject("Idle Shutdown Test", "Main", core::RationalTime::fromInteger(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);

    runtime::NodeDefinitionRegistry nodeDefinitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(nodeDefinitions),
                        "idle fixture registers built-in node definitions");
    nodeDefinitions.freeze();
    runtime::SnapshotCompiler snapshotCompiler(nodeDefinitions);
    runtime::CpuCompositionEvaluator cpuEvaluator;
    runtime::CpuReferenceDisplayPreparer referenceDisplayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedDisplayProcessorProvider;

    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge taskUiBridge(scheduler, nullptr, 1ms);
    // Present in every real run (apps/bloom/main.cpp), submitted at construction, and absent from
    // every other coordinator/shutdown test in this file -- the prime suspect the task package
    // named for a stage that never reports "drained".
    ui::QualifiedDisplayProcessorBootstrap qualifiedDisplayProcessorBootstrap(
        scheduler, taskUiBridge, qualifiedDisplayProcessorProvider);
    ui::CompositionPreviewController previewController(
        session, scheduler, taskUiBridge,
        ui::makeCompositionPreviewPipeline(snapshotCompiler, cpuEvaluator, referenceDisplayPreparer,
                                           qualifiedDisplayProcessorProvider));
    ui::ApplicationShutdownCoordinator shutdown(previewController, taskUiBridge);
    auto* application = QCoreApplication::instance();
    application->installEventFilter(&shutdown);

    ui::ProjectHost projectHost(scheduler);
    ui::EditorRegistry registry;
    ui::FrameExportController frameExportController(
        session, scheduler, taskUiBridge, snapshotCompiler, projectHost.publicationCoordinator(),
        projectHost.artifactCoordinator());
    ui::MainWindow window(registry, session, projectHost, frameExportController);
    QObject::connect(&window, &ui::MainWindow::shutdownRequested, &shutdown,
                     &ui::ApplicationShutdownCoordinator::beginShutdown);
    bool quitRequested = false;
    QObject::connect(&shutdown, &ui::ApplicationShutdownCoordinator::shutdownQuiescent, &shutdown,
                     [&quitRequested] { quitRequested = true; });

    window.show();

    // Let the application settle into the fully idle state the bug report describes: every
    // startup task (the qualified display processor build, the initial preview render) has
    // reached a terminal state and the task scheduler has nothing in flight, well before the
    // artist ever asks to quit.
    expectations.expect(
        waitUntil([&scheduler] { return scheduler.isQuiescent(); }),
        "idle fixture settles to scheduler quiescence before any quit is requested");

    // The same action MainWindow's close button (and File -> Quit, whatever menu action reaches
    // it) triggers: QWidget::close() -> MainWindow::closeEvent() -> shutdownRequested().
    window.close();

    expectations.expect(
        waitUntil([&quitRequested] { return quitRequested; }),
        "an idle application reaches shutdown quiescence within a bounded timeout after close");
    expectations.expect(scheduler.isQuiescent(),
                        "shutdownQuiescent corresponds to scheduler quiescence for an idle app");
    application->removeEventFilter(&shutdown);
    window.hide();
}

// FORMAL AMENDMENT 1 (S1-A): the missing Quit action does not explain the owner's second
// sentence -- "now it stuck at 'shutdown state'". The owner SAW a shutdown-state UI: reading
// viewer_editor.cpp confirms CompositionPreviewController::beginShutdown()'s "Preview rendering
// was cancelled during application shutdown" message paints as a literal banner over the last
// retained frame (drawDiagnosticBanner(), gated on drewPixels && activity != Ready/Rendering) --
// so beginShutdown() DID run (via the close button, the one path that worked before this task's
// first commit), and the owner's window sat on that banner instead of closing. The owner's
// session shape -- one solid layer, a preview actually delivered to the viewer, then close -- is
// exactly what drewPixels requires (a previously retained frame), and exactly what the idle
// fixture above never produces (it closes before anything ever reaches Ready). Each shape below
// reproduces the SAME full production wiring as testIdleApplicationQuitsOnClose and asserts
// shutdownQuiescent within the same 2s bound.

// S1-A(a): a solid layer added, its preview delivered to the viewer (Ready, with real pixels),
// THEN close -- the owner's literal reported session shape.
void testShapeSolidLayerPreviewDeliveredThenClose(Expectations& expectations) {
    using namespace bloom;
    const auto format = document::CompositionFormat::create(4, 4);
    expectations.expect(format.has_value(), "shape (a): the small composition format is valid");
    if (!format.has_value()) {
        return;
    }
    auto newProject = document::makeNewProject("Solid Preview Shutdown Test", "Main",
                                               core::RationalTime::fromInteger(10), *format);
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.4, 0.6, 1.0}),
        "shape (a): the solid layer is added");

    runtime::NodeDefinitionRegistry nodeDefinitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(nodeDefinitions),
                        "shape (a) fixture registers built-in node definitions");
    nodeDefinitions.freeze();
    runtime::SnapshotCompiler snapshotCompiler(nodeDefinitions);
    runtime::CpuCompositionEvaluator cpuEvaluator;
    runtime::CpuReferenceDisplayPreparer referenceDisplayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedDisplayProcessorProvider;

    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge taskUiBridge(scheduler, nullptr, 1ms);
    ui::QualifiedDisplayProcessorBootstrap qualifiedDisplayProcessorBootstrap(
        scheduler, taskUiBridge, qualifiedDisplayProcessorProvider);
    ui::CompositionPreviewController previewController(
        session, scheduler, taskUiBridge,
        ui::makeCompositionPreviewPipeline(snapshotCompiler, cpuEvaluator, referenceDisplayPreparer,
                                           qualifiedDisplayProcessorProvider));
    ui::ApplicationShutdownCoordinator shutdown(previewController, taskUiBridge);
    auto* application = QCoreApplication::instance();
    application->installEventFilter(&shutdown);

    ui::ProjectHost projectHost(scheduler);
    ui::EditorRegistry registry;
    ui::FrameExportController frameExportController(
        session, scheduler, taskUiBridge, snapshotCompiler, projectHost.publicationCoordinator(),
        projectHost.artifactCoordinator());
    ui::MainWindow window(registry, session, projectHost, frameExportController);
    QObject::connect(&window, &ui::MainWindow::shutdownRequested, &shutdown,
                     &ui::ApplicationShutdownCoordinator::beginShutdown);
    bool quitRequested = false;
    QObject::connect(&shutdown, &ui::ApplicationShutdownCoordinator::shutdownQuiescent, &shutdown,
                     [&quitRequested] { quitRequested = true; });

    window.show();

    expectations.expect(
        waitUntilBounded(
            [&] { return previewController.state().activity == ui::PreviewActivity::Ready; },
            8'000),
        "shape (a): the solid-layer preview reaches Ready with a delivered frame");
    expectations.expect(previewController.state().frame != nullptr,
                        "shape (a): the delivered frame carries real pixels before close");

    window.close();

    expectations.expect(
        waitUntil([&quitRequested] { return quitRequested; }),
        "shape (a): a delivered preview does not block shutdown quiescence within a bounded "
        "timeout");
    expectations.expect(scheduler.isQuiescent(),
                        "shape (a): shutdownQuiescent corresponds to scheduler quiescence");
    if (!quitRequested) {
        const auto snapshots = scheduler.snapshots();
        std::cerr << "shape (a) diagnostic: " << snapshots.size()
                  << " scheduler task snapshot(s) at close+2s; non-terminal state(s):\n";
        for (const auto& snapshot : snapshots) {
            if (!runtime::isTerminal(snapshot.state)) {
                std::cerr << "  task " << snapshot.id.value()
                          << " executor=" << static_cast<int>(snapshot.executor)
                          << " state=" << static_cast<int>(snapshot.state) << '\n';
            }
        }
    }
    application->removeEventFilter(&shutdown);
    window.hide();
}

// S1-A(b): close while a preview render is genuinely in flight -- a fresh composition change
// submits a task and close() is called immediately, before the event loop ever runs, so neither
// the worker thread's completion nor TaskUiBridge's poll has been observed yet.
void testShapeCloseWhilePreviewInFlight(Expectations& expectations) {
    using namespace bloom;
    const auto format = document::CompositionFormat::create(4, 4);
    expectations.expect(format.has_value(), "shape (b): the small composition format is valid");
    if (!format.has_value()) {
        return;
    }
    auto newProject = document::makeNewProject("In-Flight Preview Shutdown Test", "Main",
                                               core::RationalTime::fromInteger(10), *format);
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);

    runtime::NodeDefinitionRegistry nodeDefinitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(nodeDefinitions),
                        "shape (b) fixture registers built-in node definitions");
    nodeDefinitions.freeze();
    runtime::SnapshotCompiler snapshotCompiler(nodeDefinitions);
    runtime::CpuCompositionEvaluator cpuEvaluator;
    runtime::CpuReferenceDisplayPreparer referenceDisplayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedDisplayProcessorProvider;

    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge taskUiBridge(scheduler, nullptr, 1ms);
    ui::QualifiedDisplayProcessorBootstrap qualifiedDisplayProcessorBootstrap(
        scheduler, taskUiBridge, qualifiedDisplayProcessorProvider);
    ui::CompositionPreviewController previewController(
        session, scheduler, taskUiBridge,
        ui::makeCompositionPreviewPipeline(snapshotCompiler, cpuEvaluator, referenceDisplayPreparer,
                                           qualifiedDisplayProcessorProvider));
    ui::ApplicationShutdownCoordinator shutdown(previewController, taskUiBridge);
    auto* application = QCoreApplication::instance();
    application->installEventFilter(&shutdown);

    ui::ProjectHost projectHost(scheduler);
    ui::EditorRegistry registry;
    ui::FrameExportController frameExportController(
        session, scheduler, taskUiBridge, snapshotCompiler, projectHost.publicationCoordinator(),
        projectHost.artifactCoordinator());
    ui::MainWindow window(registry, session, projectHost, frameExportController);
    QObject::connect(&window, &ui::MainWindow::shutdownRequested, &shutdown,
                     &ui::ApplicationShutdownCoordinator::beginShutdown);
    bool quitRequested = false;
    QObject::connect(&shutdown, &ui::ApplicationShutdownCoordinator::shutdownQuiescent, &shutdown,
                     [&quitRequested] { quitRequested = true; });

    window.show();
    // Settle the startup work (bootstrap build, the constructor's own initial preview request)
    // first, so the in-flight task below is unambiguously the NEW one this shape adds.
    (void)waitUntil([&scheduler] { return scheduler.isQuiescent(); });

    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.4, 0.6, 1.0}),
        "shape (b): the solid layer is added, submitting a fresh preview task");
    expectations.expect(previewController.state().activity == ui::PreviewActivity::Rendering,
                        "shape (b): the fresh preview request is still outstanding at close time");

    window.close();

    expectations.expect(
        waitUntil([&quitRequested] { return quitRequested; }),
        "shape (b): a preview genuinely in flight at close time still reaches quiescence within a "
        "bounded timeout");
    expectations.expect(scheduler.isQuiescent(),
                        "shape (b): shutdownQuiescent corresponds to scheduler quiescence");
    application->removeEventFilter(&shutdown);
    window.hide();
}

// S1-A(c): close while CompositionPreviewController's Interactive-cadence gate is armed and a
// PlaybackController-driven tick is in flight -- playback is never paused before close, so the
// arming flag and any trailing-cadence-gated pending request are still live when beginShutdown()
// runs. Composes the real bloom::ui::PlaybackController against the SAME session/previewController
// the coordinator watches (playback_controller_tests.cpp's own ManualClock idiom), exactly the way
// TimelineEditor wires it in production (composition_editors.cpp).
void testShapeCloseWhilePlaybackArmed(Expectations& expectations) {
    using namespace bloom;
    const auto format = document::CompositionFormat::create(4, 4);
    expectations.expect(format.has_value(), "shape (c): the small composition format is valid");
    if (!format.has_value()) {
        return;
    }
    auto newProject = document::makeNewProject("Playback Shutdown Test", "Main",
                                               core::RationalTime::fromInteger(4), *format);
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.4, 0.6, 1.0}),
        "shape (c): the solid layer is added");

    runtime::NodeDefinitionRegistry nodeDefinitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(nodeDefinitions),
                        "shape (c) fixture registers built-in node definitions");
    nodeDefinitions.freeze();
    runtime::SnapshotCompiler snapshotCompiler(nodeDefinitions);
    runtime::CpuCompositionEvaluator cpuEvaluator;
    runtime::CpuReferenceDisplayPreparer referenceDisplayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedDisplayProcessorProvider;

    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge taskUiBridge(scheduler, nullptr, 1ms);
    ui::QualifiedDisplayProcessorBootstrap qualifiedDisplayProcessorBootstrap(
        scheduler, taskUiBridge, qualifiedDisplayProcessorProvider);
    ui::CompositionPreviewController previewController(
        session, scheduler, taskUiBridge,
        ui::makeCompositionPreviewPipeline(snapshotCompiler, cpuEvaluator, referenceDisplayPreparer,
                                           qualifiedDisplayProcessorProvider));
    ManualClock clock;
    ui::PlaybackController playback(
        session, previewController, [&clock] { return clock.now; }, 16ms);
    ui::ApplicationShutdownCoordinator shutdown(previewController, taskUiBridge);
    auto* application = QCoreApplication::instance();
    application->installEventFilter(&shutdown);

    ui::ProjectHost projectHost(scheduler);
    ui::EditorRegistry registry;
    ui::FrameExportController frameExportController(
        session, scheduler, taskUiBridge, snapshotCompiler, projectHost.publicationCoordinator(),
        projectHost.artifactCoordinator());
    ui::MainWindow window(registry, session, projectHost, frameExportController);
    QObject::connect(&window, &ui::MainWindow::shutdownRequested, &shutdown,
                     &ui::ApplicationShutdownCoordinator::beginShutdown);
    bool quitRequested = false;
    QObject::connect(&shutdown, &ui::ApplicationShutdownCoordinator::shutdownQuiescent, &shutdown,
                     [&quitRequested] { quitRequested = true; });

    window.show();
    (void)waitUntil([&scheduler] { return scheduler.isQuiescent(); });

    playback.play();
    clock.advance(std::chrono::milliseconds{160});
    playback.tick();
    expectations.expect(playback.state() == ui::PlaybackState::Playing,
                        "shape (c): playback is still armed and running at close time");

    // The same action the close button triggers -- playback is never paused first, exactly like
    // an artist closing the window mid-play.
    window.close();

    expectations.expect(
        waitUntil([&quitRequested] { return quitRequested; }),
        "shape (c): an application closed while playback is armed still reaches quiescence within "
        "a bounded timeout");
    expectations.expect(scheduler.isQuiescent(),
                        "shape (c): shutdownQuiescent corresponds to scheduler quiescence");
    application->removeEventFilter(&shutdown);
    window.hide();
}

// S1-A(d): close right after a Frame Export attempt (PNG, to a temp dir) reaches a terminal,
// published outcome -- mirrors frame_export_controller_tests.cpp's own PNG destination-seam drive.
void testShapeCloseAfterFrameExportCompletes(Expectations& expectations) {
    using namespace bloom;
    const auto format = document::CompositionFormat::create(4, 4);
    expectations.expect(format.has_value(), "shape (d): the small composition format is valid");
    if (!format.has_value()) {
        return;
    }
    auto newProject = document::makeNewProject("Export Shutdown Test", "Main",
                                               core::RationalTime::fromInteger(10), *format);
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.2, 0.4, 0.6, 1.0}),
        "shape (d): the solid layer is added");

    runtime::NodeDefinitionRegistry nodeDefinitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(nodeDefinitions),
                        "shape (d) fixture registers built-in node definitions");
    nodeDefinitions.freeze();
    runtime::SnapshotCompiler snapshotCompiler(nodeDefinitions);
    runtime::CpuCompositionEvaluator cpuEvaluator;
    runtime::CpuReferenceDisplayPreparer referenceDisplayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedDisplayProcessorProvider;

    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge taskUiBridge(scheduler, nullptr, 1ms);
    ui::QualifiedDisplayProcessorBootstrap qualifiedDisplayProcessorBootstrap(
        scheduler, taskUiBridge, qualifiedDisplayProcessorProvider);
    ui::CompositionPreviewController previewController(
        session, scheduler, taskUiBridge,
        ui::makeCompositionPreviewPipeline(snapshotCompiler, cpuEvaluator, referenceDisplayPreparer,
                                           qualifiedDisplayProcessorProvider));
    ui::ApplicationShutdownCoordinator shutdown(previewController, taskUiBridge);
    auto* application = QCoreApplication::instance();
    application->installEventFilter(&shutdown);

    ui::ProjectHost projectHost(scheduler);
    ui::EditorRegistry registry;
    QTemporaryDir exportDirectory;
    expectations.expect(exportDirectory.isValid(), "shape (d): the export temp dir is valid");
    ui::FrameExportController frameExportController(
        session, scheduler, taskUiBridge, snapshotCompiler, projectHost.publicationCoordinator(),
        projectHost.artifactCoordinator(),
        std::filesystem::path(exportDirectory.path().toStdString()) / "scratch");
    ui::MainWindow window(registry, session, projectHost, frameExportController);
    QObject::connect(&window, &ui::MainWindow::shutdownRequested, &shutdown,
                     &ui::ApplicationShutdownCoordinator::beginShutdown);
    bool quitRequested = false;
    QObject::connect(&shutdown, &ui::ApplicationShutdownCoordinator::shutdownQuiescent, &shutdown,
                     [&quitRequested] { quitRequested = true; });

    window.show();

    const auto target =
        std::filesystem::path(exportDirectory.path().toStdString()) / "shutdown-export.png";
    frameExportController.setDestinationProvider(
        [&target]() -> std::optional<std::filesystem::path> { return target; });
    frameExportController.setApprovalDecisionProvider([](const ui::FrameExportApprovalPrompt&) {
        return ui::FrameExportApprovalDecision::Export;
    });

    int exportFinishedCount = 0;
    ui::FrameExportOutcome exportOutcome = ui::FrameExportOutcome::Refused;
    QObject::connect(&frameExportController, &ui::FrameExportController::exportFinished,
                     [&](const ui::FrameExportOutcome outcome, const QString&) {
                         ++exportFinishedCount;
                         exportOutcome = outcome;
                     });
    frameExportController.requestExport();
    expectations.expect(waitUntilBounded([&] { return exportFinishedCount == 1; }, 8'000),
                        "shape (d): the export reaches a terminal outcome before close");
    expectations.expect(exportOutcome == ui::FrameExportOutcome::Published,
                        "shape (d): the export publishes");

    window.close();

    expectations.expect(
        waitUntil([&quitRequested] { return quitRequested; }),
        "shape (d): an application closed right after a completed export still reaches quiescence "
        "within a bounded timeout");
    expectations.expect(scheduler.isQuiescent(),
                        "shape (d): shutdownQuiescent corresponds to scheduler quiescence");
    application->removeEventFilter(&shutdown);
    window.hide();
}

// FORMAL AMENDMENT 1 (S1-C): none of the four shapes above reproduced a stuck shutdown, so this
// covers the diagnostic S1-C asks for instead of a force-quit fallback -- a genuine in-flight
// worker that never signals completion (this file's original gated-worker fixture, held past the
// 5s watchdog rather than released quickly) makes ApplicationShutdownCoordinator log its
// outstanding task bridge snapshots exactly once, through the coordinator's real qCWarning()
// output rather than a white-box hook, and shutdown still completes normally once the worker
// finally does complete.
void testStuckShutdownDiagnosticLogsAfterFiveSeconds(Expectations& expectations) {
    using namespace bloom;
    auto newProject = document::makeNewProject("Stuck Shutdown Diagnostic Test", "Main",
                                               core::RationalTime::fromInteger(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    WorkerGate gate;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&gate](const document::Snapshot&, const runtime::PreviewRequestIdentity&, std::size_t,
                const std::vector<runtime::SnapshotParameterOverride>&, runtime::TaskContext&) {
            gate.enterAndWait();
            return runtime::TaskResult<ui::PreviewPreparationResultHandle>::cancelled();
        });
    ui::ApplicationShutdownCoordinator shutdown(controller, bridge);

    expectations.expect(waitUntil([&] { return gate.entered(); }),
                        "diagnostic fixture starts worker preparation");

    capturedDiagnosticMessages().clear();
    QtMessageHandler previousHandler = qInstallMessageHandler(&captureDiagnosticMessages);

    shutdown.beginShutdown();
    // The gate holds the worker open indefinitely -- a genuine in-flight task that has not yet
    // signalled completion, exactly the case the original task package allowed to "delay, never
    // block forever" -- so shutdownQuiescent has not arrived by the time the 5s watchdog fires.
    const bool loggedDiagnostic = waitUntilBounded(
        [] { return capturedDiagnosticMessages().contains(QStringLiteral("outstanding task")); },
        6'500);
    qInstallMessageHandler(previousHandler);
    expectations.expect(loggedDiagnostic,
                        "the 5s watchdog logs a diagnostic while a genuine worker is still in "
                        "flight, not a force-quit");

    gate.release();
    expectations.expect(
        waitUntil([&scheduler] { return scheduler.isQuiescent(); }),
        "releasing the gated worker still lets shutdown reach quiescence normally afterward");
}

// Exercise QApplication::exec()/quit(), not only the coordinator's quiescent signal: Qt sends
// another close event to visible windows before it permits the application event loop to exit.
void testRealApplicationQuit(Expectations& expectations, const bool dirty) {
    using namespace bloom;
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::ProjectHost projectHost(scheduler);
    const auto [document, commands] = projectHost.liveDocumentAndStack();
    ui::CompositionSession session(*document, *commands, projectHost.lowestCompositionId());
    if (dirty)
        (void)session.addSolidLayer("Unsaved", core::Color4d{1, 0, 0, 1});
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [](const document::Snapshot&, const runtime::PreviewRequestIdentity&, std::size_t,
           const std::vector<runtime::SnapshotParameterOverride>&, runtime::TaskContext&) {
            return runtime::TaskResult<ui::PreviewPreparationResultHandle>::cancelled();
        });
    ui::ApplicationShutdownCoordinator shutdown(controller, bridge);
    auto* application = qApp;
    const bool oldQuitOnClose = application->quitOnLastWindowClosed();
    application->setQuitOnLastWindowClosed(false);
    application->installEventFilter(&shutdown);
    ui::EditorRegistry registry;
    runtime::NodeDefinitionRegistry definitions;
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    ui::FrameExportController exporter(session, scheduler, bridge, compiler,
                                       projectHost.publicationCoordinator(),
                                       projectHost.artifactCoordinator());
    ui::MainWindow window(registry, session, projectHost, exporter);
    QObject::connect(&window, &ui::MainWindow::shutdownRequested, &shutdown,
                     &ui::ApplicationShutdownCoordinator::beginShutdown);
    QObject::connect(&shutdown, &ui::ApplicationShutdownCoordinator::shutdownQuiescent, &window,
                     &ui::MainWindow::completeShutdown);
    QObject::connect(&shutdown, &ui::ApplicationShutdownCoordinator::shutdownQuiescent, application,
                     &QApplication::quit);
    int decisions = 0;
    projectHost.setUnsavedChangeDecisionProvider([&] {
        return ++decisions == 1 ? ui::UnsavedChangeDecision::Cancel
                                : ui::UnsavedChangeDecision::Discard;
    });
    bool timedOut = false;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, application, [&] {
        timedOut = true;
        QCoreApplication::exit(1);
    });
    window.show();
    QTimer::singleShot(0, &window, [&] {
        window.findChild<QAction*>("quitAction")->trigger();
        if (dirty) {
            expectations.expect(decisions == 1 && !shutdown.isShuttingDown() && window.isVisible(),
                                "Cancel leaves the dirty application active");
            window.close();
        }
    });
    watchdog.start(2000);
    const int result = application->exec();
    watchdog.stop();
    expectations.expect(!timedOut && result == 0,
                        "clean Quit or Discard must exit the real QApplication loop");
    expectations.expect(decisions == (dirty ? 2 : 0) && scheduler.isQuiescent() &&
                            !window.isVisible(),
                        "final Qt close neither prompts again nor exits before workers finish");
    application->removeEventFilter(&shutdown);
    application->setQuitOnLastWindowClosed(oldQuitOnClose);
    window.hide();
}

} // namespace

// The GPU final-render retirement participant is part of the shutdown contract: the coordinator
// signals it non-blocking at beginShutdown and then withholds shutdownQuiescent until its
// completion predicate genuinely reports the evaluator owner retired, polling from the still-live
// UI event loop. This drives that ordering with a deterministic fake so the UI never blocks and a
// never-completing owner can never fake quiescence.
void testGpuRetirementWithholdsQuiescenceUntilComplete(Expectations& expectations) {
    using namespace bloom;
    auto newProject = document::makeNewProject("GPU Retirement Shutdown Test", "Main",
                                               core::RationalTime::fromInteger(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);

    runtime::NodeDefinitionRegistry nodeDefinitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(nodeDefinitions),
                        "gpu-retirement: built-in node definitions register");
    nodeDefinitions.freeze();
    runtime::SnapshotCompiler snapshotCompiler(nodeDefinitions);
    runtime::CpuCompositionEvaluator cpuEvaluator;
    runtime::CpuReferenceDisplayPreparer referenceDisplayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedDisplayProcessorProvider;

    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge taskUiBridge(scheduler, nullptr, 1ms);
    ui::QualifiedDisplayProcessorBootstrap bootstrap(scheduler, taskUiBridge,
                                                     qualifiedDisplayProcessorProvider);
    ui::CompositionPreviewController previewController(
        session, scheduler, taskUiBridge,
        ui::makeCompositionPreviewPipeline(snapshotCompiler, cpuEvaluator, referenceDisplayPreparer,
                                           qualifiedDisplayProcessorProvider));
    ui::ApplicationShutdownCoordinator shutdown(previewController, taskUiBridge);

    auto gpuBegun = std::make_shared<std::atomic_bool>(false);
    auto gpuComplete = std::make_shared<std::atomic_bool>(false);
    shutdown.setGpuExportRetirement(
        [gpuBegun] { gpuBegun->store(true, std::memory_order_release); },
        [gpuComplete] { return gpuComplete->load(std::memory_order_acquire); });

    bool quiescent = false;
    QObject::connect(&shutdown, &ui::ApplicationShutdownCoordinator::shutdownQuiescent, &shutdown,
                     [&quiescent] { quiescent = true; });

    expectations.expect(waitUntil([&scheduler] { return scheduler.isQuiescent(); }),
                        "gpu-retirement: scheduler is idle before shutdown");
    shutdown.beginShutdown();
    expectations.expect(gpuBegun->load(std::memory_order_acquire),
                        "gpu-retirement: the participant is signalled non-blocking");

    // Pump the UI event loop for a bounded window with retirement still incomplete: the coordinator
    // must keep running but must NOT publish quiescence.
    const auto withheldUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (!quiescent && std::chrono::steady_clock::now() < withheldUntil) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    expectations.expect(!quiescent && !shutdown.gpuExportRetirementSatisfied(),
                        "gpu-retirement: quiescence is withheld until retirement is proven");

    gpuComplete->store(true, std::memory_order_release);
    expectations.expect(waitUntil([&quiescent] { return quiescent; }),
                        "gpu-retirement: async completion releases quiescence");
    expectations.expect(shutdown.gpuExportRetirementSatisfied(),
                        "gpu-retirement: the coordinator reports the GPU half satisfied");
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testRealApplicationQuit(expectations, true);
    testRealApplicationQuit(expectations, false);
    testShutdownAndCloseRouting(expectations);
    testFileMenuQuitRoutesThroughShutdown(expectations);
    testIdleApplicationQuitsOnClose(expectations);
    testShapeSolidLayerPreviewDeliveredThenClose(expectations);
    testShapeCloseWhilePreviewInFlight(expectations);
    testShapeCloseWhilePlaybackArmed(expectations);
    testShapeCloseAfterFrameExportCompletes(expectations);
    testGpuRetirementWithholdsQuiescenceUntilComplete(expectations);
    testStuckShutdownDiagnosticLogsAfterFiveSeconds(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
