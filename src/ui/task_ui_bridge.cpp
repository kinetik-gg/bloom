#include <bloom/ui/preview_frame_trace.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QDebug>
#include <QThread>

#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace bloom::ui {

// Only this UI adapter owns the QObject reference. Runtime subscriptions retain neutral serial/
// condition state. The waiter never evaluates or consumes results and has two stop-aware waits;
// destruction joins this idle adapter, not a task, GPU owner or any heavy worker.
struct TaskUiBridge::CompletionDelivery final {
    explicit CompletionDelivery(TaskUiBridge& owner, runtime::TaskScheduler& scheduler)
        : bridge(owner) {
        auto subscribed = scheduler.subscribeCompletions();
        if (subscribed.status != runtime::TaskCompletionSubscribeStatus::Accepted)
            throw std::runtime_error("Task completion subscription could not be admitted (status " +
                                     std::to_string(static_cast<unsigned>(subscribed.status)) +
                                     ')');
        subscription = std::move(subscribed.subscription);
        waiter = std::jthread([this](const std::stop_token& stop) {
            // Zero is lossless across subscription admission and thread startup. Existing terminal
            // history causes at most one initial drain, never one queued callback per old task.
            std::uint64_t observed = 0;
            while (const auto changed = subscription.waitForChange(observed, stop)) {
                {
                    std::lock_guard lock(mutex);
                    if (drainedSerial >= *changed) {
                        observed = drainedSerial;
                        continue;
                    }
                }
                if (!requestDrain())
                    return;
                std::unique_lock lock(mutex);
                if (!acknowledged.wait(lock, stop, [&] { return !pending; }))
                    return;
                observed = drainedSerial;
            }
            if (!stop.stop_requested()) {
                const auto exhausted = subscription.state() ==
                                       runtime::TaskCompletionSubscriptionState::SerialExhausted;
                QMetaObject::invokeMethod(
                    &bridge,
                    [&owner = bridge, exhausted] {
                        owner.completionError_ =
                            exhausted ? tr("Task completion notification serial is exhausted")
                                      : tr("Task completion notifications closed unexpectedly");
                        owner.setMonitorError(owner.completionError_);
                    },
                    Qt::QueuedConnection);
            }
        });
    }

    ~CompletionDelivery() {
        waiter.request_stop();
        if (waiter.joinable())
            waiter.join();
        subscription.close();
    }

    bool requestDrain() {
        {
            std::lock_guard lock(mutex);
            if (pending)
                return true;
            pending = true;
        }
        if (QMetaObject::invokeMethod(&bridge, &TaskUiBridge::drainResults, Qt::QueuedConnection))
            return true;
        {
            std::lock_guard lock(mutex);
            pending = false;
        }
        acknowledged.notify_all();
        subscription.close();
        qCritical("Task completion UI delivery could not queue a result drain");
        return false;
    }

    std::uint64_t acknowledge() {
        const auto serial = subscription.serial();
        {
            std::lock_guard lock(mutex);
            drainedSerial = serial;
            pending = false;
        }
        acknowledged.notify_all();
        return serial;
    }

    TaskUiBridge& bridge;
    runtime::TaskCompletionSubscription subscription;
    std::mutex mutex;
    std::condition_variable_any acknowledged;
    bool pending = false;
    std::uint64_t drainedSerial = 0;
    std::jthread waiter;
};

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
    completion_ = std::make_unique<CompletionDelivery>(*this, scheduler_);
}

TaskUiBridge::~TaskUiBridge() { completion_.reset(); }

std::uint64_t TaskUiBridge::completionWakeCount() const noexcept {
    assertUiThread();
    return completionWakes_;
}

void TaskUiBridge::drainResults() {
    assertUiThread();
    // Read and acknowledge BEFORE observers run. A completion during observer delivery queues the
    // next bounded wake, and an observer can delete this bridge without any access after the emit.
    const auto serial = completion_->acknowledge();
    ++completionWakes_;
    if (trace_)
        trace_->noteCompletionWake(serial);
    emit resultsAvailable();
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
    completion_->requestDrain();
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
    completion_->requestDrain();
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
    if (trace_)
        trace_->noteBridgePoll();

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
    if (!completionError_.isEmpty())
        error = completionError_;
    if (monitorError_ == error) {
        return;
    }
    monitorError_ = std::move(error);
    emit monitorErrorChanged();
}

void TaskUiBridge::assertUiThread() const { Q_ASSERT(QThread::currentThread() == thread()); }

} // namespace bloom::ui
