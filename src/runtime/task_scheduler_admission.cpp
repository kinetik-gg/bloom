#include <bloom/runtime/task_scheduler.hpp>

#include "task_scheduler_internal.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace bloom::runtime {
namespace {

[[nodiscard]] TaskDiagnostic submissionDiagnostic(std::string code, std::string summary) {
    return {.code = std::move(code),
            .severity = DiagnosticSeverity::Error,
            .summary = std::move(summary),
            .detail = {},
            .suggestedAction = {}};
}

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

[[nodiscard]] bool validExecutor(const TaskExecutor executor) noexcept {
    return executor == TaskExecutor::Cpu || executor == TaskExecutor::BlockingIo;
}

[[nodiscard]] bool validOwnerKind(const TaskOwnerKind kind) noexcept {
    switch (kind) {
    case TaskOwnerKind::Application:
    case TaskOwnerKind::Project:
    case TaskOwnerKind::Composition:
    case TaskOwnerKind::PanelRequest:
    case TaskOwnerKind::Export:
        return true;
    }
    return false;
}

[[nodiscard]] bool coalesces(const detail::TaskRecord& record,
                             const TaskRequest& request) noexcept {
    return request.coalescingKey.has_value() && record.coalescingKey.has_value() &&
           request.owner == record.snapshot.owner &&
           *request.coalescingKey == *record.coalescingKey;
}

void wakeBestEffort(const std::shared_ptr<const GpuTaskWakeSink>& wake) noexcept {
    if (wake == nullptr) {
        return;
    }
    try {
        (*wake)();
    } catch (...) {
        return;
    }
}

} // namespace

namespace detail {

SchedulerState::Admission SchedulerState::admit(TaskRequest request,
                                                std::shared_ptr<CancellationState> cancellation,
                                                std::shared_ptr<TaskWork> work) {
    if (request.name.empty() || !request.owner.isValid() || !validOwnerKind(request.owner.kind) ||
        !validPriority(request.priority) || !validExecutor(request.executor) || work == nullptr ||
        !work->isValid() || (request.coalescingKey.has_value() && request.coalescingKey->empty())) {
        return {.status = TaskSubmissionStatus::InvalidRequest,
                .id = {},
                .diagnostic = submissionDiagnostic("bloom.runtime.invalid-task-request",
                                                   "The task request is incomplete")};
    }

    auto record = std::make_shared<TaskRecord>();
    record->snapshot = {
        .id = {},
        .name = std::move(request.name),
        .owner = request.owner,
        .priority = request.priority,
        .executor = request.executor,
        .state = TaskState::Queued,
        .groupId = request.groupId,
        .sourceVersion = request.sourceVersion,
        .progress = {.phase = "Queued", .subphase = {}, .completed = 0, .total = std::nullopt},
        .diagnostics = {},
        .cancellationRequested = false,
        .queuedAt = std::chrono::steady_clock::now(),
        .startedAt = std::nullopt,
        .finishedAt = std::nullopt};
    record->snapshot.diagnostics.reserve(config.diagnosticsPerTask);
    record->coalescingKey = request.coalescingKey;
    record->cancellation = std::move(cancellation);
    record->work = std::move(work);

    std::shared_ptr<TaskGroupControl> group;
    std::vector<std::shared_ptr<TaskRecord>> superseded;
    std::shared_ptr<const GpuTaskWakeSink> gpuWakeRequest;
    TaskId id;
    {
        std::lock_guard lock(mutex);
        purgeExpiredGroupsLocked();
        if (!accepting) {
            return {.status = TaskSubmissionStatus::ShuttingDown,
                    .id = {},
                    .diagnostic = submissionDiagnostic("bloom.runtime.shutting-down",
                                                       "The task service is shutting down")};
        }
        if (request.groupId.has_value()) {
            const auto found = groups.find(*request.groupId);
            if (found != groups.end()) {
                group = found->second.lock();
            }
            if (group == nullptr || group->snapshot.owner != request.owner) {
                return {.status = TaskSubmissionStatus::UnknownGroup,
                        .id = {},
                        .diagnostic = submissionDiagnostic("bloom.runtime.unknown-task-group",
                                                           "The task group is unavailable")};
            }
            if (group->cancellation->requested.load(std::memory_order_acquire)) {
                return {.status = TaskSubmissionStatus::CancelledGroup,
                        .id = {},
                        .diagnostic = submissionDiagnostic("bloom.runtime.cancelled-task-group",
                                                           "The task group is already cancelled")};
            }
        }

        auto& target = executor(request.executor);
        const std::size_t projectedRemoval = countPendingMatchesLocked(request, target);
        if (target.pending.size() - projectedRemoval >= target.capacity) {
            return {.status = TaskSubmissionStatus::QueueFull,
                    .id = {},
                    .diagnostic = submissionDiagnostic("bloom.runtime.queue-full",
                                                       "The selected task queue is full")};
        }
        if (nextTaskId == 0) {
            return {.status = TaskSubmissionStatus::IdExhausted,
                    .id = {},
                    .diagnostic = submissionDiagnostic("bloom.runtime.task-id-exhausted",
                                                       "No additional task IDs are available")};
        }

        const std::size_t supersededCount =
            request.coalescingKey.has_value() ? countPendingMatchesLocked(request, cpu) +
                                                    countPendingMatchesLocked(request, blockingIo) +
                                                    countPendingMatchesLocked(request, gpu)
                                              : 0;
        superseded.reserve(supersededCount);

        id = TaskId::fromRaw(nextTaskId);
        record->snapshot.id = id;
        record->group = group;
        record->groupCancellation = group == nullptr ? nullptr : group->cancellation;
        const auto [position, inserted] = records.emplace(id, record);
        static_cast<void>(position);
        if (!inserted) {
            throw std::logic_error("Bloom task IDs must be unique");
        }

        if (request.coalescingKey.has_value()) {
            removeCoalescedLocked(request, cpu, superseded);
            removeCoalescedLocked(request, blockingIo, superseded);
            removeCoalescedLocked(request, gpu, superseded);
            gpuWakeRequest = gpuWakeLocked();
        }
        target.pending.push_back(record);
        groupAdded(group);
        cancelRunningMatchesLocked(request);
        advanceTaskIdLocked();
    }

    for (const auto& previous : superseded) {
        finalizeRemoved(previous);
    }
    wakeBestEffort(gpuWakeRequest);
    executor(record->snapshot.executor).available.notify_one();
    return {.status = TaskSubmissionStatus::Accepted, .id = id, .diagnostic = std::nullopt};
}

SchedulerState::GroupAdmission SchedulerState::createGroup(TaskOwner owner, std::string name) {
    if (!owner.isValid() || !validOwnerKind(owner.kind) || name.empty()) {
        return {.status = TaskSubmissionStatus::InvalidRequest,
                .group = nullptr,
                .diagnostic = submissionDiagnostic("bloom.runtime.invalid-task-group",
                                                   "The task group request is incomplete")};
    }

    auto control = std::make_shared<TaskGroupControl>();
    control->snapshot.name = std::move(name);
    control->snapshot.owner = owner;
    control->snapshot.progress.phase = control->snapshot.name;
    control->scheduler = weak_from_this();

    std::lock_guard lock(mutex);
    purgeExpiredGroupsLocked();
    if (!accepting) {
        return {.status = TaskSubmissionStatus::ShuttingDown,
                .group = nullptr,
                .diagnostic = submissionDiagnostic("bloom.runtime.shutting-down",
                                                   "The task service is shutting down")};
    }
    if (groups.size() >= config.groupRegistryCapacity) {
        return {.status = TaskSubmissionStatus::GroupRegistryFull,
                .group = nullptr,
                .diagnostic = submissionDiagnostic("bloom.runtime.group-registry-full",
                                                   "The task group registry is full")};
    }
    if (nextGroupId == 0) {
        return {.status = TaskSubmissionStatus::IdExhausted,
                .group = nullptr,
                .diagnostic = submissionDiagnostic("bloom.runtime.group-id-exhausted",
                                                   "No additional task group IDs are available")};
    }

    const TaskGroupId id = TaskGroupId::fromRaw(nextGroupId);
    control->snapshot.id = id;
    const auto [position, inserted] = groups.emplace(id, control);
    static_cast<void>(position);
    if (!inserted) {
        throw std::logic_error("Bloom task group IDs must be unique");
    }
    nextGroupId = nextGroupId == std::numeric_limits<std::uint64_t>::max() ? 0 : nextGroupId + 1;
    return {.status = TaskSubmissionStatus::Accepted,
            .group = std::move(control),
            .diagnostic = std::nullopt};
}

bool SchedulerState::cancelTask(const TaskId id) noexcept {
    std::shared_ptr<TaskRecord> removed;
    bool acceptedCancellation = false;
    std::shared_ptr<const GpuTaskWakeSink> wake;
    {
        std::lock_guard lock(mutex);
        const auto found = records.find(id);
        if (found == records.end()) {
            return false;
        }
        const auto& record = found->second;
        std::lock_guard recordLock(record->mutex);
        if (record->completionClaimed || isTerminal(record->snapshot.state)) {
            return false;
        }
        record->cancellation->requested.store(true, std::memory_order_release);
        acceptedCancellation = true;
        if (record->snapshot.state == TaskState::Queued && removePendingLocked(record)) {
            record->completionClaimed = true;
            ++finalizing;
            removed = record;
        }
        if (record->snapshot.executor == TaskExecutor::Gpu) {
            wake = gpuWakeLocked();
        }
    }
    if (removed != nullptr) {
        finalizeRemoved(removed);
    }
    wakeBestEffort(wake);
    return acceptedCancellation;
}

std::size_t SchedulerState::cancelOwner(const TaskOwner owner) {
    if (!owner.isValid() || !validOwnerKind(owner.kind)) {
        return 0;
    }
    std::vector<std::shared_ptr<TaskRecord>> removed;
    std::size_t cancellationCount = 0;
    std::shared_ptr<const GpuTaskWakeSink> wake;
    {
        std::lock_guard lock(mutex);
        removed.reserve(countQueuedOwnerLocked(owner));
        for (auto iterator = groups.begin(); iterator != groups.end();) {
            const auto group = iterator->second.lock();
            if (group == nullptr) {
                iterator = groups.erase(iterator);
                continue;
            }
            if (group->snapshot.owner == owner) {
                group->cancellation->requested.store(true, std::memory_order_release);
            }
            ++iterator;
        }
        cancellationCount += removeQueuedOwnerLocked(owner, removed);
        cancellationCount += cancelRunningOwnerLocked(owner);
        if (cancellationCount != 0) {
            wake = gpuWakeLocked();
        }
    }
    for (const auto& record : removed) {
        finalizeRemoved(record);
    }
    wakeBestEffort(wake);
    return cancellationCount;
}

void SchedulerState::cancelGroup(const std::shared_ptr<TaskGroupControl>& group) {
    if (group == nullptr) {
        return;
    }
    std::vector<std::shared_ptr<TaskRecord>> removed;
    std::shared_ptr<const GpuTaskWakeSink> wake;
    {
        std::lock_guard lock(mutex);
        const TaskGroupId id = group->snapshot.id;
        removed.reserve(countQueuedGroupLocked(id));
        group->cancellation->requested.store(true, std::memory_order_release);
        static_cast<void>(removeQueuedGroupLocked(id, removed));
        static_cast<void>(cancelRunningGroupLocked(id));
        wake = gpuWakeLocked();
    }
    for (const auto& record : removed) {
        finalizeRemoved(record);
    }
    wakeBestEffort(wake);
}

bool SchedulerState::removePendingLocked(const std::shared_ptr<TaskRecord>& record) noexcept {
    auto& pending = executor(record->snapshot.executor).pending;
    const auto found = std::ranges::find(pending, record);
    if (found == pending.end()) {
        return false;
    }
    pending.erase(found);
    return true;
}

std::size_t SchedulerState::countPendingMatchesLocked(const TaskRequest& request,
                                                      const ExecutorState& target) noexcept {
    return static_cast<std::size_t>(
        std::count_if(target.pending.begin(), target.pending.end(),
                      [&request](const auto& pending) { return coalesces(*pending, request); }));
}

void SchedulerState::removeCoalescedLocked(
    const TaskRequest& request, ExecutorState& target,
    std::vector<std::shared_ptr<TaskRecord>>& superseded) noexcept {
    auto iterator = target.pending.begin();
    while (iterator != target.pending.end()) {
        const auto& record = *iterator;
        if (!coalesces(*record, request)) {
            ++iterator;
            continue;
        }
        std::lock_guard recordLock(record->mutex);
        record->cancellation->requested.store(true, std::memory_order_release);
        record->completionClaimed = true;
        superseded.push_back(record);
        ++finalizing;
        if (&target == &gpu) {
            releaseGpuAccountingLocked(record);
        }
        iterator = target.pending.erase(iterator);
    }
}

void SchedulerState::cancelRunningMatchesLocked(const TaskRequest& request) noexcept {
    if (!request.coalescingKey.has_value()) {
        return;
    }
    for (const auto& record : running) {
        if (!coalesces(*record, request)) {
            continue;
        }
        std::lock_guard recordLock(record->mutex);
        if (record->snapshot.state == TaskState::Running && !record->completionClaimed) {
            record->cancellation->requested.store(true, std::memory_order_release);
        }
    }
}

std::size_t SchedulerState::countQueuedOwnerLocked(const TaskOwner owner) const noexcept {
    const auto countIn = [&owner](const ExecutorState& target) {
        return static_cast<std::size_t>(std::count_if(
            target.pending.begin(), target.pending.end(),
            [&owner](const auto& record) { return record->snapshot.owner == owner; }));
    };
    return countIn(cpu) + countIn(blockingIo) + countIn(gpu);
}

std::size_t SchedulerState::countQueuedGroupLocked(const TaskGroupId groupId) const noexcept {
    const auto countIn = [groupId](const ExecutorState& target) {
        return static_cast<std::size_t>(std::count_if(
            target.pending.begin(), target.pending.end(),
            [groupId](const auto& record) { return record->snapshot.groupId == groupId; }));
    };
    return countIn(cpu) + countIn(blockingIo) + countIn(gpu);
}

std::size_t SchedulerState::removeQueuedOwnerLocked(
    const TaskOwner owner, std::vector<std::shared_ptr<TaskRecord>>& removed) noexcept {
    std::size_t count = 0;
    for (auto* target : {&cpu, &blockingIo, &gpu}) {
        auto iterator = target->pending.begin();
        while (iterator != target->pending.end()) {
            const auto& record = *iterator;
            if (record->snapshot.owner != owner) {
                ++iterator;
                continue;
            }
            std::lock_guard recordLock(record->mutex);
            record->cancellation->requested.store(true, std::memory_order_release);
            record->completionClaimed = true;
            removed.push_back(record);
            ++finalizing;
            ++count;
            iterator = target->pending.erase(iterator);
        }
    }
    return count;
}

std::size_t SchedulerState::removeQueuedGroupLocked(
    const TaskGroupId groupId, std::vector<std::shared_ptr<TaskRecord>>& removed) noexcept {
    std::size_t count = 0;
    for (auto* target : {&cpu, &blockingIo, &gpu}) {
        auto iterator = target->pending.begin();
        while (iterator != target->pending.end()) {
            const auto& record = *iterator;
            if (record->snapshot.groupId != groupId) {
                ++iterator;
                continue;
            }
            std::lock_guard recordLock(record->mutex);
            record->cancellation->requested.store(true, std::memory_order_release);
            record->completionClaimed = true;
            removed.push_back(record);
            ++finalizing;
            ++count;
            iterator = target->pending.erase(iterator);
        }
    }
    return count;
}

std::size_t SchedulerState::cancelRunningOwnerLocked(const TaskOwner owner) noexcept {
    std::size_t count = 0;
    for (const auto& record : running) {
        if (record->snapshot.owner != owner) {
            continue;
        }
        std::lock_guard recordLock(record->mutex);
        if (record->snapshot.state == TaskState::Running && !record->completionClaimed) {
            record->cancellation->requested.store(true, std::memory_order_release);
            ++count;
        }
    }
    return count;
}

std::size_t SchedulerState::cancelRunningGroupLocked(const TaskGroupId groupId) noexcept {
    std::size_t count = 0;
    for (const auto& record : running) {
        if (record->snapshot.groupId != groupId) {
            continue;
        }
        std::lock_guard recordLock(record->mutex);
        if (record->snapshot.state == TaskState::Running && !record->completionClaimed) {
            record->cancellation->requested.store(true, std::memory_order_release);
            ++count;
        }
    }
    return count;
}

void SchedulerState::purgeExpiredGroupsLocked() noexcept {
    for (auto iterator = groups.begin(); iterator != groups.end();) {
        if (iterator->second.expired()) {
            iterator = groups.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void SchedulerState::advanceTaskIdLocked() noexcept {
    nextTaskId = nextTaskId == std::numeric_limits<std::uint64_t>::max() ? 0 : nextTaskId + 1;
}

} // namespace detail

TaskScheduler::TaskScheduler(TaskSchedulerConfig config) : TaskScheduler(config, {}) {}

TaskScheduler::TaskScheduler(TaskSchedulerConfig config, WorkerStartHook workerStartHook) {
    if (!config.isValid()) {
        throw std::invalid_argument("TaskScheduler configuration exceeds its bounded limits");
    }
    auto state = std::make_shared<detail::SchedulerState>(config, std::move(workerStartHook));
    state->startWorkers();
    state_ = std::move(state);
}

TaskScheduler::~TaskScheduler() = default;

TaskScheduler::ErasedSubmission
TaskScheduler::submitErased(TaskRequest request,
                            std::shared_ptr<detail::CancellationState> cancellation,
                            std::shared_ptr<detail::TaskWork> work) {
    auto admission = state_->admit(std::move(request), std::move(cancellation), std::move(work));
    return {.status = admission.status,
            .id = admission.id,
            .diagnostic = std::move(admission.diagnostic)};
}

TaskGroupSubmission TaskScheduler::createGroup(TaskOwner owner, std::string name) {
    auto admission = state_->createGroup(owner, std::move(name));
    TaskGroupSubmission result{
        .status = admission.status, .handle = {}, .diagnostic = std::move(admission.diagnostic)};
    if (admission.status == TaskSubmissionStatus::Accepted) {
        result.handle = TaskGroupHandle(std::move(admission.group));
    }
    return result;
}

bool TaskScheduler::cancel(const TaskId id) noexcept { return state_->cancelTask(id); }

std::size_t TaskScheduler::cancelOwner(const TaskOwner owner) { return state_->cancelOwner(owner); }

bool TaskScheduler::reprioritize(const TaskId id, const TaskPriority priority) noexcept {
    return state_->reprioritize(id, priority);
}

std::vector<TaskSnapshot> TaskScheduler::snapshots() const { return state_->snapshots(); }

std::optional<TaskSnapshot> TaskScheduler::snapshot(const TaskId id) const {
    return state_->snapshot(id);
}

void TaskScheduler::beginShutdown() noexcept { state_->requestShutdown(); }

bool TaskScheduler::isAccepting() const noexcept { return state_->isAccepting(); }

bool TaskScheduler::isQuiescent() const noexcept { return state_->isQuiescent(); }

namespace detail {

void requestTaskCancellation(const std::weak_ptr<SchedulerState>& scheduler, const TaskId id,
                             const std::shared_ptr<CancellationState>& cancellation) noexcept {
    if (const auto state = scheduler.lock()) {
        static_cast<void>(state->cancelTask(id));
        return;
    }
    if (cancellation != nullptr && !cancellation->completed.load(std::memory_order_acquire)) {
        cancellation->requested.store(true, std::memory_order_release);
    }
}

void requestGroupCancellation(const std::shared_ptr<TaskGroupControl>& group) {
    if (group == nullptr) {
        return;
    }
    if (const auto state = group->scheduler.lock()) {
        state->cancelGroup(group);
        return;
    }
    group->cancellation->requested.store(true, std::memory_order_release);
}

} // namespace detail
} // namespace bloom::runtime
