#pragma once

#include <bloom/runtime/task_scheduler.hpp>

#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>
#include <memory>
#include <vector>

namespace bloom::ui {

class PreviewFrameTrace;

class TaskUiBridge final : public QObject {
    Q_OBJECT

  public:
    static constexpr std::chrono::milliseconds defaultPollingInterval{100};
    explicit TaskUiBridge(runtime::TaskScheduler& scheduler, QObject* parent = nullptr,
                          std::chrono::milliseconds pollingInterval = defaultPollingInterval);
    ~TaskUiBridge() override;

    [[nodiscard]] const std::vector<runtime::TaskSnapshot>& snapshots() const noexcept;
    [[nodiscard]] const QString& monitorError() const noexcept;
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] bool isShuttingDown() const noexcept;
    [[nodiscard]] std::uint64_t completionWakeCount() const noexcept;
    [[nodiscard]] std::chrono::milliseconds pollingInterval() const noexcept {
        return pollingInterval_;
    }
    void setFrameTrace(std::shared_ptr<PreviewFrameTrace> trace) { trace_ = std::move(trace); }

    [[nodiscard]] bool requestCancellation(runtime::TaskId taskId) noexcept;

  public slots:
    void start();
    void stop();
    void wake();
    void beginShutdown();

  signals:
    void resultsAvailable();
    void snapshotsPolled();
    void monitorErrorChanged();
    void shutdownQuiescent();

  private slots:
    void poll();
    void drainResults();

  private:
    void scheduleNextPoll();
    void setMonitorError(QString error);
    void assertUiThread() const;
    struct CompletionDelivery;

    runtime::TaskScheduler& scheduler_;
    std::shared_ptr<PreviewFrameTrace> trace_;
    QTimer pollTimer_;
    std::chrono::milliseconds pollingInterval_;
    std::vector<runtime::TaskSnapshot> snapshots_;
    QString monitorError_;
    bool running_ = false;
    bool shuttingDown_ = false;
    bool quiescenceEmitted_ = false;
    std::uint64_t completionWakes_ = 0;
    QString completionError_;
    std::unique_ptr<CompletionDelivery> completion_;
};

} // namespace bloom::ui
