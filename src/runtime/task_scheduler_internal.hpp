#ifndef BLOOM_RUNTIME_TASK_SCHEDULER_INTERNAL_HPP
#define BLOOM_RUNTIME_TASK_SCHEDULER_INTERNAL_HPP

#include <bloom/runtime/task_scheduler.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bloom::runtime::detail {

struct GroupProgressContribution {
    std::uint64_t units = 0;
    bool determinate = false;
};

struct TaskGroupControl {
    mutable std::mutex mutex;
    TaskGroupSnapshot snapshot;
    std::shared_ptr<CancellationState> cancellation = std::make_shared<CancellationState>();
    std::weak_ptr<SchedulerState> scheduler;
    std::uint64_t activeContributionUnits = 0;
    std::size_t indeterminateTasks = 0;
};

struct TaskContextState {
    std::shared_ptr<CancellationState> taskCancellation;
    std::shared_ptr<CancellationState> groupCancellation;
    std::function<void(TaskProgress)> reportProgress;
    std::function<void(TaskDiagnostic)> addDiagnostic;
};

struct TaskRecord {
    mutable std::mutex mutex;
    TaskSnapshot snapshot;
    std::optional<std::string> coalescingKey;
    std::shared_ptr<CancellationState> cancellation;
    std::shared_ptr<CancellationState> groupCancellation;
    std::shared_ptr<TaskGroupControl> group;
    std::shared_ptr<TaskWork> work;
    std::shared_ptr<GpuTaskWork> gpuWork;
    GpuTaskAdmission gpuAdmission;
    GpuServiceGeneration gpuGeneration;
    std::uint64_t gpuAttachmentId = 0;
    bool gpuCommandBytesQueued = false;
    bool gpuWasDispatched = false;
    bool gpuAccountingReleased = false;
    std::string runningPhase = "Working";
    GroupProgressContribution groupContribution;
    std::uint32_t ageCredit = 0;
    bool completionClaimed = false;
};

struct SchedulerState final : std::enable_shared_from_this<SchedulerState> {
    struct Admission {
        TaskSubmissionStatus status = TaskSubmissionStatus::InvalidRequest;
        TaskId id;
        std::optional<TaskDiagnostic> diagnostic;
    };

    struct GroupAdmission {
        TaskSubmissionStatus status = TaskSubmissionStatus::InvalidRequest;
        std::shared_ptr<TaskGroupControl> group;
        std::optional<TaskDiagnostic> diagnostic;
    };

    struct GpuAttachmentAdmission {
        TaskSubmissionStatus status = TaskSubmissionStatus::InvalidRequest;
        std::uint64_t attachmentId = 0;
        std::optional<TaskDiagnostic> diagnostic;
    };

    SchedulerState(TaskSchedulerConfig schedulerConfig, std::function<void(std::size_t)> startHook);
    ~SchedulerState();

    SchedulerState(const SchedulerState&) = delete;
    SchedulerState& operator=(const SchedulerState&) = delete;

    void startWorkers();
    [[nodiscard]] Admission admit(TaskRequest request,
                                  std::shared_ptr<CancellationState> cancellation,
                                  std::shared_ptr<TaskWork> work);
    [[nodiscard]] Admission admitGpu(TaskRequest request, GpuServiceGeneration generation,
                                     GpuTaskAdmission admission,
                                     std::shared_ptr<CancellationState> cancellation,
                                     std::shared_ptr<TaskWork> work,
                                     std::shared_ptr<GpuTaskWork> gpuWork);
    [[nodiscard]] GpuAttachmentAdmission attachGpu(GpuServiceGeneration generation,
                                                   GpuTaskWakeSink wakeSink);
    [[nodiscard]] GpuDispatchStatus dispatchGpu(GpuServiceGeneration generation,
                                                std::uint64_t attachmentId) noexcept;
    [[nodiscard]] bool reportGpuDeviceLost(GpuServiceGeneration generation,
                                           std::uint64_t attachmentId,
                                           TaskDiagnostic diagnostic) noexcept;
    [[nodiscard]] bool forceGpuShutdown(GpuServiceGeneration generation,
                                        std::uint64_t attachmentId) noexcept;
    void detachGpu(GpuServiceGeneration generation, std::uint64_t attachmentId) noexcept;
    void completeGpu(const GpuCompletionCore& core, TaskState outcome) noexcept;
    void appendGpuDiagnostics(const GpuCompletionCore& core,
                              const std::vector<TaskDiagnostic>& diagnostics) noexcept;
    [[nodiscard]] bool gpuAttachmentActive(GpuServiceGeneration generation,
                                           std::uint64_t attachmentId) const noexcept;
    void setGpuDispatchTransitionHookForTesting(std::function<void()> hook);
    [[nodiscard]] GroupAdmission createGroup(TaskOwner owner, std::string name);
    [[nodiscard]] bool cancelTask(TaskId id) noexcept;
    [[nodiscard]] std::size_t cancelOwner(TaskOwner owner);
    void cancelGroup(const std::shared_ptr<TaskGroupControl>& group);
    [[nodiscard]] bool reprioritize(TaskId id, TaskPriority priority) noexcept;
    [[nodiscard]] std::vector<TaskSnapshot> snapshots() const;
    [[nodiscard]] std::optional<TaskSnapshot> snapshot(TaskId id) const;
    void requestShutdown() noexcept;
    [[nodiscard]] bool isAccepting() const noexcept;
    [[nodiscard]] bool isQuiescent() const noexcept;

  private:
    struct ExecutorState {
        std::vector<std::shared_ptr<TaskRecord>> pending;
        std::condition_variable available;
        std::size_t active = 0;
        std::size_t capacity = 0;
        std::vector<std::jthread> workers;
    };

    void startExecutor(ExecutorState& target, TaskExecutor kind, std::size_t count,
                       std::size_t& workerIndex);
    void joinWorkers() noexcept;
    void finalizerLoop() noexcept;
    [[nodiscard]] ExecutorState& executor(TaskExecutor kind) noexcept;
    [[nodiscard]] std::shared_ptr<TaskRecord> takeNextLocked(ExecutorState& target);
    void workerLoop(TaskExecutor kind) noexcept;
    [[nodiscard]] std::shared_ptr<TaskContextState>
    makeContextState(const std::shared_ptr<TaskRecord>& record);
    void finishRunning(const std::shared_ptr<TaskRecord>& record, TaskState outcome) noexcept;
    void finalizeRemoved(const std::shared_ptr<TaskRecord>& record) noexcept;
    void finalizeForcedGpu(const std::shared_ptr<TaskRecord>& record, TaskState previousState,
                           TaskState outcome, const TaskDiagnostic& diagnostic) noexcept;
    static void publishTerminalSnapshot(const std::shared_ptr<TaskRecord>& record,
                                        TaskState outcome) noexcept;
    void retainTerminalLocked(TaskId id) noexcept;
    [[nodiscard]] bool removePendingLocked(const std::shared_ptr<TaskRecord>& record) noexcept;
    [[nodiscard]] static std::size_t
    countPendingMatchesLocked(const TaskRequest& request, const ExecutorState& target) noexcept;
    void removeCoalescedLocked(const TaskRequest& request, ExecutorState& target,
                               std::vector<std::shared_ptr<TaskRecord>>& superseded) noexcept;
    void cancelRunningMatchesLocked(const TaskRequest& request) noexcept;
    [[nodiscard]] std::shared_ptr<const GpuTaskWakeSink> gpuWakeLocked() const noexcept;
    void releaseGpuAccountingLocked(const std::shared_ptr<TaskRecord>& record) noexcept;
    void removeGpuQueuedAccountingLocked(const std::shared_ptr<TaskRecord>& record) noexcept;
    void terminalizeAllGpu(TaskState outcome, const TaskDiagnostic& diagnostic) noexcept;
    [[nodiscard]] std::size_t countQueuedOwnerLocked(TaskOwner owner) const noexcept;
    [[nodiscard]] std::size_t countQueuedGroupLocked(TaskGroupId groupId) const noexcept;
    [[nodiscard]] std::size_t
    removeQueuedOwnerLocked(TaskOwner owner,
                            std::vector<std::shared_ptr<TaskRecord>>& removed) noexcept;
    [[nodiscard]] std::size_t
    removeQueuedGroupLocked(TaskGroupId groupId,
                            std::vector<std::shared_ptr<TaskRecord>>& removed) noexcept;
    [[nodiscard]] std::size_t cancelRunningOwnerLocked(TaskOwner owner) noexcept;
    [[nodiscard]] std::size_t cancelRunningGroupLocked(TaskGroupId groupId) noexcept;
    void purgeExpiredGroupsLocked() noexcept;
    void advanceTaskIdLocked() noexcept;

    TaskSchedulerConfig config;
    mutable std::mutex mutex;
    ExecutorState cpu;
    ExecutorState blockingIo;
    ExecutorState gpu;
    std::vector<std::shared_ptr<TaskRecord>> cancellationQueue;
    std::condition_variable cancellationAvailable;
    std::optional<std::jthread> finalizerWorker;
    std::unordered_map<TaskId, std::shared_ptr<TaskRecord>> records;
    std::vector<std::shared_ptr<TaskRecord>> running;
    std::vector<TaskId> terminalRing;
    std::size_t terminalHead = 0;
    std::size_t terminalCount = 0;
    std::unordered_map<TaskGroupId, std::weak_ptr<TaskGroupControl>> groups;
    std::uint64_t nextTaskId = 1;
    std::uint64_t nextGroupId = 1;
    std::size_t finalizing = 0;
    std::size_t gpuAdmittedStates = 0;
    std::size_t gpuLiveContinuations = 0;
    std::size_t gpuQueuedCommandBytes = 0;
    std::size_t gpuRequestOwnedBytes = 0;
    std::optional<GpuServiceGeneration> gpuGeneration;
    std::uint64_t gpuAttachmentId = 0;
    std::uint64_t nextGpuAttachmentId = 1;
    GpuServiceGeneration lastGpuGeneration;
    std::optional<std::thread::id> gpuServiceThread;
    std::shared_ptr<const GpuTaskWakeSink> gpuWake;
    std::shared_ptr<const std::function<void()>> gpuDispatchTransitionHook;
    bool accepting = true;
    bool stopping = false;
    std::function<void(std::size_t)> workerStartHook;
};

[[nodiscard]] GroupProgressContribution progressContribution(const TaskProgress& progress) noexcept;
void groupAdded(const std::shared_ptr<TaskGroupControl>& group) noexcept;
void groupStarted(const std::shared_ptr<TaskGroupControl>& group) noexcept;
void groupProgress(const std::shared_ptr<TaskGroupControl>& group,
                   GroupProgressContribution previous, GroupProgressContribution next) noexcept;
void groupFinished(const std::shared_ptr<TaskGroupControl>& group, TaskState previousState,
                   GroupProgressContribution contribution) noexcept;

struct TaskSchedulerTestAccess {
    [[nodiscard]] static std::unique_ptr<TaskScheduler>
    create(TaskSchedulerConfig config, std::function<void(std::size_t)> workerStartHook) {
        return std::unique_ptr<TaskScheduler>(
            new TaskScheduler(config, std::move(workerStartHook)));
    }

    static void setGpuDispatchTransitionHook(TaskScheduler& scheduler, std::function<void()> hook) {
        scheduler.state_->setGpuDispatchTransitionHookForTesting(std::move(hook));
    }
};

} // namespace bloom::runtime::detail

#endif
