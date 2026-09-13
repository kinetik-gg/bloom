#pragma once

#include <bloom/core/id.hpp>

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime {

struct TaskIdTag;
struct TaskGroupIdTag;
struct TaskOwnerIdTag;

using TaskId = core::Id<TaskIdTag>;
using TaskGroupId = core::Id<TaskGroupIdTag>;
using TaskOwnerId = core::Id<TaskOwnerIdTag>;

enum class TaskOwnerKind {
    Application,
    Project,
    Composition,
    PanelRequest,
    Export,
};

struct TaskOwner {
    TaskOwnerKind kind = TaskOwnerKind::Application;
    TaskOwnerId id;

    [[nodiscard]] bool isValid() const noexcept { return id.isValid(); }

    friend bool operator==(const TaskOwner&, const TaskOwner&) = default;
};

enum class TaskPriority {
    Interactive,
    Visible,
    Foreground,
    Background,
};

enum class TaskExecutor {
    Cpu,
    BlockingIo,
    Gpu,
};

enum class TaskState {
    Queued,
    Running,
    Succeeded,
    Cancelled,
    Failed,
};

[[nodiscard]] constexpr bool isTerminal(const TaskState state) noexcept {
    return state == TaskState::Succeeded || state == TaskState::Cancelled ||
           state == TaskState::Failed;
}

enum class DiagnosticSeverity {
    Information,
    Warning,
    Error,
};

struct TaskDiagnostic {
    std::string code;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string summary;
    std::string detail;
    std::string suggestedAction;

    friend bool operator==(const TaskDiagnostic&, const TaskDiagnostic&) = default;
};

struct TaskProgress {
    std::string phase;
    std::string subphase;
    std::uint64_t completed = 0;
    std::optional<std::uint64_t> total;

    [[nodiscard]] bool indeterminate() const noexcept { return !total.has_value(); }

    friend bool operator==(const TaskProgress&, const TaskProgress&) = default;
};

struct TaskSourceVersion {
    std::optional<std::uint64_t> documentRevision;
    std::optional<std::uint64_t> requestGeneration;

    friend bool operator==(const TaskSourceVersion&, const TaskSourceVersion&) = default;
};

struct TaskRequest {
    TaskRequest(std::string taskName, TaskOwner taskOwner,
                TaskPriority taskPriority = TaskPriority::Background,
                TaskExecutor taskExecutor = TaskExecutor::Cpu)
        : name(std::move(taskName)), owner(taskOwner), priority(taskPriority),
          executor(taskExecutor) {}

    std::string name;
    TaskOwner owner;
    TaskPriority priority = TaskPriority::Background;
    TaskExecutor executor = TaskExecutor::Cpu;
    std::optional<TaskGroupId> groupId;
    std::optional<std::string> coalescingKey;
    TaskSourceVersion sourceVersion;
};

struct TaskSnapshot {
    TaskId id;
    std::string name;
    TaskOwner owner;
    TaskPriority priority = TaskPriority::Background;
    TaskExecutor executor = TaskExecutor::Cpu;
    TaskState state = TaskState::Queued;
    std::optional<TaskGroupId> groupId;
    TaskSourceVersion sourceVersion;
    TaskProgress progress;
    std::vector<TaskDiagnostic> diagnostics;
    bool cancellationRequested = false;
    std::chrono::steady_clock::time_point queuedAt;
    std::optional<std::chrono::steady_clock::time_point> startedAt;
    std::optional<std::chrono::steady_clock::time_point> finishedAt;
};

struct TaskGroupSnapshot {
    TaskGroupId id;
    std::string name;
    TaskOwner owner;
    std::size_t totalTasks = 0;
    std::size_t queuedTasks = 0;
    std::size_t runningTasks = 0;
    std::size_t finishedTasks = 0;
    TaskProgress progress;
    bool cancellationRequested = false;
};

enum class TaskSubmissionStatus {
    Accepted,
    InvalidRequest,
    QueueFull,
    UnknownGroup,
    CancelledGroup,
    GroupRegistryFull,
    ShuttingDown,
    IdExhausted,
    ExecutorUnavailable,
};

struct TaskSchedulerConfig {
    std::size_t cpuWorkerCount = 1;
    // How many THREADS the scheduler's row-band executor owns (TaskScheduler::rowBandExecutor()).
    // Zero derives a bounded default from std::thread::hardware_concurrency(), which is what every
    // caller that has not thought about it should get; a test that wants strictly serial row
    // evaluation asks for a negative answer explicitly by setting kSerialRowBandWorkers.
    //
    // These are deliberately NOT the cpuWorkerCount threads: a CPU task that blocked on CPU tasks
    // of its own would deadlock as soon as every worker were such a task, and cpuWorkerCount is
    // allowed to be one.
    std::size_t rowBandWorkerCount = 0;
    std::size_t blockingIoWorkerCount = 1;
    std::size_t cpuQueueCapacity = 256;
    std::size_t blockingIoQueueCapacity = 64;
    std::size_t gpuPendingQueueCapacity = 256;
    std::size_t gpuAdmittedStateCapacity = 512;
    std::size_t gpuLiveContinuationCapacity = 64;
    std::size_t gpuQueuedCommandByteCapacity = std::size_t{256} * 1024U * 1024U;
    std::size_t gpuRequestOwnedByteCapacity = std::size_t{1024} * 1024U * 1024U;
    std::size_t terminalHistoryCapacity = 256;
    std::size_t diagnosticsPerTask = 64;
    std::size_t groupRegistryCapacity = 256;

    [[nodiscard]] static TaskSchedulerConfig defaults() noexcept;
    [[nodiscard]] bool isValid() const noexcept;
};

class GpuServiceGeneration final {
  public:
    GpuServiceGeneration() = default;

    [[nodiscard]] static std::optional<GpuServiceGeneration>
    fromRaw(const std::uint64_t value) noexcept {
        return value == 0 ? std::nullopt : std::optional(GpuServiceGeneration(value));
    }

    [[nodiscard]] bool isValid() const noexcept { return value_ != 0; }
    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

    friend bool operator==(const GpuServiceGeneration&, const GpuServiceGeneration&) = default;
    friend auto operator<=>(const GpuServiceGeneration&, const GpuServiceGeneration&) = default;

  private:
    explicit GpuServiceGeneration(const std::uint64_t value) noexcept : value_(value) {}

    std::uint64_t value_ = 0;
};

// The one value of TaskSchedulerConfig::rowBandWorkerCount that asks for no row parallelism at all:
// every band runs on the calling thread, in band order. It exists so a test can pin serial
// evaluation as the reference a parallel evaluation must match bit for bit.
inline constexpr std::size_t kSerialRowBandWorkers = std::numeric_limits<std::size_t>::max();

struct GpuTaskAdmission {
    std::size_t queuedCommandBytes = 0;
    std::size_t requestOwnedBytes = 0;

    [[nodiscard]] bool isValid() const noexcept;
};

enum class GpuDispatchStatus {
    Dispatched,
    Empty,
    ExecutorUnavailable,
    WrongThread,
    ShuttingDown,
};

} // namespace bloom::runtime
