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
#include <bloom/ui/application_shutdown_coordinator.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/main_window.hpp>
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

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
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

template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::invoke(predicate)) {
            return true;
        }
        std::this_thread::yield();
    }
    return std::invoke(predicate);
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
                const std::optional<runtime::SnapshotParameterOverride>&, runtime::TaskContext&) {
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
           const std::optional<runtime::SnapshotParameterOverride>&, runtime::TaskContext&) {
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

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testShutdownAndCloseRouting(expectations);
    testFileMenuQuitRoutesThroughShutdown(expectations);
    testIdleApplicationQuitsOnClose(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
