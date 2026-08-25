#include "task_scheduler_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <thread>
#include <utility>

namespace bloom::runtime {
namespace {

constexpr std::uint64_t kProgressScale = 1'000;
constexpr std::size_t kMaxCpuWorkers = 256;
constexpr std::size_t kMaxBlockingIoWorkers = 64;
constexpr std::size_t kMaxQueueCapacity = 65'536;
constexpr std::size_t kMaxTerminalHistory = 65'536;
constexpr std::size_t kMaxDiagnosticsPerTask = 1'024;
constexpr std::size_t kMaxGroupRegistry = 16'384;
constexpr std::size_t kMaxGpuPendingQueue = 65'536;
constexpr std::size_t kMaxGpuAdmittedStates = 65'536;
constexpr std::size_t kMaxGpuLiveContinuations = 16'384;
constexpr std::size_t kMaxGpuQueuedCommandBytes = std::size_t{1} << 30U;
constexpr std::size_t kMaxGpuRequestOwnedBytes = std::size_t{1} << 30U;

struct ScaledFraction {
    std::uint64_t quotient = 0;
    std::uint64_t remainder = 0;
};

[[nodiscard]] ScaledFraction addFractions(const ScaledFraction lhs, const ScaledFraction rhs,
                                          const std::uint64_t divisor) noexcept {
    ScaledFraction sum{.quotient = lhs.quotient + rhs.quotient, .remainder = 0};
    if (lhs.remainder >= divisor - rhs.remainder) {
        ++sum.quotient;
        sum.remainder = lhs.remainder - (divisor - rhs.remainder);
    } else {
        sum.remainder = lhs.remainder + rhs.remainder;
    }
    return sum;
}

[[nodiscard]] std::uint64_t scaledRatio(const std::uint64_t completed,
                                        const std::uint64_t total) noexcept {
    if (total == 0) {
        return 0;
    }
    const std::uint64_t boundedCompleted = std::min(completed, total);
    ScaledFraction result;
    ScaledFraction term{.quotient = boundedCompleted / total,
                        .remainder = boundedCompleted % total};
    std::uint64_t multiplier = kProgressScale;
    while (multiplier != 0) {
        if ((multiplier & 1U) != 0U) {
            result = addFractions(result, term, total);
        }
        multiplier >>= 1U;
        if (multiplier != 0) {
            term = addFractions(term, term, total);
        }
    }
    return std::min(result.quotient, kProgressScale);
}

void updateGroupProgressLocked(detail::TaskGroupControl& group) noexcept {
    auto& aggregate = group.snapshot.progress;
    const auto maxTasks = std::numeric_limits<std::uint64_t>::max() / kProgressScale;
    if (group.indeterminateTasks != 0 || group.snapshot.totalTasks > maxTasks) {
        aggregate.completed = 0;
        aggregate.total.reset();
        return;
    }

    aggregate.completed =
        static_cast<std::uint64_t>(group.snapshot.finishedTasks) * kProgressScale +
        group.activeContributionUnits;
    aggregate.total = static_cast<std::uint64_t>(group.snapshot.totalTasks) * kProgressScale;
}

} // namespace

namespace detail {

GroupProgressContribution progressContribution(const TaskProgress& progress) noexcept {
    if (!progress.total.has_value() || *progress.total == 0) {
        return {};
    }
    return {.units = scaledRatio(progress.completed, *progress.total), .determinate = true};
}

void groupAdded(const std::shared_ptr<TaskGroupControl>& group) noexcept {
    if (group == nullptr) {
        return;
    }
    std::lock_guard lock(group->mutex);
    ++group->snapshot.totalTasks;
    ++group->snapshot.queuedTasks;
    ++group->indeterminateTasks;
    updateGroupProgressLocked(*group);
}

void groupStarted(const std::shared_ptr<TaskGroupControl>& group) noexcept {
    if (group == nullptr) {
        return;
    }
    std::lock_guard lock(group->mutex);
    if (group->snapshot.queuedTasks > 0) {
        --group->snapshot.queuedTasks;
    }
    ++group->snapshot.runningTasks;
}

void groupProgress(const std::shared_ptr<TaskGroupControl>& group,
                   const GroupProgressContribution previous,
                   const GroupProgressContribution next) noexcept {
    if (group == nullptr) {
        return;
    }
    std::lock_guard lock(group->mutex);
    if (previous.determinate) {
        group->activeContributionUnits -= previous.units;
    } else if (group->indeterminateTasks > 0) {
        --group->indeterminateTasks;
    }
    if (next.determinate) {
        group->activeContributionUnits += next.units;
    } else {
        ++group->indeterminateTasks;
    }
    updateGroupProgressLocked(*group);
}

void groupFinished(const std::shared_ptr<TaskGroupControl>& group, const TaskState previousState,
                   const GroupProgressContribution contribution) noexcept {
    if (group == nullptr) {
        return;
    }
    std::lock_guard lock(group->mutex);
    if (previousState == TaskState::Queued && group->snapshot.queuedTasks > 0) {
        --group->snapshot.queuedTasks;
    } else if (previousState == TaskState::Running && group->snapshot.runningTasks > 0) {
        --group->snapshot.runningTasks;
    }
    if (contribution.determinate) {
        group->activeContributionUnits -= contribution.units;
    } else if (group->indeterminateTasks > 0) {
        --group->indeterminateTasks;
    }
    ++group->snapshot.finishedTasks;
    updateGroupProgressLocked(*group);
}

} // namespace detail

TaskSchedulerConfig TaskSchedulerConfig::defaults() noexcept {
    TaskSchedulerConfig config;
    const unsigned int available = std::thread::hardware_concurrency();
    const auto useful = available > 1 ? static_cast<std::size_t>(available - 1U) : std::size_t{1};
    config.cpuWorkerCount = std::clamp<std::size_t>(useful, 1, 16);
    config.blockingIoWorkerCount = 2;
    return config;
}

bool TaskSchedulerConfig::isValid() const noexcept {
    return cpuWorkerCount > 0 && cpuWorkerCount <= kMaxCpuWorkers && blockingIoWorkerCount > 0 &&
           blockingIoWorkerCount <= kMaxBlockingIoWorkers && cpuQueueCapacity > 0 &&
           cpuQueueCapacity <= kMaxQueueCapacity && blockingIoQueueCapacity > 0 &&
           blockingIoQueueCapacity <= kMaxQueueCapacity && gpuPendingQueueCapacity > 0 &&
           gpuPendingQueueCapacity <= kMaxGpuPendingQueue && gpuAdmittedStateCapacity > 0 &&
           gpuAdmittedStateCapacity <= kMaxGpuAdmittedStates &&
           gpuPendingQueueCapacity <= gpuAdmittedStateCapacity && gpuLiveContinuationCapacity > 0 &&
           gpuLiveContinuationCapacity <= kMaxGpuLiveContinuations &&
           gpuLiveContinuationCapacity <= gpuAdmittedStateCapacity &&
           gpuQueuedCommandByteCapacity > 0 &&
           gpuQueuedCommandByteCapacity <= kMaxGpuQueuedCommandBytes &&
           gpuRequestOwnedByteCapacity > 0 &&
           gpuRequestOwnedByteCapacity <= kMaxGpuRequestOwnedBytes && terminalHistoryCapacity > 0 &&
           terminalHistoryCapacity <= kMaxTerminalHistory && diagnosticsPerTask > 0 &&
           diagnosticsPerTask <= kMaxDiagnosticsPerTask && groupRegistryCapacity > 0 &&
           groupRegistryCapacity <= kMaxGroupRegistry;
}

bool GpuTaskAdmission::isValid() const noexcept {
    return queuedCommandBytes <= kMaxGpuQueuedCommandBytes &&
           requestOwnedBytes <= kMaxGpuRequestOwnedBytes;
}

CancellationToken::CancellationToken(
    std::shared_ptr<const detail::CancellationState> taskState,
    std::shared_ptr<const detail::CancellationState> groupState) noexcept
    : taskState_(std::move(taskState)), groupState_(std::move(groupState)) {}

bool CancellationToken::isCancellationRequested() const noexcept {
    const bool taskCancelled =
        taskState_ != nullptr && taskState_->requested.load(std::memory_order_acquire);
    const bool groupCancelled =
        groupState_ != nullptr && groupState_->requested.load(std::memory_order_acquire);
    return taskCancelled || groupCancelled;
}

TaskContext::TaskContext(std::shared_ptr<detail::TaskContextState> state) noexcept
    : state_(std::move(state)), cancellation_(state_->taskCancellation, state_->groupCancellation) {
}

const CancellationToken& TaskContext::cancellation() const noexcept { return cancellation_; }

bool TaskContext::isCancellationRequested() const noexcept {
    return cancellation_.isCancellationRequested();
}

void TaskContext::reportProgress(TaskProgress progress) {
    state_->reportProgress(std::move(progress));
}

void TaskContext::addDiagnostic(TaskDiagnostic diagnostic) {
    state_->addDiagnostic(std::move(diagnostic));
}

TaskGroupHandle::TaskGroupHandle(std::shared_ptr<detail::TaskGroupControl> control) noexcept
    : control_(std::move(control)) {}

TaskGroupId TaskGroupHandle::id() const noexcept {
    return control_ == nullptr ? TaskGroupId{} : control_->snapshot.id;
}

bool TaskGroupHandle::isValid() const noexcept { return id().isValid(); }

TaskOwner TaskGroupHandle::owner() const noexcept {
    return control_ == nullptr ? TaskOwner{} : control_->snapshot.owner;
}

bool TaskGroupHandle::cancellationRequested() const noexcept {
    return control_ != nullptr && control_->cancellation->requested.load(std::memory_order_acquire);
}

std::optional<TaskGroupSnapshot> TaskGroupHandle::snapshot() const {
    if (control_ == nullptr) {
        return std::nullopt;
    }
    std::lock_guard lock(control_->mutex);
    TaskGroupSnapshot result = control_->snapshot;
    result.cancellationRequested = cancellationRequested();
    return result;
}

void TaskGroupHandle::cancel() const { detail::requestGroupCancellation(control_); }

} // namespace bloom::runtime
