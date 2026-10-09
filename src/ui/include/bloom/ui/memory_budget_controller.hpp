#pragma once

#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>

namespace bloom::ui {
class TaskUiBridge;

// One application owner submits bounded OS sampling on BlockingIo. Workers retain only the
// neutral sampler; cache callbacks and status observation stay on the owner's/UI thread.
class MemoryBudgetController final : public QObject {
    Q_OBJECT
  public:
    using Sampler = std::function<runtime::MemoryBudgetSample(const runtime::CancellationToken&)>;
    MemoryBudgetController(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                           runtime::MemoryBudgetLedger& ledger,
                           Sampler sampler = runtime::memoryBudgetSample,
                           QObject* parent = nullptr);
    ~MemoryBudgetController() override;
    void requestSample();
    void beginShutdown();
    [[nodiscard]] bool sampleActive() const noexcept { return active_; }
    [[nodiscard]] std::uint64_t admittedSampleCount() const noexcept { return admitted_; }

  signals:
    void sampleApplied();

  private:
    void consume();
    runtime::TaskScheduler& scheduler_;
    runtime::MemoryBudgetLedger& ledger_;
    Sampler sampler_;
    QTimer timer_;
    using Snapshot = std::unique_ptr<runtime::MemoryBudgetSample>;
    runtime::TaskHandle<Snapshot> handle_;
    bool active_ = false;
    bool closed_ = false;
    std::uint64_t admitted_ = 0;
};
} // namespace bloom::ui
