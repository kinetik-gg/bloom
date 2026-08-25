#include <bloom/runtime/task_gpu_executor.hpp>

#include "task_scheduler_internal.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace bloom::runtime {
namespace {

[[nodiscard]] TaskDiagnostic gpuDiagnostic(std::string code, std::string summary,
                                           std::string detail = {}) {
    return {.code = std::move(code),
            .severity = DiagnosticSeverity::Error,
            .summary = std::move(summary),
            .detail = std::move(detail),
            .suggestedAction = "Review GPU service diagnostics and retry the operation."};
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

[[nodiscard]] TaskDiagnostic normalizedDeviceLostDiagnostic(TaskDiagnostic diagnostic) {
    if (!diagnostic.code.empty() && !diagnostic.summary.empty()) {
        diagnostic.severity = DiagnosticSeverity::Error;
        return diagnostic;
    }
    return gpuDiagnostic("bloom.runtime.gpu-device-lost", "The GPU device was lost",
                         "The active GPU generation cannot complete its admitted work.");
}

} // namespace

namespace detail {

SchedulerState::Admission SchedulerState::admitGpu(TaskRequest request,
                                                   const GpuServiceGeneration generation,
                                                   const GpuTaskAdmission admission,
                                                   std::shared_ptr<CancellationState> cancellation,
                                                   std::shared_ptr<TaskWork> work,
                                                   std::shared_ptr<GpuTaskWork> gpuWork) {
    if (request.name.empty() || !request.owner.isValid() || !validOwnerKind(request.owner.kind) ||
        !validPriority(request.priority) || request.executor != TaskExecutor::Gpu ||
        !generation.isValid() || !admission.isValid() || work == nullptr || gpuWork == nullptr ||
        !work->isValid() || (request.coalescingKey.has_value() && request.coalescingKey->empty())) {
        return {.status = TaskSubmissionStatus::InvalidRequest,
                .id = {},
                .diagnostic = gpuDiagnostic("bloom.runtime.invalid-gpu-task-request",
                                            "The GPU task request is incomplete")};
    }

    auto record = std::make_shared<TaskRecord>();
    record->snapshot = {
        .id = {},
        .name = std::move(request.name),
        .owner = request.owner,
        .priority = request.priority,
        .executor = TaskExecutor::Gpu,
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
    record->gpuWork = std::move(gpuWork);
    record->gpuAdmission = admission;
    record->gpuGeneration = generation;
    record->gpuCommandBytesQueued = true;

    std::shared_ptr<TaskGroupControl> group;
    std::vector<std::shared_ptr<TaskRecord>> superseded;
    std::shared_ptr<const GpuTaskWakeSink> wake;
    TaskId id;
    {
        std::lock_guard lock(mutex);
        purgeExpiredGroupsLocked();
        if (!accepting) {
            return {.status = TaskSubmissionStatus::ShuttingDown,
                    .id = {},
                    .diagnostic = gpuDiagnostic("bloom.runtime.shutting-down",
                                                "The task service is shutting down")};
        }
        if (!gpuGeneration.has_value() || *gpuGeneration != generation || gpuAttachmentId == 0) {
            return {.status = TaskSubmissionStatus::ExecutorUnavailable,
                    .id = {},
                    .diagnostic = gpuDiagnostic("bloom.runtime.gpu-executor-unavailable",
                                                "The requested GPU generation is unavailable")};
        }
        if (request.groupId.has_value()) {
            const auto found = groups.find(*request.groupId);
            if (found != groups.end()) {
                group = found->second.lock();
            }
            if (group == nullptr || group->snapshot.owner != request.owner) {
                return {.status = TaskSubmissionStatus::UnknownGroup,
                        .id = {},
                        .diagnostic = gpuDiagnostic("bloom.runtime.unknown-task-group",
                                                    "The task group is unavailable")};
            }
            if (group->cancellation->requested.load(std::memory_order_acquire)) {
                return {.status = TaskSubmissionStatus::CancelledGroup,
                        .id = {},
                        .diagnostic = gpuDiagnostic("bloom.runtime.cancelled-task-group",
                                                    "The task group is already cancelled")};
            }
        }

        std::size_t projectedCount = 0;
        std::size_t projectedCommandBytes = 0;
        std::size_t projectedRequestBytes = 0;
        if (request.coalescingKey.has_value()) {
            for (const auto& pending : gpu.pending) {
                if (coalesces(*pending, request)) {
                    ++projectedCount;
                    projectedCommandBytes += pending->gpuAdmission.queuedCommandBytes;
                    projectedRequestBytes += pending->gpuAdmission.requestOwnedBytes;
                }
            }
        }

        const std::size_t remainingPending = gpu.pending.size() - projectedCount;
        const std::size_t remainingAdmitted = gpuAdmittedStates - projectedCount;
        const std::size_t remainingCommandBytes = gpuQueuedCommandBytes - projectedCommandBytes;
        const std::size_t remainingRequestBytes = gpuRequestOwnedBytes - projectedRequestBytes;
        const bool capacityExceeded =
            remainingPending >= config.gpuPendingQueueCapacity ||
            remainingAdmitted >= config.gpuAdmittedStateCapacity ||
            admission.queuedCommandBytes >
                config.gpuQueuedCommandByteCapacity - remainingCommandBytes ||
            admission.requestOwnedBytes >
                config.gpuRequestOwnedByteCapacity - remainingRequestBytes;
        if (capacityExceeded) {
            return {.status = TaskSubmissionStatus::QueueFull,
                    .id = {},
                    .diagnostic = gpuDiagnostic("bloom.runtime.gpu-admission-full",
                                                "The bounded GPU task capacity is full")};
        }
        if (nextTaskId == 0) {
            return {.status = TaskSubmissionStatus::IdExhausted,
                    .id = {},
                    .diagnostic = gpuDiagnostic("bloom.runtime.task-id-exhausted",
                                                "No additional task IDs are available")};
        }

        const std::size_t supersededCount =
            request.coalescingKey.has_value()
                ? countPendingMatchesLocked(request, cpu) +
                      countPendingMatchesLocked(request, blockingIo) + projectedCount
                : 0;
        superseded.reserve(supersededCount);

        id = TaskId::fromRaw(nextTaskId);
        record->snapshot.id = id;
        record->group = group;
        record->groupCancellation = group == nullptr ? nullptr : group->cancellation;
        record->gpuAttachmentId = gpuAttachmentId;
        const auto [position, inserted] = records.emplace(id, record);
        static_cast<void>(position);
        if (!inserted) {
            throw std::logic_error("Bloom task IDs must be unique");
        }

        if (request.coalescingKey.has_value()) {
            removeCoalescedLocked(request, cpu, superseded);
            removeCoalescedLocked(request, blockingIo, superseded);
            removeCoalescedLocked(request, gpu, superseded);
        }
        gpu.pending.push_back(record);
        ++gpuAdmittedStates;
        gpuQueuedCommandBytes += admission.queuedCommandBytes;
        gpuRequestOwnedBytes += admission.requestOwnedBytes;
        groupAdded(group);
        cancelRunningMatchesLocked(request);
        wake = gpuWakeLocked();
        advanceTaskIdLocked();
    }

    for (const auto& previous : superseded) {
        finalizeRemoved(previous);
    }
    wakeBestEffort(wake);
    return {.status = TaskSubmissionStatus::Accepted, .id = id, .diagnostic = std::nullopt};
}

SchedulerState::GpuAttachmentAdmission
SchedulerState::attachGpu(const GpuServiceGeneration generation, GpuTaskWakeSink wakeSink) {
    if (!generation.isValid() || !wakeSink) {
        return {.status = TaskSubmissionStatus::InvalidRequest,
                .attachmentId = 0,
                .diagnostic = gpuDiagnostic("bloom.runtime.invalid-gpu-attachment",
                                            "The GPU service attachment is incomplete")};
    }
    auto retainedWake = std::make_shared<const GpuTaskWakeSink>(std::move(wakeSink));

    std::lock_guard lock(mutex);
    if (!accepting) {
        return {.status = TaskSubmissionStatus::ShuttingDown,
                .attachmentId = 0,
                .diagnostic = gpuDiagnostic("bloom.runtime.shutting-down",
                                            "The task service is shutting down")};
    }
    if (gpuGeneration.has_value() || gpuAttachmentId != 0 ||
        (lastGpuGeneration.isValid() && generation <= lastGpuGeneration)) {
        return {.status = TaskSubmissionStatus::ExecutorUnavailable,
                .attachmentId = 0,
                .diagnostic =
                    gpuDiagnostic("bloom.runtime.gpu-attachment-unavailable",
                                  "A newer exclusive GPU service generation is required")};
    }
    if (nextGpuAttachmentId == 0) {
        return {.status = TaskSubmissionStatus::IdExhausted,
                .attachmentId = 0,
                .diagnostic = gpuDiagnostic("bloom.runtime.gpu-attachment-id-exhausted",
                                            "No additional GPU attachment IDs are available")};
    }

    const std::uint64_t attachmentId = nextGpuAttachmentId;
    nextGpuAttachmentId = nextGpuAttachmentId == std::numeric_limits<std::uint64_t>::max()
                              ? 0
                              : nextGpuAttachmentId + 1;
    gpuGeneration = generation;
    lastGpuGeneration = generation;
    gpuAttachmentId = attachmentId;
    gpuServiceThread.reset();
    gpuWake = std::move(retainedWake);
    return {.status = TaskSubmissionStatus::Accepted,
            .attachmentId = attachmentId,
            .diagnostic = std::nullopt};
}

GpuDispatchStatus SchedulerState::dispatchGpu(const GpuServiceGeneration generation,
                                              const std::uint64_t attachmentId) noexcept {
    std::shared_ptr<TaskRecord> record;
    std::shared_ptr<const std::function<void()>> transitionHook;
    GpuCompletionCore core;
    {
        std::lock_guard lock(mutex);
        if (!gpuGeneration.has_value() || *gpuGeneration != generation ||
            gpuAttachmentId != attachmentId || attachmentId == 0) {
            return GpuDispatchStatus::ExecutorUnavailable;
        }
        if (stopping) {
            return GpuDispatchStatus::ShuttingDown;
        }
        const std::thread::id currentThread = std::this_thread::get_id();
        if (gpuServiceThread.has_value() && *gpuServiceThread != currentThread) {
            return GpuDispatchStatus::WrongThread;
        }
        if (!gpuServiceThread.has_value()) {
            gpuServiceThread = currentThread;
        }
        if (gpu.pending.empty() || gpuLiveContinuations >= config.gpuLiveContinuationCapacity) {
            return GpuDispatchStatus::Empty;
        }

        record = takeNextLocked(gpu);
        removeGpuQueuedAccountingLocked(record);
        ++gpuLiveContinuations;
        record->gpuWasDispatched = true;
        running.push_back(record);
        {
            std::lock_guard recordLock(record->mutex);
            record->snapshot.state = TaskState::Running;
            record->snapshot.startedAt = std::chrono::steady_clock::now();
            record->snapshot.progress.phase = "GPU service";
        }
        core = {.scheduler = weak_from_this(),
                .id = record->snapshot.id,
                .generation = generation,
                .attachmentId = attachmentId,
                .serviceThread = currentThread,
                .taskCancellation = record->cancellation,
                .groupCancellation = record->groupCancellation};
        groupStarted(record->group);
        transitionHook = gpuDispatchTransitionHook;
    }

    if (transitionHook != nullptr) {
        try {
            (*transitionHook)();
        } catch (...) {
            transitionHook.reset();
        }
    }
    try {
        auto contextState = makeContextState(record);
        TaskContext context(std::move(contextState));
        record->gpuWork->dispatch(core, context);
    } catch (...) {
        finishGpuToken(core, TaskState::Failed);
    }
    return GpuDispatchStatus::Dispatched;
}

void SchedulerState::completeGpu(const GpuCompletionCore& core, TaskState outcome) noexcept {
    std::shared_ptr<TaskRecord> record;
    std::shared_ptr<const GpuTaskWakeSink> wake;
    {
        std::lock_guard lock(mutex);
        const auto found = records.find(core.id);
        if (found == records.end()) {
            return;
        }
        record = found->second;
        std::lock_guard recordLock(record->mutex);
        if (record->snapshot.executor != TaskExecutor::Gpu || record->completionClaimed ||
            record->snapshot.state != TaskState::Running ||
            record->gpuGeneration != core.generation ||
            record->gpuAttachmentId != core.attachmentId) {
            return;
        }
        if (std::this_thread::get_id() != core.serviceThread) {
            outcome = TaskState::Failed;
        }
        if (!isTerminal(outcome)) {
            outcome = TaskState::Failed;
        }
        if (outcome == TaskState::Succeeded &&
            (record->cancellation->requested.load(std::memory_order_acquire) ||
             (record->groupCancellation != nullptr &&
              record->groupCancellation->requested.load(std::memory_order_acquire)))) {
            outcome = TaskState::Cancelled;
        }
        record->completionClaimed = true;
        const auto runningRecord = std::ranges::find(running, record);
        if (runningRecord != running.end()) {
            running.erase(runningRecord);
        }
        ++finalizing;
        wake = gpuWakeLocked();
    }

    const TaskState publishedOutcome = record->work->publish(outcome);
    groupFinished(record->group, TaskState::Running, record->groupContribution);
    publishTerminalSnapshot(record, publishedOutcome);
    {
        std::lock_guard lock(mutex);
        releaseGpuAccountingLocked(record);
        if (finalizing > 0) {
            --finalizing;
        }
        retainTerminalLocked(record->snapshot.id);
    }
    wakeBestEffort(wake);
}

void SchedulerState::appendGpuDiagnostics(const GpuCompletionCore& core,
                                          const std::vector<TaskDiagnostic>& diagnostics) noexcept {
    try {
        std::shared_ptr<TaskRecord> record;
        {
            std::lock_guard lock(mutex);
            const auto found = records.find(core.id);
            if (found == records.end()) {
                return;
            }
            record = found->second;
        }
        std::lock_guard recordLock(record->mutex);
        if (record->completionClaimed || record->gpuGeneration != core.generation ||
            record->gpuAttachmentId != core.attachmentId) {
            return;
        }
        for (const auto& diagnostic : diagnostics) {
            if (record->snapshot.diagnostics.size() >= config.diagnosticsPerTask) {
                break;
            }
            record->snapshot.diagnostics.push_back(diagnostic);
        }
    } catch (...) {
        return;
    }
}

void SchedulerState::removeGpuQueuedAccountingLocked(
    const std::shared_ptr<TaskRecord>& record) noexcept {
    if (record->snapshot.executor != TaskExecutor::Gpu || !record->gpuCommandBytesQueued) {
        return;
    }
    const std::size_t bytes = record->gpuAdmission.queuedCommandBytes;
    gpuQueuedCommandBytes = bytes > gpuQueuedCommandBytes ? 0 : gpuQueuedCommandBytes - bytes;
    record->gpuCommandBytesQueued = false;
}

void SchedulerState::releaseGpuAccountingLocked(
    const std::shared_ptr<TaskRecord>& record) noexcept {
    if (record->snapshot.executor != TaskExecutor::Gpu || record->gpuAccountingReleased) {
        return;
    }
    removeGpuQueuedAccountingLocked(record);
    if (record->gpuWasDispatched && gpuLiveContinuations > 0) {
        --gpuLiveContinuations;
    }
    if (gpuAdmittedStates > 0) {
        --gpuAdmittedStates;
    }
    const std::size_t bytes = record->gpuAdmission.requestOwnedBytes;
    gpuRequestOwnedBytes = bytes > gpuRequestOwnedBytes ? 0 : gpuRequestOwnedBytes - bytes;
    record->gpuAccountingReleased = true;
}

std::shared_ptr<const GpuTaskWakeSink> SchedulerState::gpuWakeLocked() const noexcept {
    return gpuWake;
}

void SchedulerState::finalizeForcedGpu(const std::shared_ptr<TaskRecord>& record,
                                       const TaskState previousState, const TaskState outcome,
                                       const TaskDiagnostic& diagnostic) noexcept {
    bool diagnosticStaged = true;
    try {
        record->gpuWork->stageForcedFailure(diagnostic);
        {
            std::lock_guard recordLock(record->mutex);
            if (record->snapshot.diagnostics.size() < config.diagnosticsPerTask) {
                record->snapshot.diagnostics.push_back(diagnostic);
            }
        }
    } catch (...) {
        diagnosticStaged = false;
    }
    static_cast<void>(diagnosticStaged);
    const TaskState publishedOutcome = record->work->publish(outcome);
    groupFinished(record->group, previousState, record->groupContribution);
    publishTerminalSnapshot(record, publishedOutcome);
    {
        std::lock_guard lock(mutex);
        releaseGpuAccountingLocked(record);
        if (finalizing > 0) {
            --finalizing;
        }
        retainTerminalLocked(record->snapshot.id);
    }
}

void SchedulerState::terminalizeAllGpu(const TaskState outcome,
                                       const TaskDiagnostic& diagnostic) noexcept {
    while (true) {
        std::shared_ptr<TaskRecord> record;
        TaskState previousState = TaskState::Queued;
        {
            std::lock_guard lock(mutex);
            while (!gpu.pending.empty() && record == nullptr) {
                record = gpu.pending.back();
                gpu.pending.pop_back();
                std::lock_guard recordLock(record->mutex);
                if (record->completionClaimed) {
                    record.reset();
                    continue;
                }
                record->completionClaimed = true;
                ++finalizing;
            }
            if (record == nullptr) {
                const auto runningRecord = std::ranges::find_if(running, [](const auto& candidate) {
                    return candidate->snapshot.executor == TaskExecutor::Gpu;
                });
                if (runningRecord != running.end()) {
                    record = *runningRecord;
                    running.erase(runningRecord);
                    std::lock_guard recordLock(record->mutex);
                    if (!record->completionClaimed) {
                        record->completionClaimed = true;
                        previousState = TaskState::Running;
                        ++finalizing;
                    } else {
                        record.reset();
                    }
                }
            }
        }
        if (record == nullptr) {
            return;
        }
        finalizeForcedGpu(record, previousState, outcome, diagnostic);
    }
}

bool SchedulerState::reportGpuDeviceLost(const GpuServiceGeneration generation,
                                         const std::uint64_t attachmentId,
                                         TaskDiagnostic diagnostic) noexcept {
    try {
        diagnostic = normalizedDeviceLostDiagnostic(std::move(diagnostic));
    } catch (...) {
        diagnostic.severity = DiagnosticSeverity::Error;
    }
    {
        std::lock_guard lock(mutex);
        if (!gpuGeneration.has_value() || *gpuGeneration != generation ||
            gpuAttachmentId != attachmentId || attachmentId == 0 ||
            (gpuServiceThread.has_value() && *gpuServiceThread != std::this_thread::get_id())) {
            return false;
        }
        gpuGeneration.reset();
        gpuAttachmentId = 0;
        gpuServiceThread.reset();
        gpuWake.reset();
    }
    terminalizeAllGpu(TaskState::Failed, diagnostic);
    return true;
}

bool SchedulerState::forceGpuShutdown(const GpuServiceGeneration generation,
                                      const std::uint64_t attachmentId) noexcept {
    TaskDiagnostic diagnostic;
    try {
        diagnostic = gpuDiagnostic("bloom.runtime.gpu-shutdown-fallback",
                                   "GPU work was cancelled by the shutdown fallback");
    } catch (...) {
        diagnostic.severity = DiagnosticSeverity::Error;
    }
    {
        std::lock_guard lock(mutex);
        if (!gpuGeneration.has_value() || *gpuGeneration != generation ||
            gpuAttachmentId != attachmentId || attachmentId == 0) {
            return false;
        }
        gpuGeneration.reset();
        gpuAttachmentId = 0;
        gpuServiceThread.reset();
        gpuWake.reset();
        for (const auto& record : running) {
            if (record->snapshot.executor == TaskExecutor::Gpu) {
                record->cancellation->requested.store(true, std::memory_order_release);
            }
        }
    }
    terminalizeAllGpu(TaskState::Cancelled, diagnostic);
    return true;
}

void SchedulerState::detachGpu(const GpuServiceGeneration generation,
                               const std::uint64_t attachmentId) noexcept {
    bool shutdown = false;
    {
        std::lock_guard lock(mutex);
        if (!gpuGeneration.has_value() || *gpuGeneration != generation ||
            gpuAttachmentId != attachmentId || attachmentId == 0) {
            return;
        }
        shutdown = stopping;
        gpuGeneration.reset();
        gpuAttachmentId = 0;
        gpuServiceThread.reset();
        gpuWake.reset();
    }
    TaskDiagnostic diagnostic;
    try {
        diagnostic = gpuDiagnostic("bloom.runtime.gpu-executor-detached",
                                   shutdown ? "GPU work was cancelled during service shutdown"
                                            : "The GPU service detached before its work completed");
    } catch (...) {
        diagnostic.severity = DiagnosticSeverity::Error;
    }
    terminalizeAllGpu(shutdown ? TaskState::Cancelled : TaskState::Failed, diagnostic);
}

bool SchedulerState::gpuAttachmentActive(const GpuServiceGeneration generation,
                                         const std::uint64_t attachmentId) const noexcept {
    std::lock_guard lock(mutex);
    return gpuGeneration.has_value() && *gpuGeneration == generation &&
           gpuAttachmentId == attachmentId && attachmentId != 0;
}

void SchedulerState::setGpuDispatchTransitionHookForTesting(std::function<void()> hook) {
    auto retained = hook ? std::make_shared<const std::function<void()>>(std::move(hook)) : nullptr;
    std::lock_guard lock(mutex);
    gpuDispatchTransitionHook = std::move(retained);
}

void finishGpuToken(const GpuCompletionCore& core, const TaskState outcome) noexcept {
    if (const auto scheduler = core.scheduler.lock()) {
        scheduler->completeGpu(core, outcome);
    }
}

void appendGpuDiagnostics(const GpuCompletionCore& core,
                          const std::vector<TaskDiagnostic>& diagnostics) noexcept {
    if (const auto scheduler = core.scheduler.lock()) {
        scheduler->appendGpuDiagnostics(core, diagnostics);
    }
}

} // namespace detail

TaskScheduler::ErasedSubmission TaskScheduler::submitGpuErased(
    TaskRequest request, const GpuServiceGeneration generation, const GpuTaskAdmission admission,
    std::shared_ptr<detail::CancellationState> cancellation, std::shared_ptr<detail::TaskWork> work,
    std::shared_ptr<detail::GpuTaskWork> gpuWork) {
    auto result = state_->admitGpu(std::move(request), generation, admission,
                                   std::move(cancellation), std::move(work), std::move(gpuWork));
    return {.status = result.status, .id = result.id, .diagnostic = std::move(result.diagnostic)};
}

GpuExecutorAttachment TaskScheduler::attachGpuExecutor(const GpuServiceGeneration generation,
                                                       GpuTaskWakeSink wakeSink) {
    auto result = state_->attachGpu(generation, std::move(wakeSink));
    GpuExecutorAttachment attachment{
        .status = result.status, .lease = {}, .diagnostic = std::move(result.diagnostic)};
    if (result.status == TaskSubmissionStatus::Accepted) {
        attachment.lease = GpuExecutorLease(state_, generation, result.attachmentId);
    }
    return attachment;
}

} // namespace bloom::runtime
