#include <bloom/ui/memory_budget_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QPointer>

#include <limits>
#include <utility>

namespace bloom::ui {
MemoryBudgetController::MemoryBudgetController(
    runtime::TaskScheduler& scheduler, TaskUiBridge& bridge, runtime::MemoryBudgetLedger& ledger,
    Sampler sampler, QObject* parent,
    std::optional<runtime::MemoryPressureMonitor::Options> pressure)
    : QObject(parent), scheduler_(scheduler), ledger_(ledger), sampler_(std::move(sampler)) {
    connect(&bridge, &TaskUiBridge::resultsAvailable, this, &MemoryBudgetController::consume);
    timer_.setInterval(5'000);
    connect(&timer_, &QTimer::timeout, this, &MemoryBudgetController::requestSample);
    timer_.start();
    // Queue initial admission until application consumers have connected. This is not a result
    // poll; subsequent delivery comes exclusively through the runtime completion bridge.
    QTimer::singleShot(0, this, &MemoryBudgetController::requestSample);
    if (pressure) {
        // The monitor thread never touches cache owners: it queues the sample to this object's
        // thread. beginShutdown() joins the monitor before this object dies, and Qt drops a
        // queued call whose context object was destroyed.
        pressureMonitor_ = std::make_unique<runtime::MemoryPressureMonitor>(
            ledger_,
            [this, queued = pressureQueued_](const runtime::MachineMemorySample& sample) {
                if (queued->exchange(true))
                    return;
                if (!QMetaObject::invokeMethod(
                        this,
                        [this, queued, sample] {
                            queued->store(false);
                            applyPressure(sample);
                        },
                        Qt::QueuedConnection))
                    queued->store(false);
            },
            *pressure);
    }
}

void MemoryBudgetController::applyPressure(const runtime::MachineMemorySample& sample) {
    if (closed_)
        return;
    static_cast<void>(ledger_.applyPressureSample(sample));
    ++pressureApplied_;
    emit pressureApplied();
}

void MemoryBudgetController::wakePressureMonitor() {
    if (pressureMonitor_)
        pressureMonitor_->wake();
}

MemoryBudgetController::~MemoryBudgetController() { beginShutdown(); }

void MemoryBudgetController::beginShutdown() {
    if (closed_)
        return;
    closed_ = true;
    timer_.stop();
    if (pressureMonitor_)
        pressureMonitor_->stop();
    if (active_)
        handle_.cancel();
}

void MemoryBudgetController::requestSample() {
    if (closed_ || active_ || !scheduler_.isAccepting())
        return;
    runtime::TaskRequest request(
        "Sample memory cache allowance",
        {runtime::TaskOwnerKind::Application, runtime::TaskOwnerId::fromRaw(1)},
        runtime::TaskPriority::Background, runtime::TaskExecutor::BlockingIo);
    auto submission = scheduler_.submit<Snapshot>(
        std::move(request), [sampler = sampler_](runtime::TaskContext& context) {
            if (context.cancellation().isCancellationRequested())
                return runtime::TaskResult<Snapshot>::cancelled();
            auto sample = sampler(context.cancellation());
            if (context.cancellation().isCancellationRequested())
                return runtime::TaskResult<Snapshot>::cancelled();
            return runtime::TaskResult<Snapshot>::succeeded(
                std::make_unique<runtime::MemoryBudgetSample>(std::move(sample)));
        });
    if (!submission.accepted())
        return; // No worker/result is retained; the bounded normal cadence may retry admission.
    handle_ = std::move(submission.handle);
    active_ = true;
    if (admitted_ != std::numeric_limits<std::uint64_t>::max())
        ++admitted_;
}

void MemoryBudgetController::consume() {
    if (!active_)
        return;
    auto result = handle_.tryTakeResult();
    if (!result)
        return;
    active_ = false;
    handle_ = {};
    if (closed_)
        return;
    runtime::MemoryBudgetSample sample;
    sample.process.sampledAt = runtime::MemoryBudgetLedger::Clock::now();
    if (result->state() == runtime::TaskState::Succeeded) {
        auto value = std::move(*result).value();
        if (value.has_value() && *value)
            sample = std::move(**value);
    }
    const QPointer guard(this);
    static_cast<void>(ledger_.applySample(std::move(sample)));
    if (guard)
        emit sampleApplied(); // An observer may delete this owner; access no member afterward.
}
} // namespace bloom::ui
