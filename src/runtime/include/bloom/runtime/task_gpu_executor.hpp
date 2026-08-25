#pragma once

#include <bloom/runtime/task_scheduler.hpp>

#include <thread>

namespace bloom::runtime {
namespace detail {

struct GpuCompletionCore {
    std::weak_ptr<SchedulerState> scheduler;
    TaskId id;
    GpuServiceGeneration generation;
    std::uint64_t attachmentId = 0;
    std::thread::id serviceThread;
    std::shared_ptr<CancellationState> taskCancellation;
    std::shared_ptr<CancellationState> groupCancellation;
};

void finishGpuToken(const GpuCompletionCore& core, TaskState outcome) noexcept;
void appendGpuDiagnostics(const GpuCompletionCore& core,
                          const std::vector<TaskDiagnostic>& diagnostics) noexcept;

template <TaskResultValue Value> class GpuCompletionSink {
  public:
    GpuCompletionSink() = default;
    GpuCompletionSink(const GpuCompletionSink&) = delete;
    GpuCompletionSink& operator=(const GpuCompletionSink&) = delete;
    virtual ~GpuCompletionSink() = default;

    [[nodiscard]] virtual bool complete(TaskResult<Value> result) noexcept = 0;
    virtual void abandon() noexcept = 0;
    [[nodiscard]] virtual bool cancellationRequested() const noexcept = 0;
};

template <TaskResultValue> class GpuTypedTaskWork;

} // namespace detail

template <TaskResultValue Value> class GpuTaskCompletion final {
  public:
    GpuTaskCompletion() = default;
    GpuTaskCompletion(const GpuTaskCompletion&) = delete;
    GpuTaskCompletion& operator=(const GpuTaskCompletion&) = delete;

    GpuTaskCompletion(GpuTaskCompletion&& other) noexcept : sink_(std::move(other.sink_)) {}

    GpuTaskCompletion& operator=(GpuTaskCompletion&& other) noexcept {
        if (this != &other) {
            abandon();
            sink_ = std::move(other.sink_);
        }
        return *this;
    }

    ~GpuTaskCompletion() { abandon(); }

    [[nodiscard]] bool isValid() const noexcept { return sink_ != nullptr; }
    [[nodiscard]] bool cancellationRequested() const noexcept {
        return sink_ != nullptr && sink_->cancellationRequested();
    }

    [[nodiscard]] bool complete(TaskResult<Value> result) && noexcept {
        auto sink = std::move(sink_);
        return sink != nullptr && sink->complete(std::move(result));
    }

    template <typename Item = Value>
        requires(!std::is_void_v<Item>)
    [[nodiscard]] bool succeed(Item value) && noexcept {
        return std::move(*this).complete(TaskResult<Value>::succeeded(std::move(value)));
    }

    [[nodiscard]] bool succeed() && noexcept
        requires std::is_void_v<Value>
    {
        return std::move(*this).complete(TaskResult<void>::succeeded());
    }

    [[nodiscard]] bool cancel(std::vector<TaskDiagnostic> diagnostics = {}) && noexcept {
        return std::move(*this).complete(TaskResult<Value>::cancelled(std::move(diagnostics)));
    }

    [[nodiscard]] bool fail(TaskDiagnostic diagnostic) && {
        return std::move(*this).complete(TaskResult<Value>::failed(std::move(diagnostic)));
    }

  private:
    template <TaskResultValue> friend class detail::GpuTypedTaskWork;

    explicit GpuTaskCompletion(std::shared_ptr<detail::GpuCompletionSink<Value>> sink) noexcept
        : sink_(std::move(sink)) {}

    void abandon() noexcept {
        if (sink_ != nullptr) {
            auto sink = std::move(sink_);
            sink->abandon();
        }
    }

    std::shared_ptr<detail::GpuCompletionSink<Value>> sink_;
};

namespace detail {

class GpuTaskWork {
  public:
    GpuTaskWork() = default;
    GpuTaskWork(const GpuTaskWork&) = delete;
    GpuTaskWork& operator=(const GpuTaskWork&) = delete;
    virtual ~GpuTaskWork() = default;

    virtual void dispatch(const GpuCompletionCore& core, TaskContext& context) noexcept = 0;
    virtual void stageForcedFailure(TaskDiagnostic diagnostic) noexcept = 0;
};

template <TaskResultValue Value>
class GpuTypedTaskWork final : public TaskWork,
                               public GpuTaskWork,
                               public std::enable_shared_from_this<GpuTypedTaskWork<Value>> {
  private:
    class Sink final : public GpuCompletionSink<Value> {
      public:
        Sink(std::weak_ptr<GpuTypedTaskWork> owner, GpuCompletionCore core)
            : owner_(std::move(owner)), core_(std::move(core)) {}

        [[nodiscard]] bool complete(TaskResult<Value> result) noexcept override {
            {
                std::lock_guard lock(mutex_);
                if (claimed_) {
                    return false;
                }
                claimed_ = true;
            }
            try {
                if (std::this_thread::get_id() != core_.serviceThread) {
                    result = TaskResult<Value>::failed(wrongThreadDiagnostic());
                }
                publish(std::move(result));
            } catch (...) {
                finishGpuToken(core_, TaskState::Failed);
            }
            return true;
        }

        void abandon() noexcept override {
            bool publishNow = false;
            {
                std::lock_guard lock(mutex_);
                if (claimed_) {
                    return;
                }
                if (starterActive_) {
                    abandonedDuringStart_ = true;
                    return;
                }
                claimed_ = true;
                publishNow = true;
            }
            if (publishNow) {
                publishFailure(droppedDiagnostic);
            }
        }

        [[nodiscard]] bool cancellationRequested() const noexcept override {
            const bool task = core_.taskCancellation != nullptr &&
                              core_.taskCancellation->requested.load(std::memory_order_acquire);
            const bool group = core_.groupCancellation != nullptr &&
                               core_.groupCancellation->requested.load(std::memory_order_acquire);
            return task || group;
        }

        void starterReturned() noexcept {
            bool publishDrop = false;
            {
                std::lock_guard lock(mutex_);
                starterActive_ = false;
                if (!claimed_ && abandonedDuringStart_) {
                    claimed_ = true;
                    publishDrop = true;
                }
            }
            if (publishDrop) {
                publishFailure(droppedDiagnostic);
            }
        }

        void starterThrew(std::string detail) noexcept {
            {
                std::lock_guard lock(mutex_);
                starterActive_ = false;
                if (claimed_) {
                    return;
                }
                claimed_ = true;
            }
            try {
                publish(TaskResult<Value>::failed(throwingStarterDiagnostic(std::move(detail))));
            } catch (...) {
                finishGpuToken(core_, TaskState::Failed);
            }
        }

      private:
        using DiagnosticFactory = TaskDiagnostic (*)();

        [[nodiscard]] static TaskDiagnostic droppedDiagnostic() {
            return {.code = "bloom.runtime.gpu-completion-dropped",
                    .severity = DiagnosticSeverity::Error,
                    .summary = "GPU work dropped its completion token",
                    .detail = "The GPU service returned without retaining or consuming the task "
                              "completion.",
                    .suggestedAction = "Review the GPU service state machine."};
        }

        [[nodiscard]] static TaskDiagnostic wrongThreadDiagnostic() {
            return {.code = "bloom.runtime.gpu-completion-wrong-thread",
                    .severity = DiagnosticSeverity::Error,
                    .summary = "GPU work completed on the wrong thread",
                    .detail = "Only the service thread that dispatched the task may consume its "
                              "completion.",
                    .suggestedAction = "Route completion through the owning GPU service loop."};
        }

        [[nodiscard]] static TaskDiagnostic throwingStarterDiagnostic(std::string detail) {
            return {.code = "bloom.runtime.gpu-starter-threw",
                    .severity = DiagnosticSeverity::Error,
                    .summary = "GPU task dispatch failed unexpectedly",
                    .detail = std::move(detail),
                    .suggestedAction = "Review the GPU service dispatch diagnostics."};
        }

        void publishFailure(const DiagnosticFactory factory) noexcept {
            try {
                publish(TaskResult<Value>::failed(factory()));
            } catch (...) {
                finishGpuToken(core_, TaskState::Failed);
            }
        }

        void publish(TaskResult<Value> result) noexcept {
            appendGpuDiagnostics(core_, result.diagnostics());
            const TaskState outcome = result.state();
            if (const auto owner = owner_.lock()) {
                owner->tryStage(std::move(result));
            }
            finishGpuToken(core_, outcome);
        }

        std::weak_ptr<GpuTypedTaskWork> owner_;
        GpuCompletionCore core_;
        mutable std::mutex mutex_;
        bool starterActive_ = true;
        bool abandonedDuringStart_ = false;
        bool claimed_ = false;
    };

  public:
    GpuTypedTaskWork(GpuTaskStarter<Value> starter,
                     std::shared_ptr<ResultMailbox<TaskResult<Value>>> mailbox)
        : starter_(std::move(starter)), mailbox_(std::move(mailbox)),
          fallbackResult_(TaskResult<Value>::failed(unhandledTaskDiagnostic())) {}

    [[nodiscard]] TaskState execute(TaskContext&) noexcept override { return TaskState::Failed; }

    [[nodiscard]] TaskState publish(const TaskState requestedOutcome) noexcept override {
        {
            std::lock_guard dispatchLock(dispatchMutex_);
            published_ = true;
            starter_ = nullptr;
        }
        std::lock_guard resultLock(resultMutex_);
        std::lock_guard mailboxLock(mailbox_->mutex);
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

    [[nodiscard]] bool isValid() const noexcept override {
        std::lock_guard lock(dispatchMutex_);
        return !published_ && static_cast<bool>(starter_);
    }

    void dispatch(const GpuCompletionCore& core, TaskContext& context) noexcept override {
        std::shared_ptr<Sink> sink;
        try {
            sink = std::make_shared<Sink>(this->weak_from_this(), core);
            GpuTaskStarter<Value> starter;
            {
                std::lock_guard lock(dispatchMutex_);
                if (published_ || !starter_) {
                    return;
                }
                starter = std::move(starter_);
            }
            GpuTaskCompletion<Value> completion(sink);
            starter(context, std::move(completion));
            sink->starterReturned();
        } catch (const std::exception& error) {
            if (sink != nullptr) {
                try {
                    sink->starterThrew(error.what() == nullptr ? std::string{} : error.what());
                } catch (...) {
                    sink->starterThrew({});
                }
            } else {
                finishGpuToken(core, TaskState::Failed);
            }
        } catch (...) {
            if (sink != nullptr) {
                sink->starterThrew({});
            } else {
                finishGpuToken(core, TaskState::Failed);
            }
        }
    }

    void stageForcedFailure(TaskDiagnostic diagnostic) noexcept override {
        try {
            std::lock_guard lock(resultMutex_);
            forced_ = true;
            pending_.emplace(TaskResult<Value>::failed(std::move(diagnostic)));
        } catch (...) {
            return;
        }
    }

  private:
    void tryStage(TaskResult<Value> result) noexcept {
        try {
            std::lock_guard lock(resultMutex_);
            if (!forced_) {
                pending_.emplace(std::move(result));
            }
        } catch (...) {
            return;
        }
    }

    GpuTaskStarter<Value> starter_;
    std::shared_ptr<ResultMailbox<TaskResult<Value>>> mailbox_;
    mutable std::mutex dispatchMutex_;
    std::mutex resultMutex_;
    std::optional<TaskResult<Value>> pending_;
    TaskResult<Value> fallbackResult_;
    bool published_ = false;
    bool forced_ = false;
};

} // namespace detail

class GpuExecutorLease final {
  public:
    GpuExecutorLease() = default;
    GpuExecutorLease(const GpuExecutorLease&) = delete;
    GpuExecutorLease& operator=(const GpuExecutorLease&) = delete;

    GpuExecutorLease(GpuExecutorLease&& other) noexcept;
    GpuExecutorLease& operator=(GpuExecutorLease&& other) noexcept;
    ~GpuExecutorLease();

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] GpuServiceGeneration generation() const noexcept;
    [[nodiscard]] GpuDispatchStatus dispatchOne() noexcept;
    [[nodiscard]] bool reportDeviceLost(TaskDiagnostic diagnostic) noexcept;
    [[nodiscard]] bool forceShutdownFallback() noexcept;
    void detach() noexcept;

  private:
    friend class TaskScheduler;
    GpuExecutorLease(std::weak_ptr<detail::SchedulerState> scheduler,
                     GpuServiceGeneration generation, std::uint64_t attachmentId) noexcept;

    std::weak_ptr<detail::SchedulerState> scheduler_;
    GpuServiceGeneration generation_;
    std::uint64_t attachmentId_ = 0;
};

struct GpuExecutorAttachment {
    TaskSubmissionStatus status = TaskSubmissionStatus::InvalidRequest;
    GpuExecutorLease lease;
    std::optional<TaskDiagnostic> diagnostic;

    [[nodiscard]] bool attached() const noexcept {
        return status == TaskSubmissionStatus::Accepted && lease.isValid();
    }
};

template <TaskResultValue Value>
TaskSubmission<Value>
TaskScheduler::submitGpu(TaskRequest request, const GpuServiceGeneration generation,
                         const GpuTaskAdmission admission, GpuTaskStarter<Value> starter) {
    auto mailbox = std::make_shared<detail::ResultMailbox<TaskResult<Value>>>();
    auto cancellation = std::make_shared<detail::CancellationState>();
    auto work = std::make_shared<detail::GpuTypedTaskWork<Value>>(std::move(starter), mailbox);

    request.executor = TaskExecutor::Gpu;
    ErasedSubmission erased =
        submitGpuErased(std::move(request), generation, admission, cancellation, work, work);
    TaskSubmission<Value> submission{
        .status = erased.status, .handle = {}, .diagnostic = std::move(erased.diagnostic)};
    if (erased.status == TaskSubmissionStatus::Accepted) {
        submission.handle =
            TaskHandle<Value>(erased.id, state_, std::move(cancellation), std::move(mailbox));
    }
    return submission;
}

} // namespace bloom::runtime
