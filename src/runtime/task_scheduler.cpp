#include <bloom/runtime/task_scheduler.hpp>

#include "task_scheduler_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <ranges>
#include <utility>

namespace bloom::runtime {
namespace {

constexpr std::uint32_t kFairnessStep = 4;

[[nodiscard]] bool validPriority(const TaskPriority priority) noexcept {
    switch (priority) {
    case TaskPriority::Interactive:
    case TaskPriority::Visible:
    case TaskPriority::Foreground:
    case TaskPriority::Background:
        return true;
    }
    return false;
}

[[nodiscard]] std::uint32_t priorityBase(const TaskPriority priority) noexcept {
    switch (priority) {
    case TaskPriority::Interactive:
        return 0;
    case TaskPriority::Visible:
        return kFairnessStep;
    case TaskPriority::Foreground:
        return kFairnessStep * 2;
    case TaskPriority::Background:
        return kFairnessStep * 3;
    }
    return kFairnessStep * 3;
}

[[nodiscard]] std::uint32_t agingIncrement(const TaskPriority priority) noexcept {
    switch (priority) {
    case TaskPriority::Interactive:
        return 1;
    case TaskPriority::Visible:
        return 2;
    case TaskPriority::Foreground:
        return 3;
    case TaskPriority::Background:
        return 4;
    }
    return 4;
}

[[nodiscard]] bool cancellationRequested(const detail::TaskRecord& record) noexcept {
    const bool taskCancelled = record.cancellation->requested.load(std::memory_order_acquire);
    const bool groupCancelled = record.groupCancellation != nullptr &&
                                record.groupCancellation->requested.load(std::memory_order_acquire);
    return taskCancelled || groupCancelled;
}

[[nodiscard]] TaskSnapshot copySnapshot(const std::shared_ptr<detail::TaskRecord>& record) {
    std::lock_guard lock(record->mutex);
    TaskSnapshot result = record->snapshot;
    result.cancellationRequested = !isTerminal(result.state) && cancellationRequested(*record);
    return result;
}

void normalizeProgress(TaskProgress& next, const TaskProgress& previous) {
    if (next.phase.empty()) {
        next.phase = previous.phase.empty() ? "Working" : previous.phase;
    }
    if (next.phase == previous.phase) {
        next.completed = std::max(next.completed, previous.completed);
    }
    if (next.total.has_value() && *next.total < next.completed) {
        next.total = next.completed;
    }
}

} // namespace

namespace detail {

SchedulerState::SchedulerState(TaskSchedulerConfig schedulerConfig,
                               std::function<void(std::size_t)> startHook)
    : config(schedulerConfig), workerStartHook(std::move(startHook)) {
    cpu.capacity = config.cpuQueueCapacity;
    blockingIo.capacity = config.blockingIoQueueCapacity;
    gpu.capacity = config.gpuPendingQueueCapacity;
    cpu.pending.reserve(cpu.capacity);
    blockingIo.pending.reserve(blockingIo.capacity);
    gpu.pending.reserve(gpu.capacity);
    cancellationQueue.reserve(cpu.capacity + blockingIo.capacity + gpu.capacity);
    cpu.workers.reserve(config.cpuWorkerCount);
    blockingIo.workers.reserve(config.blockingIoWorkerCount);
    terminalRing.resize(config.terminalHistoryCapacity);
    const std::size_t activeAndQueuedCapacity =
        config.cpuQueueCapacity + config.blockingIoQueueCapacity + config.gpuAdmittedStateCapacity +
        config.cpuWorkerCount + config.blockingIoWorkerCount;
    running.reserve(config.cpuWorkerCount + config.blockingIoWorkerCount +
                    config.gpuLiveContinuationCapacity);
    records.reserve(activeAndQueuedCapacity + config.terminalHistoryCapacity);
    groups.reserve(config.groupRegistryCapacity);
}

SchedulerState::~SchedulerState() {
    requestShutdown();
    std::optional<GpuServiceGeneration> generation;
    std::uint64_t attachmentId = 0;
    {
        std::lock_guard lock(mutex);
        generation = gpuGeneration;
        attachmentId = gpuAttachmentId;
    }
    if (generation.has_value()) {
        static_cast<void>(forceGpuShutdown(*generation, attachmentId));
    }
    joinWorkers();
}

void SchedulerState::startWorkers() {
    std::size_t workerIndex = 0;
    try {
        startExecutor(cpu, TaskExecutor::Cpu, config.cpuWorkerCount, workerIndex);
        startExecutor(blockingIo, TaskExecutor::BlockingIo, config.blockingIoWorkerCount,
                      workerIndex);
        if (workerStartHook) {
            workerStartHook(workerIndex);
        }
        finalizerWorker.emplace([this] { finalizerLoop(); });
    } catch (...) {
        requestShutdown();
        joinWorkers();
        throw;
    }
}

void SchedulerState::startExecutor(ExecutorState& target, const TaskExecutor kind,
                                   const std::size_t count, std::size_t& workerIndex) {
    for (std::size_t index = 0; index < count; ++index, ++workerIndex) {
        static_cast<void>(index);
        if (workerStartHook) {
            workerStartHook(workerIndex);
        }
        target.workers.emplace_back([this, kind] { workerLoop(kind); });
    }
}

void SchedulerState::joinWorkers() noexcept {
    cpu.workers.clear();
    blockingIo.workers.clear();
    finalizerWorker.reset();
}

void SchedulerState::finalizerLoop() noexcept {
    while (true) {
        std::shared_ptr<TaskRecord> record;
        {
            std::unique_lock lock(mutex);
            cancellationAvailable.wait(lock,
                                       [&] { return stopping || !cancellationQueue.empty(); });
            if (cancellationQueue.empty()) {
                if (stopping) {
                    return;
                }
                continue;
            }
            record = std::move(cancellationQueue.back());
            cancellationQueue.pop_back();
        }
        finalizeRemoved(record);
    }
}

SchedulerState::ExecutorState& SchedulerState::executor(const TaskExecutor kind) noexcept {
    switch (kind) {
    case TaskExecutor::Cpu:
        return cpu;
    case TaskExecutor::BlockingIo:
        return blockingIo;
    case TaskExecutor::Gpu:
        return gpu;
    }
    return cpu;
}

std::shared_ptr<TaskRecord> SchedulerState::takeNextLocked(ExecutorState& target) {
    const auto selected = std::min_element(
        target.pending.begin(), target.pending.end(), [](const auto& lhs, const auto& rhs) {
            const auto lhsScore = static_cast<std::int64_t>(priorityBase(lhs->snapshot.priority)) -
                                  static_cast<std::int64_t>(lhs->ageCredit);
            const auto rhsScore = static_cast<std::int64_t>(priorityBase(rhs->snapshot.priority)) -
                                  static_cast<std::int64_t>(rhs->ageCredit);
            return lhsScore == rhsScore ? lhs->snapshot.id < rhs->snapshot.id : lhsScore < rhsScore;
        });
    auto record = *selected;
    for (auto& pending : target.pending) {
        const std::uint32_t increment = agingIncrement(pending->snapshot.priority);
        if (pending != record &&
            pending->ageCredit <= std::numeric_limits<std::uint32_t>::max() - increment) {
            pending->ageCredit += increment;
        }
    }
    target.pending.erase(selected);
    return record;
}

void SchedulerState::workerLoop(const TaskExecutor kind) noexcept {
    auto& target = executor(kind);
    while (true) {
        std::shared_ptr<TaskRecord> record;
        {
            std::unique_lock lock(mutex);
            target.available.wait(lock, [&] { return stopping || !target.pending.empty(); });
            if (target.pending.empty()) {
                if (stopping) {
                    return;
                }
                continue;
            }
            record = takeNextLocked(target);
            ++target.active;
            running.push_back(record);
            std::lock_guard recordLock(record->mutex);
            record->snapshot.state = TaskState::Running;
            record->snapshot.startedAt = std::chrono::steady_clock::now();
            record->snapshot.progress.phase = std::move(record->runningPhase);
        }

        groupStarted(record->group);
        TaskState outcome = TaskState::Failed;
        try {
            if (cancellationRequested(*record)) {
                outcome = TaskState::Cancelled;
            } else {
                auto contextState = makeContextState(record);
                TaskContext context(std::move(contextState));
                outcome = record->work->execute(context);
            }
        } catch (...) {
            outcome = TaskState::Failed;
        }
        finishRunning(record, outcome);
    }
}

std::shared_ptr<TaskContextState>
SchedulerState::makeContextState(const std::shared_ptr<TaskRecord>& record) {
    auto context = std::make_shared<TaskContextState>();
    context->taskCancellation = record->cancellation;
    context->groupCancellation = record->groupCancellation;
    context->reportProgress = [record](TaskProgress progress) {
        GroupProgressContribution previous;
        GroupProgressContribution next;
        {
            std::lock_guard lock(record->mutex);
            normalizeProgress(progress, record->snapshot.progress);
            previous = record->groupContribution;
            next = progressContribution(progress);
            record->snapshot.progress = std::move(progress);
            record->groupContribution = next;
        }
        groupProgress(record->group, previous, next);
    };
    context->addDiagnostic = [record,
                              limit = config.diagnosticsPerTask](TaskDiagnostic diagnostic) {
        std::lock_guard lock(record->mutex);
        if (record->snapshot.diagnostics.size() < limit) {
            record->snapshot.diagnostics.push_back(std::move(diagnostic));
        }
    };
    return context;
}

void SchedulerState::finishRunning(const std::shared_ptr<TaskRecord>& record,
                                   TaskState outcome) noexcept {
    {
        std::lock_guard lock(record->mutex);
        if (record->completionClaimed) {
            return;
        }
        record->completionClaimed = true;
        if (!isTerminal(outcome)) {
            outcome = TaskState::Failed;
        }
        if (outcome == TaskState::Succeeded && cancellationRequested(*record)) {
            outcome = TaskState::Cancelled;
        }
    }

    const TaskState publishedOutcome = record->work->publish(outcome);
    groupFinished(record->group, TaskState::Running, record->groupContribution);
    publishTerminalSnapshot(record, publishedOutcome);
    {
        std::lock_guard lock(mutex);
        auto& target = executor(record->snapshot.executor);
        const auto runningRecord = std::ranges::find(running, record);
        if (runningRecord != running.end()) {
            running.erase(runningRecord);
        }
        if (target.active > 0) {
            --target.active;
        }
        retainTerminalLocked(record->snapshot.id);
    }
    cpu.available.notify_all();
    blockingIo.available.notify_all();
}

void SchedulerState::finalizeRemoved(const std::shared_ptr<TaskRecord>& record) noexcept {
    const TaskState publishedOutcome = record->work->publish(TaskState::Cancelled);
    groupFinished(record->group, TaskState::Queued, record->groupContribution);
    publishTerminalSnapshot(record, publishedOutcome);
    {
        std::lock_guard lock(mutex);
        releaseGpuAccountingLocked(record);
        if (finalizing > 0) {
            --finalizing;
        }
        retainTerminalLocked(record->snapshot.id);
    }
    cpu.available.notify_all();
    blockingIo.available.notify_all();
}

void SchedulerState::publishTerminalSnapshot(const std::shared_ptr<TaskRecord>& record,
                                             const TaskState outcome) noexcept {
    std::lock_guard lock(record->mutex);
    record->snapshot.state = outcome;
    record->snapshot.finishedAt = std::chrono::steady_clock::now();
    record->cancellation->completed.store(true, std::memory_order_release);
}

void SchedulerState::retainTerminalLocked(const TaskId id) noexcept {
    if (terminalCount == terminalRing.size()) {
        const TaskId expired = terminalRing[terminalHead];
        records.erase(expired);
        terminalRing[terminalHead] = id;
        terminalHead = (terminalHead + 1) % terminalRing.size();
        return;
    }
    const std::size_t position = (terminalHead + terminalCount) % terminalRing.size();
    terminalRing[position] = id;
    ++terminalCount;
}

bool SchedulerState::reprioritize(const TaskId id, const TaskPriority priority) noexcept {
    if (!validPriority(priority)) {
        return false;
    }
    std::shared_ptr<const GpuTaskWakeSink> wake;
    {
        std::lock_guard lock(mutex);
        const auto found = records.find(id);
        if (found == records.end()) {
            return false;
        }
        const auto& record = found->second;
        std::lock_guard recordLock(record->mutex);
        if (record->snapshot.state != TaskState::Queued || record->completionClaimed) {
            return false;
        }
        record->snapshot.priority = priority;
        record->ageCredit = 0;
        if (record->snapshot.executor == TaskExecutor::Gpu) {
            wake = gpuWakeLocked();
        } else {
            executor(record->snapshot.executor).available.notify_one();
        }
    }
    if (wake != nullptr) {
        try {
            (*wake)();
        } catch (...) {
            return true;
        }
    }
    return true;
}

std::vector<TaskSnapshot> SchedulerState::snapshots() const {
    std::vector<std::shared_ptr<TaskRecord>> retained;
    {
        std::lock_guard lock(mutex);
        retained.reserve(records.size());
        for (const auto& [id, record] : records) {
            static_cast<void>(id);
            retained.push_back(record);
        }
    }
    std::vector<TaskSnapshot> result;
    result.reserve(retained.size());
    for (const auto& record : retained) {
        result.push_back(copySnapshot(record));
    }
    std::ranges::sort(result, {}, &TaskSnapshot::id);
    return result;
}

std::optional<TaskSnapshot> SchedulerState::snapshot(const TaskId id) const {
    std::shared_ptr<TaskRecord> record;
    {
        std::lock_guard lock(mutex);
        const auto found = records.find(id);
        if (found == records.end()) {
            return std::nullopt;
        }
        record = found->second;
    }
    return copySnapshot(record);
}

void SchedulerState::requestShutdown() noexcept {
    std::shared_ptr<const GpuTaskWakeSink> wake;
    {
        std::lock_guard lock(mutex);
        if (stopping) {
            return;
        }
        accepting = false;
        stopping = true;
        for (const auto& record : running) {
            std::lock_guard recordLock(record->mutex);
            if (!record->completionClaimed && !isTerminal(record->snapshot.state)) {
                record->cancellation->requested.store(true, std::memory_order_release);
            }
        }
        for (auto* target : {&cpu, &blockingIo, &gpu}) {
            for (const auto& record : target->pending) {
                std::lock_guard recordLock(record->mutex);
                record->cancellation->requested.store(true, std::memory_order_release);
                record->completionClaimed = true;
                cancellationQueue.push_back(record);
                ++finalizing;
            }
            target->pending.clear();
        }
        for (const auto& [id, weakGroup] : groups) {
            static_cast<void>(id);
            if (const auto group = weakGroup.lock()) {
                group->cancellation->requested.store(true, std::memory_order_release);
            }
        }
        wake = gpuWakeLocked();
    }
    cpu.available.notify_all();
    blockingIo.available.notify_all();
    cancellationAvailable.notify_all();
    if (wake != nullptr) {
        try {
            (*wake)();
        } catch (...) {
            return;
        }
    }
}

bool SchedulerState::isAccepting() const noexcept {
    std::lock_guard lock(mutex);
    return accepting;
}

bool SchedulerState::isQuiescent() const noexcept {
    std::lock_guard lock(mutex);
    return cpu.pending.empty() && blockingIo.pending.empty() && gpu.pending.empty() &&
           cpu.active == 0 && blockingIo.active == 0 && gpuLiveContinuations == 0 &&
           finalizing == 0;
}

} // namespace detail
} // namespace bloom::runtime
