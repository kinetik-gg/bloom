#pragma once

#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/memory_pressure_monitor.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <QObject>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace bloom::ui {
class TaskUiBridge;

// One application owner submits bounded OS sampling on BlockingIo. Workers retain only the
// neutral sampler; cache callbacks and status observation stay on the owner's/UI thread. A
// MemoryPressureMonitor samples the machine between process samples; a sample short of the RAM
// headroom is queued to this thread and applied at once, so caches evict within one interval.
class MemoryBudgetController final : public QObject {
    Q_OBJECT
  public:
    using Sampler = std::function<runtime::MemoryBudgetSample(const runtime::CancellationToken&)>;
    MemoryBudgetController(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                           runtime::MemoryBudgetLedger& ledger,
                           Sampler sampler = runtime::memoryBudgetSample, QObject* parent = nullptr,
                           std::optional<runtime::MemoryPressureMonitor::Options> pressure =
                               runtime::MemoryPressureMonitor::Options{});
    ~MemoryBudgetController() override;
    void requestSample();
    void beginShutdown();
    [[nodiscard]] bool sampleActive() const noexcept { return active_; }
    [[nodiscard]] std::uint64_t admittedSampleCount() const noexcept { return admitted_; }
    [[nodiscard]] std::uint64_t pressureApplications() const noexcept { return pressureApplied_; }
    // Samples pressure now (a headroom preference change).
    void wakePressureMonitor();
    // Evict-on-demand, callable from any thread (a GpuProcessingPhasePolicy retention reclaimer):
    // queues MemoryBudgetLedger::reclaim of `bytes` (at least kMinimumReclaimBytes) to this
    // object's thread, then requests a host-memory sample at once so the waiting work sees the
    // released memory. Requests arriving while one is queued are coalesced to the largest.
    static constexpr std::size_t kMinimumReclaimBytes = std::size_t{256} * 1024U * 1024U;
    void reclaimForWork(std::size_t bytes);
    // The same as a std::function that is safe to keep beyond this object (it becomes a no-op at
    // shutdown), for GpuProcessingPhasePolicy::addRetentionReclaimer.
    [[nodiscard]] std::function<void(std::size_t)> workReclaimer() const;
    [[nodiscard]] std::uint64_t reclaimApplications() const noexcept { return reclaimApplied_; }

  signals:
    void sampleApplied();
    // A pressure sample was applied and caches were given their shrunken budgets.
    void pressureApplied();

  private:
    void consume();
    void applyPressure(const runtime::MachineMemorySample& sample);
    runtime::TaskScheduler& scheduler_;
    runtime::MemoryBudgetLedger& ledger_;
    Sampler sampler_;
    QTimer timer_;
    using Snapshot = std::unique_ptr<runtime::MemoryBudgetSample>;
    runtime::TaskHandle<Snapshot> handle_;
    bool active_ = false;
    bool closed_ = false;
    std::uint64_t admitted_ = 0;
    std::uint64_t pressureApplied_ = 0;
    std::uint64_t reclaimApplied_ = 0;
    struct ReclaimRequest final {
        std::mutex mutex;
        std::size_t bytes = 0;
        bool queued = false;
        // Cleared at shutdown under `mutex`, so a reclaimer kept by another owner never touches
        // a destroyed controller.
        MemoryBudgetController* owner = nullptr;
    };
    static void queueReclaim(const std::shared_ptr<ReclaimRequest>& request, std::size_t bytes);
    std::shared_ptr<ReclaimRequest> reclaimRequest_ = std::make_shared<ReclaimRequest>();
    // Coalesces queued pressure applications: at most one waits for the UI thread at a time.
    std::shared_ptr<std::atomic<bool>> pressureQueued_ = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<runtime::MemoryPressureMonitor> pressureMonitor_;
};
} // namespace bloom::ui
