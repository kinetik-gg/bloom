#include <bloom/ui/application_shutdown_coordinator.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <bloom/runtime/task_types.hpp>

#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QLoggingCategory>
#include <QThread>

#include <utility>

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
    gpuRetirementPollTimer_.setInterval(5);
    connect(&gpuRetirementPollTimer_, &QTimer::timeout, this,
            &ApplicationShutdownCoordinator::pollGpuExportRetirement);
    connect(&taskUiBridge_, &TaskUiBridge::shutdownQuiescent, this, [this] {
        Q_ASSERT(QThread::currentThread() == thread());
        if (!shuttingDown_ || quiescencePublished_) {
            return;
        }
        taskQuiescence_ = true;
        publishQuiescenceIfReady();
    });
}

bool ApplicationShutdownCoordinator::isShuttingDown() const noexcept { return shuttingDown_; }

bool ApplicationShutdownCoordinator::nativeSurfaceRetirementSatisfied() const noexcept {
    return surfaceRetirementComplete_;
}

const std::string& ApplicationShutdownCoordinator::nativeSurfaceRefusalDiagnostic() const noexcept {
    return nativeSurfaceRefusalDiagnostic_;
}

bool ApplicationShutdownCoordinator::gpuExportRetirementSatisfied() const noexcept {
    return gpuExportRetirementComplete_;
}

void ApplicationShutdownCoordinator::setGpuExportRetirement(
    GpuRetirementBegin beginGpuRetirement, GpuRetirementComplete gpuRetirementComplete) {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(!shuttingDown_);
    gpuRetirementBegin_ = std::move(beginGpuRetirement);
    gpuRetirementComplete_ = std::move(gpuRetirementComplete);
    // With no participant the GPU half is trivially satisfied; with one, completion starts false.
    gpuExportRetirementComplete_ = !gpuRetirementComplete_;
}

void ApplicationShutdownCoordinator::setNativeSurfaceSource(NativeSurfaceSource source) {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(!shuttingDown_);
    nativeSurfaceSource_ = std::move(source);
}

void ApplicationShutdownCoordinator::beginShutdown() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;
    emit shutdownStarted();
    previewController_.beginShutdown();
    // FORMAL AMENDMENT 1 (S1-C): every shape tried in application_shutdown_tests.cpp (a delivered
    // preview, a preview genuinely in flight, playback armed and never paused, a just-completed
    // frame export -- each closed the same way this method is reached) reaches shutdownQuiescent
    // well within 2s, so this is a bounded diagnostic ONLY, never a force-quit fallback: if
    // quiescence still has not arrived 5s after this call, log what the task bridge still
    // considers outstanding so a genuine future occurrence is self-explaining instead of silently
    // unresponsive.
    stuckShutdownDiagnosticTimer_.start(5'000);
    taskUiBridge_.beginShutdown();
    // GPU final-render retirement is the third half of the contract: signal the evaluator owner
    // (non-blocking) and then poll genuine completion from the UI event loop. The UI never blocks
    // on the native owner; retirement completes asynchronously and only then is quiescence
    // published.
    if (gpuRetirementBegin_) {
        gpuRetirementBegin_();
        gpuRetirementPollTimer_.start();
        pollGpuExportRetirement();
    }
    // Native-surface retirement is the second half of the shutdown contract: task quiescence alone
    // is not enough. The service owner keeps pumping (the adapter's UI-thread timer) until the
    // owner publishes a genuine retirement; a refusal keeps the tree alive and shutdownQuiescent
    // un-emitted.
    beginNativeSurfaceRetirement();
    // Re-check once here: a task-free application (blank startup, no preview/asset work yet) has a
    // never-started TaskUiBridge, and beginShutdown() can quiesce it synchronously during the
    // taskUiBridge_.beginShutdown() call above -- BEFORE surfaceRetirementComplete_ is known. That
    // synchronous quiescence must still publish exactly once, so the final state is re-evaluated
    // after both halves are settled. Idempotent: quiescencePublished_ guards a duplicate emission,
    // and a pending/refused native retirement leaves surfaceRetirementComplete_ false.
    publishQuiescenceIfReady();
}

void ApplicationShutdownCoordinator::pollGpuExportRetirement() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (quiescencePublished_ || gpuExportRetirementComplete_) {
        gpuRetirementPollTimer_.stop();
        return;
    }
    if (gpuRetirementComplete_ && gpuRetirementComplete_()) {
        gpuExportRetirementComplete_ = true;
        gpuRetirementPollTimer_.stop();
        publishQuiescenceIfReady();
    }
}

void ApplicationShutdownCoordinator::beginNativeSurfaceRetirement() {
    if (!nativeSurfaceSource_) {
        // CPU-only / no-native platform: the surface half is trivially satisfied, so behavior is
        // exactly the previous task-quiescence-only shutdown.
        surfaceRetirementComplete_ = true;
        return;
    }

    nativeSurfaceRefusalDiagnostic_.clear();
    NativeSurfaceRetirementOptions options;
    options.resumeSurvivorsOnSuccess = false;
    NativeSurfaceRetirementGate::Result synchronous;
    const auto status = surfaceRetirementGate_.begin(
        nativeSurfaceSource_(), [] {},
        [this](const NativeSurfaceRetirementGate::Result& result) {
            surfaceRetirementComplete_ = result.committed;
            if (!result.committed) {
                nativeSurfaceRefusalDiagnostic_ = result.diagnostic;
                qCWarning(applicationShutdownLog).noquote()
                    << QStringLiteral(
                           "Native surface retirement was refused; keeping the window tree alive "
                           "instead of faking quiescence: %1")
                           .arg(QString::fromStdString(result.diagnostic));
            }
            publishQuiescenceIfReady();
        },
        options, &synchronous);

    if (status == NativeSurfaceRetirementGate::StartStatus::CompletedSynchronously) {
        surfaceRetirementComplete_ = synchronous.committed;
        if (!synchronous.committed) {
            nativeSurfaceRefusalDiagnostic_ = synchronous.diagnostic;
        }
    } else if (status == NativeSurfaceRetirementGate::StartStatus::Refused) {
        surfaceRetirementComplete_ = false;
        nativeSurfaceRefusalDiagnostic_ = synchronous.diagnostic;
    }
}

void ApplicationShutdownCoordinator::publishQuiescenceIfReady() {
    if (!shuttingDown_ || quiescencePublished_) {
        return;
    }
    if (!taskQuiescence_ || !surfaceRetirementComplete_ || !gpuExportRetirementComplete_) {
        return;
    }
    quiescencePublished_ = true;
    stuckShutdownDiagnosticTimer_.stop();
    // Publish through the event loop rather than re-entrantly. A task-free application (blank
    // startup whose TaskUiBridge was never started) can reach task quiescence synchronously inside
    // beginShutdown(), which itself may run inside the window's close event; emitting directly
    // would re-enter close/quit handling before that event returns. Queuing keeps exactly one
    // publication while letting the current event finish.
    QTimer::singleShot(0, this, [this] { emit shutdownQuiescent(); });
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
                          "outstanding task(s) of %2 tracked, task bridge shutting down=%3, "
                          "native surface retirement satisfied=%4, surface diagnostic=\"%5\"")
               .arg(outstanding)
               .arg(snapshots.size())
               .arg(taskUiBridge_.isShuttingDown() ? QStringLiteral("true")
                                                   : QStringLiteral("false"))
               .arg(surfaceRetirementComplete_ ? QStringLiteral("true") : QStringLiteral("false"))
               .arg(QString::fromStdString(nativeSurfaceRefusalDiagnostic_));
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
