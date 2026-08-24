#include <bloom/ui/application_shutdown_coordinator.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QCoreApplication>
#include <QEvent>
#include <QThread>

namespace bloom::ui {

ApplicationShutdownCoordinator::ApplicationShutdownCoordinator(
    CompositionPreviewController& previewController, TaskUiBridge& taskUiBridge, QObject* parent)
    : QObject(parent), previewController_(previewController), taskUiBridge_(taskUiBridge) {
    connect(&taskUiBridge_, &TaskUiBridge::shutdownQuiescent, this, [this] {
        Q_ASSERT(QThread::currentThread() == thread());
        if (!shuttingDown_ || quiescencePublished_) {
            return;
        }
        quiescencePublished_ = true;
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
