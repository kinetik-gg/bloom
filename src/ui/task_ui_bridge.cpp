#include <bloom/ui/task_ui_bridge.hpp>

#include <QThread>

#include <limits>
#include <stdexcept>
#include <utility>

namespace bloom::ui {

TaskUiBridge::TaskUiBridge(runtime::TaskScheduler& scheduler, QObject* parent,
                           const std::chrono::milliseconds pollingInterval)
    : QObject(parent), scheduler_(scheduler), pollingInterval_(pollingInterval) {
    constexpr auto maximumInterval = std::chrono::milliseconds{std::numeric_limits<int>::max()};
    if (pollingInterval_ <= std::chrono::milliseconds::zero() ||
        pollingInterval_ > maximumInterval) {
        throw std::invalid_argument("Task UI polling interval is outside the supported range");
    }

    pollTimer_.setSingleShot(true);
    pollTimer_.setTimerType(Qt::PreciseTimer);
    connect(&pollTimer_, &QTimer::timeout, this, &TaskUiBridge::poll);
}

const std::vector<runtime::TaskSnapshot>& TaskUiBridge::snapshots() const noexcept {
    assertUiThread();
    return snapshots_;
}

const QString& TaskUiBridge::monitorError() const noexcept {
    assertUiThread();
    return monitorError_;
}

bool TaskUiBridge::isRunning() const noexcept {
    assertUiThread();
    return running_;
}

bool TaskUiBridge::isShuttingDown() const noexcept {
    assertUiThread();
    return shuttingDown_;
}

bool TaskUiBridge::requestCancellation(const runtime::TaskId taskId) noexcept {
    assertUiThread();
    return taskId.isValid() && scheduler_.cancel(taskId);
}

void TaskUiBridge::start() {
    assertUiThread();
    if (running_ || quiescenceEmitted_) {
        return;
    }
    running_ = true;
    poll();
}

void TaskUiBridge::stop() {
    assertUiThread();
    if (shuttingDown_ && !quiescenceEmitted_) {
        return;
    }
    running_ = false;
    pollTimer_.stop();
}

void TaskUiBridge::wake() {
    assertUiThread();
    if (!running_) {
        start();
        return;
    }
    if (!pollTimer_.isActive()) {
        scheduleNextPoll();
    }
}

void TaskUiBridge::beginShutdown() {
    assertUiThread();
    if (!shuttingDown_) {
        shuttingDown_ = true;
        scheduler_.beginShutdown();
    }
    wake();
}

void TaskUiBridge::poll() {
    assertUiThread();
    if (!running_) {
        return;
    }

    std::vector<runtime::TaskSnapshot> next;
    bool quiescent = false;
    try {
        next = scheduler_.snapshots();
        quiescent = shuttingDown_ && scheduler_.isQuiescent();
        if (quiescent) {
            // Admission is closed and all workers are idle, so this second copy is stable and
            // guarantees observers see terminal rows before shutdownQuiescent.
            next = scheduler_.snapshots();
        }
    } catch (const std::exception& error) {
        setMonitorError(tr("Jobs could not be refreshed: %1").arg(QString::fromUtf8(error.what())));
        scheduleNextPoll();
        return;
    } catch (...) {
        setMonitorError(tr("Jobs could not be refreshed because of an unexpected error"));
        scheduleNextPoll();
        return;
    }

    snapshots_ = std::move(next);
    setMonitorError({});
    emit snapshotsPolled();

    if (quiescent) {
        running_ = false;
        pollTimer_.stop();
        if (!quiescenceEmitted_) {
            quiescenceEmitted_ = true;
            emit shutdownQuiescent();
        }
        return;
    }
    scheduleNextPoll();
}

void TaskUiBridge::scheduleNextPoll() {
    if (!running_) {
        return;
    }
    pollTimer_.start(static_cast<int>(pollingInterval_.count()));
}

void TaskUiBridge::setMonitorError(QString error) {
    if (monitorError_ == error) {
        return;
    }
    monitorError_ = std::move(error);
    emit monitorErrorChanged();
}

void TaskUiBridge::assertUiThread() const { Q_ASSERT(QThread::currentThread() == thread()); }

} // namespace bloom::ui
