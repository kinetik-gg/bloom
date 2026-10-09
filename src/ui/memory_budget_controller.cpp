#include <bloom/ui/memory_budget_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QPointer>

#include <limits>
#include <utility>

namespace bloom::ui {
MemoryBudgetController::MemoryBudgetController(runtime::TaskScheduler& scheduler,
                                               TaskUiBridge& bridge,
                                               runtime::MemoryBudgetLedger& ledger, Sampler sampler,
                                               QObject* parent)
    : QObject(parent), scheduler_(scheduler), ledger_(ledger), sampler_(std::move(sampler)) {
    connect(&bridge, &TaskUiBridge::resultsAvailable, this, &MemoryBudgetController::consume);
    timer_.setInterval(5'000);
    connect(&timer_, &QTimer::timeout, this, &MemoryBudgetController::requestSample);
    timer_.start();
    // Queue initial admission until application consumers have connected. This is not a result
    // poll; subsequent delivery comes exclusively through the runtime completion bridge.
    QTimer::singleShot(0, this, &MemoryBudgetController::requestSample);
}

MemoryBudgetController::~MemoryBudgetController() { beginShutdown(); }

void MemoryBudgetController::beginShutdown() {
    if (closed_)
        return;
    closed_ = true;
    timer_.stop();
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
