#include <bloom/ui/application_shutdown_coordinator.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <bloom/runtime/task_types.hpp>

#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QLoggingCategory>
#include <QThread>

namespace bloom::ui {

namespace {
Q_LOGGING_CATEGORY(applicationShutdownLog, "bloom.ui.application_shutdown")
} // namespace

ApplicationShutdownCoordinator::ApplicationShutdownCoordinator(
    CompositionPreviewController& previewController, TaskUiBridge& taskUiBridge, QObject* parent)
    : QObject(parent), previewController_(previewController), taskUiBridge_(taskUiBridge) {
    stuckShutdownDiagnosticTimer_.setSingleShot(true);
    connect(&stuckShutdownDiagnosticTimer_, &QTimer::timeout, this,
            &ApplicationShutdownCoordinator::logStillShuttingDownDiagnostic);
    connect(&taskUiBridge_, &TaskUiBridge::shutdownQuiescent, this, [this] {
        Q_ASSERT(QThread::currentThread() == thread());
        if (!shuttingDown_ || quiescencePublished_) {
            return;
        }
        quiescencePublished_ = true;
        stuckShutdownDiagnosticTimer_.stop();
        emit shutdownQuiescent();
    });
}

bool ApplicationShutdownCoordinator::isShuttingDown() const noexcept { return shuttingDown_; }

void ApplicationShutdownCoordinator::beginShutdown() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;
    emit shutdownStarted();
    previewController_.beginShutdown();
    taskUiBridge_.beginShutdown();
    // FORMAL AMENDMENT 1 (S1-C): every shape tried in application_shutdown_tests.cpp (a delivered
    // preview, a preview genuinely in flight, playback armed and never paused, a just-completed
    // frame export -- each closed the same way this method is reached) reaches shutdownQuiescent
    // well within 2s, so this is a bounded diagnostic ONLY, never a force-quit fallback: if
    // quiescence still has not arrived 5s after this call, log what the task bridge still
    // considers outstanding so a genuine future occurrence is self-explaining instead of silently
    // unresponsive.
    stuckShutdownDiagnosticTimer_.start(5'000);
}

void ApplicationShutdownCoordinator::logStillShuttingDownDiagnostic() const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (quiescencePublished_) {
        return;
    }
    const auto& snapshots = taskUiBridge_.snapshots();
    std::size_t outstanding = 0;
    for (const auto& snapshot : snapshots) {
        if (!runtime::isTerminal(snapshot.state)) {
            ++outstanding;
        }
    }
    qCWarning(applicationShutdownLog).noquote()
        << QStringLiteral("Shutdown has not reached quiescence 5s after beginShutdown(): %1 "
                          "outstanding task(s) of %2 tracked, task bridge shutting down=%3")
               .arg(outstanding)
               .arg(snapshots.size())
               .arg(taskUiBridge_.isShuttingDown() ? QStringLiteral("true")
                                                   : QStringLiteral("false"));
    for (const auto& snapshot : snapshots) {
        if (runtime::isTerminal(snapshot.state)) {
            continue;
        }
        qCWarning(applicationShutdownLog).noquote()
            << QStringLiteral("  task id=%1 name=\"%2\" executor=%3 state=%4 "
                              "cancellationRequested=%5")
                   .arg(snapshot.id.value())
                   .arg(QString::fromStdString(snapshot.name))
                   .arg(static_cast<int>(snapshot.executor))
                   .arg(static_cast<int>(snapshot.state))
                   .arg(snapshot.cancellationRequested ? QStringLiteral("true")
                                                       : QStringLiteral("false"));
    }
}

bool ApplicationShutdownCoordinator::eventFilter(QObject* watched, QEvent* event) {
    if (watched == QCoreApplication::instance() && event->type() == QEvent::Quit &&
        !quiescencePublished_) {
        event->ignore();
        beginShutdown();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace bloom::ui
