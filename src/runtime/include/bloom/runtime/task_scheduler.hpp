#pragma once

#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/task_types.hpp>

#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace bloom::runtime {

namespace detail {
struct SchedulerState;
struct TaskContextState;
struct TaskGroupControl;
struct TaskSchedulerTestAccess;

template <typename Result> struct ResultMailbox {
    std::mutex mutex;
    std::optional<Result> result;
};

void requestTaskCancellation(const std::weak_ptr<SchedulerState>& scheduler, TaskId id,
                             const std::shared_ptr<CancellationState>& cancellation) noexcept;
void requestGroupCancellation(const std::shared_ptr<TaskGroupControl>& group);
} // namespace detail

class TaskContext final {
  public:
    [[nodiscard]] const CancellationToken& cancellation() const noexcept;
    [[nodiscard]] bool isCancellationRequested() const noexcept;

    void reportProgress(TaskProgress progress);
    void addDiagnostic(TaskDiagnostic diagnostic);

  private:
    friend class TaskScheduler;
    friend struct detail::SchedulerState;
    explicit TaskContext(std::shared_ptr<detail::TaskContextState> state) noexcept;

    std::shared_ptr<detail::TaskContextState> state_;
    CancellationToken cancellation_;
};

template <typename Value>
concept TaskResultValue = std::is_void_v<Value> || (std::is_nothrow_move_constructible_v<Value> &&
                                                    std::is_nothrow_destructible_v<Value> &&
                                                    sizeof(Value) <= sizeof(void*) * 4U);

template <TaskResultValue Value> class TaskResult final {
  public:
    [[nodiscard]] static TaskResult succeeded(Value value,
                                              std::vector<TaskDiagnostic> diagnostics = {}) {
        return TaskResult(TaskState::Succeeded, std::move(value), std::move(diagnostics));
    }

    [[nodiscard]] static TaskResult cancelled(std::vector<TaskDiagnostic> diagnostics = {}) {
        return TaskResult(TaskState::Cancelled, std::nullopt, std::move(diagnostics));
    }

    [[nodiscard]] static TaskResult failed(TaskDiagnostic diagnostic) {
        std::vector<TaskDiagnostic> diagnostics;
        diagnostics.push_back(std::move(diagnostic));
        return TaskResult(TaskState::Failed, std::nullopt, std::move(diagnostics));
    }

    [[nodiscard]] static TaskResult failed(std::vector<TaskDiagnostic> diagnostics) {
        return TaskResult(TaskState::Failed, std::nullopt, std::move(diagnostics));
    }

    [[nodiscard]] TaskState state() const noexcept { return state_; }
    [[nodiscard]] const std::optional<Value>& value() const& noexcept { return value_; }
    [[nodiscard]] std::optional<Value>&& value() && noexcept { return std::move(value_); }
    [[nodiscard]] const std::vector<TaskDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

    [[nodiscard]] TaskResult intoCancelled() && noexcept {
        state_ = TaskState::Cancelled;
        value_.reset();
        return std::move(*this);
    }

  private:
    TaskResult(TaskState state, std::optional<Value> value, std::vector<TaskDiagnostic> diagnostics)
        : state_(state), value_(std::move(value)), diagnostics_(std::move(diagnostics)) {}

    TaskState state_;
    std::optional<Value> value_;
    std::vector<TaskDiagnostic> diagnostics_;
};

template <> class TaskResult<void> final {
  public:
    [[nodiscard]] static TaskResult succeeded(std::vector<TaskDiagnostic> diagnostics = {}) {
        return TaskResult(TaskState::Succeeded, std::move(diagnostics));
    }

    [[nodiscard]] static TaskResult cancelled(std::vector<TaskDiagnostic> diagnostics = {}) {
        return TaskResult(TaskState::Cancelled, std::move(diagnostics));
    }

    [[nodiscard]] static TaskResult failed(TaskDiagnostic diagnostic) {
        std::vector<TaskDiagnostic> diagnostics;
        diagnostics.push_back(std::move(diagnostic));
        return TaskResult(TaskState::Failed, std::move(diagnostics));
    }

    [[nodiscard]] static TaskResult failed(std::vector<TaskDiagnostic> diagnostics) {
        return TaskResult(TaskState::Failed, std::move(diagnostics));
    }

    [[nodiscard]] TaskState state() const noexcept { return state_; }
    [[nodiscard]] const std::vector<TaskDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

    [[nodiscard]] TaskResult intoCancelled() && noexcept {
        state_ = TaskState::Cancelled;
        return std::move(*this);
    }

  private:
    TaskResult(TaskState state, std::vector<TaskDiagnostic> diagnostics)
        : state_(state), diagnostics_(std::move(diagnostics)) {}

    TaskState state_;
    std::vector<TaskDiagnostic> diagnostics_;
};

template <TaskResultValue Value>
using TaskFunction = std::function<TaskResult<Value>(TaskContext&)>;

namespace detail {

class TaskWork {
  public:
    TaskWork() = default;
    TaskWork(const TaskWork&) = delete;
    TaskWork& operator=(const TaskWork&) = delete;
    virtual ~TaskWork() = default;

    [[nodiscard]] virtual TaskState execute(TaskContext& context) noexcept = 0;
    [[nodiscard]] virtual TaskState publish(TaskState requestedOutcome) noexcept = 0;
    [[nodiscard]] virtual bool isValid() const noexcept = 0;
};

inline TaskDiagnostic unhandledTaskDiagnostic(std::string detail = {}) {
    return {.code = "bloom.runtime.unhandled-exception",
            .severity = DiagnosticSeverity::Error,
            .summary = "A background task failed unexpectedly",
            .detail = std::move(detail),
            .suggestedAction = "Review the task diagnostics and retry the operation."};
}

template <TaskResultValue Value> class TypedTaskWork final : public TaskWork {
  public:
    TypedTaskWork(TaskFunction<Value> function,
                  std::shared_ptr<ResultMailbox<TaskResult<Value>>> mailbox)
        : function_(std::move(function)), mailbox_(std::move(mailbox)),
          fallbackResult_(TaskResult<Value>::failed(unhandledTaskDiagnostic())),
          fallbackSnapshotDiagnostic_(unhandledTaskDiagnostic()) {}

    [[nodiscard]] TaskState execute(TaskContext& context) noexcept override {
        try {
            TaskResult<Value> result = function_(context);
            copyDiagnosticsBestEffort(result, context);
            const TaskState state = result.state();
            pending_.emplace(std::move(result));
            return state;
        } catch (const std::exception& error) {
            return captureFailure(context, error.what());
        } catch (...) {
            return captureFallback(context);
        }
    }

    [[nodiscard]] TaskState publish(const TaskState requestedOutcome) noexcept override {
        std::lock_guard lock(mailbox_->mutex);
        if (mailbox_->result.has_value()) {
            return mailbox_->result->state();
        }
        if (requestedOutcome == TaskState::Cancelled) {
            if (pending_.has_value()) {
                mailbox_->result.emplace(std::move(*pending_).intoCancelled());
            } else {
                mailbox_->result.emplace(TaskResult<Value>::cancelled());
            }
        } else if (pending_.has_value() && pending_->state() == requestedOutcome) {
            mailbox_->result.emplace(std::move(*pending_));
        } else {
            mailbox_->result.emplace(std::move(fallbackResult_));
        }
        pending_.reset();
        return mailbox_->result->state();
    }

    [[nodiscard]] bool isValid() const noexcept override { return static_cast<bool>(function_); }

  private:
    static void copyDiagnosticsBestEffort(const TaskResult<Value>& result,
                                          TaskContext& context) noexcept {
        for (const auto& diagnostic : result.diagnostics()) {
            try {
                context.addDiagnostic(diagnostic);
            } catch (...) {
                return;
            }
        }
    }

    [[nodiscard]] TaskState captureFailure(TaskContext& context, const char* detail) noexcept {
        try {
            TaskDiagnostic diagnostic = unhandledTaskDiagnostic(detail == nullptr ? "" : detail);
            try {
                context.addDiagnostic(diagnostic);
            } catch (...) {
            }
            pending_.emplace(TaskResult<Value>::failed(std::move(diagnostic)));
            return TaskState::Failed;
        } catch (...) {
            return captureFallback(context);
        }
    }

    [[nodiscard]] TaskState captureFallback(TaskContext& context) noexcept {
        try {
            context.addDiagnostic(std::move(fallbackSnapshotDiagnostic_));
        } catch (...) {
        }
        pending_.emplace(std::move(fallbackResult_));
        return TaskState::Failed;
    }

    TaskFunction<Value> function_;
    std::shared_ptr<ResultMailbox<TaskResult<Value>>> mailbox_;
    std::optional<TaskResult<Value>> pending_;
    TaskResult<Value> fallbackResult_;
    TaskDiagnostic fallbackSnapshotDiagnostic_;
};

} // namespace detail

template <TaskResultValue Value> class TaskHandle final {
  public:
    TaskHandle() = default;

    [[nodiscard]] TaskId id() const noexcept { return id_; }
    [[nodiscard]] bool isValid() const noexcept { return id_.isValid(); }

    void cancel() const noexcept {
        detail::requestTaskCancellation(scheduler_, id_, cancellation_);
    }

    [[nodiscard]] bool cancellationRequested() const noexcept {
        return cancellation_ != nullptr &&
               !cancellation_->completed.load(std::memory_order_acquire) &&
               cancellation_->requested.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::optional<TaskResult<Value>> tryTakeResult() const {
        if (mailbox_ == nullptr) {
            return std::nullopt;
        }
        std::lock_guard lock(mailbox_->mutex);
        if (!mailbox_->result.has_value()) {
            return std::nullopt;
        }
        auto result = std::move(mailbox_->result);
        mailbox_->result.reset();
        return result;
    }

  private:
    friend class TaskScheduler;
    TaskHandle(TaskId id, std::weak_ptr<detail::SchedulerState> scheduler,
               std::shared_ptr<detail::CancellationState> cancellation,
               std::shared_ptr<detail::ResultMailbox<TaskResult<Value>>> mailbox) noexcept
        : id_(id), scheduler_(std::move(scheduler)), cancellation_(std::move(cancellation)),
          mailbox_(std::move(mailbox)) {}

    TaskId id_;
    std::weak_ptr<detail::SchedulerState> scheduler_;
    std::shared_ptr<detail::CancellationState> cancellation_;
    std::shared_ptr<detail::ResultMailbox<TaskResult<Value>>> mailbox_;
};

template <TaskResultValue Value> struct TaskSubmission {
    TaskSubmissionStatus status = TaskSubmissionStatus::InvalidRequest;
    TaskHandle<Value> handle;
    std::optional<TaskDiagnostic> diagnostic;

    [[nodiscard]] bool accepted() const noexcept {
        return status == TaskSubmissionStatus::Accepted;
    }
};

class TaskGroupHandle final {
  public:
    TaskGroupHandle() = default;

    [[nodiscard]] TaskGroupId id() const noexcept;
    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] TaskOwner owner() const noexcept;
    [[nodiscard]] bool cancellationRequested() const noexcept;
    [[nodiscard]] std::optional<TaskGroupSnapshot> snapshot() const;
    void cancel() const;

  private:
    friend class TaskScheduler;
    explicit TaskGroupHandle(std::shared_ptr<detail::TaskGroupControl> control) noexcept;

    std::shared_ptr<detail::TaskGroupControl> control_;
};

struct TaskGroupSubmission {
    TaskSubmissionStatus status = TaskSubmissionStatus::InvalidRequest;
    TaskGroupHandle handle;
    std::optional<TaskDiagnostic> diagnostic;

    [[nodiscard]] bool accepted() const noexcept {
        return status == TaskSubmissionStatus::Accepted;
    }
};

class TaskScheduler final {
  public:
    explicit TaskScheduler(TaskSchedulerConfig config = TaskSchedulerConfig::defaults());
    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;
    TaskScheduler(TaskScheduler&&) = delete;
    TaskScheduler& operator=(TaskScheduler&&) = delete;

    // Destruction is non-blocking only after isQuiescent() reports true. Release builds still
    // request cancellation and join as a lifetime-safe fallback if the owner violates that
    // contract.
    ~TaskScheduler();

    template <TaskResultValue Value>
    [[nodiscard]] TaskSubmission<Value> submit(TaskRequest request, TaskFunction<Value> function) {
        auto mailbox = std::make_shared<detail::ResultMailbox<TaskResult<Value>>>();
        auto cancellation = std::make_shared<detail::CancellationState>();
        auto work = std::make_shared<detail::TypedTaskWork<Value>>(std::move(function), mailbox);

        ErasedSubmission erased = submitErased(std::move(request), cancellation, std::move(work));
        TaskSubmission<Value> submission{
            .status = erased.status, .handle = {}, .diagnostic = std::move(erased.diagnostic)};
        if (erased.status == TaskSubmissionStatus::Accepted) {
            submission.handle =
                TaskHandle<Value>(erased.id, state_, std::move(cancellation), std::move(mailbox));
        }
        return submission;
    }

    [[nodiscard]] TaskGroupSubmission createGroup(TaskOwner owner, std::string name);
    [[nodiscard]] bool cancel(TaskId id) noexcept;
    [[nodiscard]] std::size_t cancelOwner(TaskOwner owner);
    [[nodiscard]] bool reprioritize(TaskId id, TaskPriority priority) noexcept;

    [[nodiscard]] std::vector<TaskSnapshot> snapshots() const;
    [[nodiscard]] std::optional<TaskSnapshot> snapshot(TaskId id) const;

    // This call only closes admission, requests cancellation, and wakes workers. It never drains.
    void beginShutdown() noexcept;
    [[nodiscard]] bool isAccepting() const noexcept;
    [[nodiscard]] bool isQuiescent() const noexcept;

  private:
    using WorkerStartHook = std::function<void(std::size_t)>;
    friend struct detail::TaskSchedulerTestAccess;

    struct ErasedSubmission {
        TaskSubmissionStatus status = TaskSubmissionStatus::InvalidRequest;
        TaskId id;
        std::optional<TaskDiagnostic> diagnostic;
    };

    TaskScheduler(TaskSchedulerConfig config, WorkerStartHook workerStartHook);
    [[nodiscard]] ErasedSubmission
    submitErased(TaskRequest request, std::shared_ptr<detail::CancellationState> cancellation,
                 std::shared_ptr<detail::TaskWork> work);

    std::shared_ptr<detail::SchedulerState> state_;
};

} // namespace bloom::runtime
