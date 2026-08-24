#pragma once

#include <bloom/runtime/task_scheduler.hpp>

#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>
#include <vector>

namespace bloom::ui {

class TaskUiBridge final : public QObject {
    Q_OBJECT

  public:
    explicit TaskUiBridge(runtime::TaskScheduler& scheduler, QObject* parent = nullptr,
                          std::chrono::milliseconds pollingInterval = std::chrono::milliseconds{
                              100});

    [[nodiscard]] const std::vector<runtime::TaskSnapshot>& snapshots() const noexcept;
    [[nodiscard]] const QString& monitorError() const noexcept;
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] bool isShuttingDown() const noexcept;

    [[nodiscard]] bool requestCancellation(runtime::TaskId taskId) noexcept;

  public slots:
    void start();
    void stop();
    void wake();
    void beginShutdown();

  signals:
    void snapshotsPolled();
    void monitorErrorChanged();
    void shutdownQuiescent();

  private slots:
    void poll();

  private:
    void scheduleNextPoll();
    void setMonitorError(QString error);
    void assertUiThread() const;

    runtime::TaskScheduler& scheduler_;
    QTimer pollTimer_;
    std::chrono::milliseconds pollingInterval_;
    std::vector<runtime::TaskSnapshot> snapshots_;
    QString monitorError_;
    bool running_ = false;
    bool shuttingDown_ = false;
    bool quiescenceEmitted_ = false;
};

} // namespace bloom::ui
