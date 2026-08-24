#include <bloom/commands/command_stack.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/application_shutdown_coordinator.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
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
                runtime::TaskContext&) {
            gate.enterAndWait();
            return runtime::TaskResult<ui::PreviewPreparationResultHandle>::cancelled();
        });
    ui::ApplicationShutdownCoordinator shutdown(controller, bridge);
    auto* application = QCoreApplication::instance();
    application->installEventFilter(&shutdown);

    ui::EditorRegistry registry;
    ui::MainWindow window(registry, session);
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

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testShutdownAndCloseRouting(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
