#pragma once

#include <bloom/core/id.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
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
};

struct TaskSchedulerConfig {
    std::size_t cpuWorkerCount = 1;
    std::size_t blockingIoWorkerCount = 1;
    std::size_t cpuQueueCapacity = 256;
    std::size_t blockingIoQueueCapacity = 64;
    std::size_t terminalHistoryCapacity = 256;
    std::size_t diagnosticsPerTask = 64;
    std::size_t groupRegistryCapacity = 256;

    [[nodiscard]] static TaskSchedulerConfig defaults() noexcept;
    [[nodiscard]] bool isValid() const noexcept;
};

} // namespace bloom::runtime
