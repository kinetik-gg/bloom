#include <bloom/runtime/task_gpu_executor.hpp>

#include "task_scheduler_internal.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using namespace std::chrono_literals;
using bloom::runtime::GpuDispatchStatus;
using bloom::runtime::GpuExecutorAttachment;
using bloom::runtime::GpuServiceGeneration;
using bloom::runtime::GpuTaskAdmission;
using bloom::runtime::GpuTaskCompletion;
using bloom::runtime::TaskContext;
using bloom::runtime::TaskExecutor;
using bloom::runtime::TaskHandle;
using bloom::runtime::TaskOwner;
using bloom::runtime::TaskOwnerId;
using bloom::runtime::TaskOwnerKind;
using bloom::runtime::TaskRequest;
using bloom::runtime::TaskResult;
using bloom::runtime::TaskScheduler;
using bloom::runtime::TaskSchedulerConfig;
using bloom::runtime::TaskState;
using bloom::runtime::TaskSubmissionStatus;

static_assert(!std::is_copy_constructible_v<GpuTaskCompletion<int>>);
static_assert(!std::is_copy_assignable_v<GpuTaskCompletion<int>>);
static_assert(std::is_nothrow_move_constructible_v<GpuTaskCompletion<int>>);
static_assert(std::is_nothrow_move_assignable_v<GpuTaskCompletion<int>>);

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

class Gate final {
  public:
    void enterAndWait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [&] { return released_; });
    }

    [[nodiscard]] bool waitUntilEntered() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 2s, [&] { return entered_; });
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

[[nodiscard]] TaskSchedulerConfig config() {
    return {.cpuWorkerCount = 1,
            .blockingIoWorkerCount = 1,
            .cpuQueueCapacity = 8,
            .blockingIoQueueCapacity = 8,
            .gpuPendingQueueCapacity = 8,
            .gpuAdmittedStateCapacity = 8,
            .gpuLiveContinuationCapacity = 4,
            .gpuQueuedCommandByteCapacity = 1'024,
            .gpuRequestOwnedByteCapacity = 2'048,
            .terminalHistoryCapacity = 32,
            .diagnosticsPerTask = 8,
            .groupRegistryCapacity = 8};
}

[[nodiscard]] TaskOwner owner(const std::uint64_t value) {
    return {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(value)};
}

[[nodiscard]] GpuServiceGeneration generation(const std::uint64_t value) {
    const auto result = GpuServiceGeneration::fromRaw(value);
    if (!result.has_value()) {
        std::terminate();
    }
    return *result;
}

template <typename Value> [[nodiscard]] Value& required(std::optional<Value>& value) {
    if (!value.has_value()) {
        std::terminate();
    }
    return *value;
}

template <typename Value>
[[nodiscard]] TaskResult<Value> takeRequired(const TaskHandle<Value>& handle) {
    auto result = handle.tryTakeResult();
    if (!result.has_value()) {
        std::terminate();
    }
    return std::move(*result);
}

template <typename Value>
[[nodiscard]] std::optional<TaskResult<Value>> awaitResult(const TaskHandle<Value>& handle) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = handle.tryTakeResult()) {
            return result;
        }
        std::this_thread::yield();
    }
    return std::nullopt;
}

[[nodiscard]] bool hasDiagnostic(const std::optional<bloom::runtime::TaskResult<int>>& result,
                                 const std::string& code) {
    if (!result.has_value()) {
        return false;
    }
    for (const auto& diagnostic : result->diagnostics()) {
        if (diagnostic.code == code) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] GpuExecutorAttachment attach(TaskScheduler& scheduler, const std::uint64_t value,
                                           std::atomic_size_t& wakes) {
    return scheduler.attachGpuExecutor(generation(value), [&scheduler, &wakes] {
        ++wakes;
        static_cast<void>(scheduler.snapshots());
        static_cast<void>(scheduler.isQuiescent());
    });
}

void testAttachmentAndSynchronousCompletion(Expectations& expectations) {
    expectations.expect(!GpuServiceGeneration::fromRaw(0).has_value(),
                        "zero is not a service generation");
    TaskScheduler scheduler(config());
    std::atomic_size_t wakes = 0;
    auto attachment = attach(scheduler, 1, wakes);
    expectations.expect(attachment.attached() && attachment.lease.isValid(),
                        "one valid GPU generation attaches");
    auto duplicate = attach(scheduler, 2, wakes);
    expectations.expect(duplicate.status == TaskSubmissionStatus::ExecutorUnavailable,
                        "a second simultaneous service cannot attach");

    auto invalidOrdinary =
        scheduler.submit<int>(TaskRequest("Invalid ordinary GPU task", owner(1),
                                          bloom::runtime::TaskPriority::Visible, TaskExecutor::Gpu),
                              [](TaskContext&) { return TaskResult<int>::succeeded(1); });
    expectations.expect(invalidOrdinary.status == TaskSubmissionStatus::InvalidRequest,
                        "ordinary synchronous submission never accepts the GPU executor");

    TaskHandle<int> handle;
    auto submission = scheduler.submitGpu<int>(
        TaskRequest("Synchronous GPU task", owner(1)), generation(1), {16, 32},
        [&scheduler, &handle](TaskContext&, GpuTaskCompletion<int> completion) {
            const auto running = scheduler.snapshot(handle.id());
            if (!running.has_value() || running->state != TaskState::Running) {
                static_cast<void>(std::move(completion)
                                      .fail({.code = "test.not-running",
                                             .summary = "Task was not running",
                                             .detail = {},
                                             .suggestedAction = {}}));
                return;
            }
            static_cast<void>(std::move(completion).succeed(42));
        });
    handle = submission.handle;
    expectations.expect(submission.accepted() && wakes.load() > 0,
                        "GPU admission succeeds and requests a service wake");
    expectations.expect(attachment.lease.dispatchOne() == GpuDispatchStatus::Dispatched,
                        "the attached service pulls one starter");
    const auto result = handle.tryTakeResult();
    expectations.expect(result.has_value() && result->state() == TaskState::Succeeded &&
                            result->value() == 42,
                        "synchronous service completion publishes through the task mailbox");
    expectations.expect(!handle.tryTakeResult().has_value(), "the result mailbox is take-once");
    expectations.expect(scheduler.isQuiescent(), "synchronous completion releases all accounting");
}

void testRunningPersistenceCancellationAndDrop(Expectations& expectations) {
    TaskScheduler scheduler(config());
    std::atomic_size_t wakes = 0;
    auto attachment = attach(scheduler, 10, wakes);
    std::optional<GpuTaskCompletion<int>> retained;
    auto running = scheduler.submitGpu<int>(
        TaskRequest("Retained GPU task", owner(2)), generation(10), {8, 16},
        [&retained](TaskContext&, GpuTaskCompletion<int> completion) {
            retained.emplace(std::move(completion));
        });
    expectations.expect(attachment.lease.dispatchOne() == GpuDispatchStatus::Dispatched,
                        "retained completion dispatches");
    const auto snapshot = scheduler.snapshot(running.handle.id());
    expectations.expect(snapshot.has_value() && snapshot->state == TaskState::Running &&
                            !running.handle.tryTakeResult().has_value(),
                        "Running persists after the starter returns");
    running.handle.cancel();
    expectations.expect(retained.has_value() && retained->cancellationRequested(),
                        "a live completion observes cooperative cancellation");
    expectations.expect(std::move(required(retained)).succeed(7), "the live token consumes once");
    expectations.expect(!std::move(required(retained)).succeed(8),
                        "a consumed completion rejects duplicate consumption");
    retained.reset();
    const auto cancelled = running.handle.tryTakeResult();
    expectations.expect(cancelled.has_value() && cancelled->state() == TaskState::Cancelled,
                        "success after cancellation cannot publish as success");

    auto dropped =
        scheduler.submitGpu<int>(TaskRequest("Dropped GPU task", owner(2)), generation(10), {1, 1},
                                 [](TaskContext&, GpuTaskCompletion<int>) {});
    expectations.expect(attachment.lease.dispatchOne() == GpuDispatchStatus::Dispatched,
                        "unretained completion dispatches");
    const auto droppedResult = dropped.handle.tryTakeResult();
    expectations.expect(droppedResult.has_value() && droppedResult->state() == TaskState::Failed &&
                            hasDiagnostic(droppedResult, "bloom.runtime.gpu-completion-dropped"),
                        "dropping a completion fails with a stable diagnostic");
}

void testQueuedScopeCancellation(Expectations& expectations) {
    TaskScheduler scheduler(config());
    std::atomic_size_t wakes = 0;
    auto attachment = attach(scheduler, 15, wakes);

    auto byHandle =
        scheduler.submitGpu<int>(TaskRequest("Cancel before dispatch", owner(21)), generation(15),
                                 {1, 1}, [](TaskContext&, GpuTaskCompletion<int> completion) {
                                     static_cast<void>(std::move(completion).succeed(1));
                                 });
    byHandle.handle.cancel();
    const auto handleResult = byHandle.handle.tryTakeResult();
    expectations.expect(handleResult.has_value() && handleResult->state() == TaskState::Cancelled,
                        "handle cancellation removes queued GPU work immediately");

    const TaskOwner owned = owner(22);
    auto byOwner =
        scheduler.submitGpu<int>(TaskRequest("Owner cancellation", owned), generation(15), {1, 1},
                                 [](TaskContext&, GpuTaskCompletion<int> completion) {
                                     static_cast<void>(std::move(completion).succeed(2));
                                 });
    expectations.expect(scheduler.cancelOwner(owned) == 1,
                        "owner cancellation includes queued GPU work");
    expectations.expect(takeRequired(byOwner.handle).state() == TaskState::Cancelled,
                        "owner-cancelled GPU work publishes cancellation");

    const TaskOwner groupedOwner = owner(23);
    auto group = scheduler.createGroup(groupedOwner, "GPU group");
    TaskRequest groupedRequest("Group cancellation", groupedOwner);
    groupedRequest.groupId = group.handle.id();
    auto byGroup = scheduler.submitGpu<int>(std::move(groupedRequest), generation(15), {1, 1},
                                            [](TaskContext&, GpuTaskCompletion<int> completion) {
                                                static_cast<void>(std::move(completion).succeed(3));
                                            });
    group.handle.cancel();
    expectations.expect(takeRequired(byGroup.handle).state() == TaskState::Cancelled &&
                            attachment.lease.dispatchOne() == GpuDispatchStatus::Empty &&
                            scheduler.isQuiescent(),
                        "group cancellation includes GPU admission and accounting");
}

void testWrongThreadMoveOverwriteAndThrow(Expectations& expectations) {
    TaskScheduler scheduler(config());
    std::atomic_size_t wakes = 0;
    auto attachment = attach(scheduler, 20, wakes);

    std::optional<GpuTaskCompletion<int>> wrongThreadToken;
    auto wrongThread = scheduler.submitGpu<int>(
        TaskRequest("Wrong-thread GPU task", owner(3)), generation(20), {1, 1},
        [&wrongThreadToken](TaskContext&, GpuTaskCompletion<int> completion) {
            wrongThreadToken.emplace(std::move(completion));
        });
    static_cast<void>(attachment.lease.dispatchOne());
    std::jthread intruder([token = std::move(required(wrongThreadToken))]() mutable {
        static_cast<void>(std::move(token).succeed(9));
    });
    wrongThreadToken.reset();
    intruder.join();
    const auto wrongResult = wrongThread.handle.tryTakeResult();
    expectations.expect(wrongResult.has_value() && wrongResult->state() == TaskState::Failed &&
                            hasDiagnostic(wrongResult, "bloom.runtime.gpu-completion-wrong-thread"),
                        "completion from a different thread fails closed");

    std::optional<GpuTaskCompletion<int>> firstToken;
    std::optional<GpuTaskCompletion<int>> secondToken;
    auto first =
        scheduler.submitGpu<int>(TaskRequest("Overwritten token", owner(3)), generation(20), {1, 1},
                                 [&firstToken](TaskContext&, GpuTaskCompletion<int> completion) {
                                     firstToken.emplace(std::move(completion));
                                 });
    auto second =
        scheduler.submitGpu<int>(TaskRequest("Replacement token", owner(3)), generation(20), {1, 1},
                                 [&secondToken](TaskContext&, GpuTaskCompletion<int> completion) {
                                     secondToken.emplace(std::move(completion));
                                 });
    static_cast<void>(attachment.lease.dispatchOne());
    static_cast<void>(attachment.lease.dispatchOne());
    required(firstToken) = std::move(required(secondToken));
    secondToken.reset();
    const auto overwritten = first.handle.tryTakeResult();
    expectations.expect(overwritten.has_value() && overwritten->state() == TaskState::Failed &&
                            hasDiagnostic(overwritten, "bloom.runtime.gpu-completion-dropped"),
                        "move-overwrite fails the token that would otherwise be lost");
    expectations.expect(std::move(required(firstToken)).succeed(11),
                        "the replacement token remains consumable");
    firstToken.reset();
    const auto replacement = second.handle.tryTakeResult();
    expectations.expect(replacement.has_value() && replacement->value() == 11,
                        "move-overwrite preserves the incoming completion");

    auto throwing = scheduler.submitGpu<int>(
        TaskRequest("Throwing GPU starter", owner(3)), generation(20), {1, 1},
        [](TaskContext&, GpuTaskCompletion<int>) { throw std::runtime_error("dispatch fault"); });
    static_cast<void>(attachment.lease.dispatchOne());
    const auto throwingResult = throwing.handle.tryTakeResult();
    expectations.expect(throwingResult.has_value() &&
                            throwingResult->state() == TaskState::Failed &&
                            hasDiagnostic(throwingResult, "bloom.runtime.gpu-starter-threw"),
                        "a throwing starter terminalizes with a stable diagnostic");
}

void testBoundsCoalescingAndAccounting(Expectations& expectations) {
    auto bounded = config();
    bounded.gpuPendingQueueCapacity = 2;
    bounded.gpuAdmittedStateCapacity = 2;
    bounded.gpuLiveContinuationCapacity = 1;
    bounded.gpuQueuedCommandByteCapacity = 10;
    bounded.gpuRequestOwnedByteCapacity = 20;
    TaskScheduler scheduler(bounded);
    std::atomic_size_t wakes = 0;
    auto attachment = attach(scheduler, 30, wakes);
    std::optional<GpuTaskCompletion<int>> firstToken;
    std::optional<GpuTaskCompletion<int>> secondToken;
    auto first = scheduler.submitGpu<int>(
        TaskRequest("First bounded task", owner(4)), generation(30), {6, 12},
        [&firstToken](TaskContext&, GpuTaskCompletion<int> completion) {
            firstToken.emplace(std::move(completion));
        });
    auto second = scheduler.submitGpu<int>(
        TaskRequest("Second bounded task", owner(4)), generation(30), {4, 8},
        [&secondToken](TaskContext&, GpuTaskCompletion<int> completion) {
            secondToken.emplace(std::move(completion));
        });
    auto full =
        scheduler.submitGpu<int>(TaskRequest("Over capacity", owner(4)), generation(30), {0, 0},
                                 [](TaskContext&, GpuTaskCompletion<int> completion) {
                                     static_cast<void>(std::move(completion).succeed(0));
                                 });
    expectations.expect(first.accepted() && second.accepted() &&
                            full.status == TaskSubmissionStatus::QueueFull,
                        "independent pending, admitted, and byte caps reject exact overflow");
    static_cast<void>(attachment.lease.dispatchOne());
    expectations.expect(attachment.lease.dispatchOne() == GpuDispatchStatus::Empty,
                        "the live-continuation cap blocks a second dispatch");
    static_cast<void>(std::move(required(firstToken)).succeed(1));
    firstToken.reset();
    expectations.expect(attachment.lease.dispatchOne() == GpuDispatchStatus::Dispatched,
                        "completion releases live capacity for queued work");
    static_cast<void>(std::move(required(secondToken)).succeed(2));
    secondToken.reset();
    expectations.expect(takeRequired(first.handle).value() == 1 &&
                            takeRequired(second.handle).value() == 2 && scheduler.isQuiescent(),
                        "terminal work releases all admitted byte and count accounting");

    TaskRequest oldRequest("Coalesced old task", owner(5));
    oldRequest.coalescingKey = "viewer";
    auto old = scheduler.submitGpu<int>(std::move(oldRequest), generation(30), {10, 20},
                                        [](TaskContext&, GpuTaskCompletion<int> completion) {
                                            static_cast<void>(std::move(completion).succeed(3));
                                        });
    TaskRequest rejectedRequest("Rejected replacement", owner(5));
    rejectedRequest.coalescingKey = "viewer";
    auto rejected =
        scheduler.submitGpu<int>(std::move(rejectedRequest), generation(30), {11, 20},
                                 [](TaskContext&, GpuTaskCompletion<int> completion) {
                                     static_cast<void>(std::move(completion).succeed(4));
                                 });
    expectations.expect(rejected.status == TaskSubmissionStatus::QueueFull &&
                            !old.handle.cancellationRequested(),
                        "failed replacement admission does not cancel old admitted work");
    TaskRequest acceptedRequest("Accepted replacement", owner(5));
    acceptedRequest.coalescingKey = "viewer";
    auto accepted =
        scheduler.submitGpu<int>(std::move(acceptedRequest), generation(30), {10, 20},
                                 [](TaskContext&, GpuTaskCompletion<int> completion) {
                                     static_cast<void>(std::move(completion).succeed(5));
                                 });
    const auto oldResult = old.handle.tryTakeResult();
    expectations.expect(accepted.accepted() && oldResult.has_value() &&
                            oldResult->state() == TaskState::Cancelled,
                        "successful coalescing replaces queued GPU work atomically");
    static_cast<void>(attachment.lease.dispatchOne());
    expectations.expect(takeRequired(accepted.handle).value() == 5,
                        "the accepted coalesced generation is the dispatched generation");

    auto overflow = scheduler.submitGpu<int>(
        TaskRequest("Invalid overflow admission", owner(5)), generation(30),
        {std::numeric_limits<std::size_t>::max(), 0}, [](TaskContext&, GpuTaskCompletion<int>) {});
    expectations.expect(overflow.status == TaskSubmissionStatus::InvalidRequest,
                        "per-request hard limits reject arithmetic-overflow inputs");
}

void testDeviceLossRecoveryAndShutdown(Expectations& expectations) {
    TaskScheduler scheduler(config());
    std::atomic_size_t wakes = 0;
    auto attachment = attach(scheduler, 40, wakes);
    std::optional<GpuTaskCompletion<int>> liveToken;
    auto live = scheduler.submitGpu<int>(
        TaskRequest("Live at device loss", owner(6)), generation(40), {1, 1},
        [&liveToken](TaskContext&, GpuTaskCompletion<int> completion) {
            liveToken.emplace(std::move(completion));
        });
    auto queued =
        scheduler.submitGpu<int>(TaskRequest("Queued at device loss", owner(6)), generation(40),
                                 {1, 1}, [](TaskContext&, GpuTaskCompletion<int> completion) {
                                     static_cast<void>(std::move(completion).succeed(2));
                                 });
    static_cast<void>(attachment.lease.dispatchOne());
    expectations.expect(attachment.lease.reportDeviceLost({.code = "test.device-lost",
                                                           .summary = "Test device loss",
                                                           .detail = {},
                                                           .suggestedAction = {}}),
                        "the owning service reports device loss");
    expectations.expect(!attachment.attached(),
                        "an accepted attachment no longer reports live after device loss");
    const auto liveLost = live.handle.tryTakeResult();
    const auto queuedLost = queued.handle.tryTakeResult();
    expectations.expect(liveLost.has_value() && liveLost->state() == TaskState::Failed &&
                            queuedLost.has_value() && queuedLost->state() == TaskState::Failed,
                        "device loss terminalizes both live and queued work");
    expectations.expect(std::move(required(liveToken)).succeed(99),
                        "a stale token may be consumed without reaching destroyed state");
    liveToken.reset();
    expectations.expect(!live.handle.tryTakeResult().has_value(),
                        "stale completion cannot overwrite the published loss result");

    auto stale = attach(scheduler, 40, wakes);
    expectations.expect(stale.status == TaskSubmissionStatus::ExecutorUnavailable,
                        "a lost generation cannot reactivate");
    auto recovered = attach(scheduler, 41, wakes);
    expectations.expect(recovered.attached(), "a newer service generation can recover");
    auto recoveredTask =
        scheduler.submitGpu<int>(TaskRequest("Recovered GPU task", owner(6)), generation(41),
                                 {1, 1}, [](TaskContext&, GpuTaskCompletion<int> completion) {
                                     static_cast<void>(std::move(completion).succeed(6));
                                 });
    static_cast<void>(recovered.lease.dispatchOne());
    expectations.expect(takeRequired(recoveredTask.handle).value() == 6,
                        "the recovered generation executes new work");

    std::optional<GpuTaskCompletion<int>> shutdownToken;
    auto liveShutdown =
        scheduler.submitGpu<int>(TaskRequest("Live at shutdown", owner(6)), generation(41), {1, 1},
                                 [&shutdownToken](TaskContext&, GpuTaskCompletion<int> completion) {
                                     shutdownToken.emplace(std::move(completion));
                                 });
    static_cast<void>(recovered.lease.dispatchOne());
    const auto started = std::chrono::steady_clock::now();
    scheduler.beginShutdown();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expectations.expect(elapsed < 250ms && required(shutdownToken).cancellationRequested(),
                        "beginShutdown is non-blocking and signals live GPU work");
    expectations.expect(recovered.lease.forceShutdownFallback(),
                        "service lag is bounded by an explicit shutdown fallback");
    const auto shutdownResult = liveShutdown.handle.tryTakeResult();
    expectations.expect(shutdownResult.has_value() &&
                            shutdownResult->state() == TaskState::Cancelled &&
                            scheduler.isQuiescent(),
                        "the fallback leaves no GPU task Running");
    shutdownToken.reset();
}

void testDispatchTransitionAndActiveStarterRaces(Expectations& expectations) {
    {
        TaskScheduler scheduler(config());
        std::atomic_size_t wakes = 0;
        auto attachment = attach(scheduler, 45, wakes);
        Gate transition;
        bloom::runtime::detail::TaskSchedulerTestAccess::setGpuDispatchTransitionHook(
            scheduler, [&transition] { transition.enterAndWait(); });
        const TaskOwner groupedOwner = owner(61);
        auto group = scheduler.createGroup(groupedOwner, "Transition group");
        std::atomic_size_t starterCalls = 0;
        TaskRequest request("Transition race", groupedOwner);
        request.groupId = group.handle.id();
        auto task = scheduler.submitGpu<int>(
            std::move(request), generation(45), {1, 1},
            [&starterCalls](TaskContext&, GpuTaskCompletion<int> completion) {
                ++starterCalls;
                static_cast<void>(std::move(completion).succeed(1));
            });
        GpuDispatchStatus dispatchStatus = GpuDispatchStatus::Empty;
        std::jthread service([&] { dispatchStatus = attachment.lease.dispatchOne(); });
        expectations.expect(transition.waitUntilEntered(),
                            "dispatch reaches the deterministic transition seam");
        attachment.lease.detach();
        transition.release();
        service.join();
        const auto result = task.handle.tryTakeResult();
        const auto groupState = group.handle.snapshot();
        expectations.expect(dispatchStatus == GpuDispatchStatus::Dispatched &&
                                starterCalls.load() == 0 && result.has_value() &&
                                result->state() == TaskState::Failed,
                            "detach at the dispatch transition prevents stale starter invocation");
        expectations.expect(groupState.has_value() && groupState->queuedTasks == 0 &&
                                groupState->runningTasks == 0 && groupState->finishedTasks == 1,
                            "transition detachment performs exactly one group start/finish pair");
    }

    {
        TaskScheduler scheduler(config());
        std::atomic_size_t wakes = 0;
        auto attachment = attach(scheduler, 46, wakes);
        Gate activeStarter;
        const TaskOwner groupedOwner = owner(62);
        auto group = scheduler.createGroup(groupedOwner, "Active starter group");
        TaskRequest request("Active starter race", groupedOwner);
        request.groupId = group.handle.id();
        auto task = scheduler.submitGpu<int>(
            std::move(request), generation(46), {1, 1},
            [&activeStarter](TaskContext&, GpuTaskCompletion<int> completion) {
                activeStarter.enterAndWait();
                static_cast<void>(std::move(completion).succeed(2));
            });
        std::jthread service([&] { static_cast<void>(attachment.lease.dispatchOne()); });
        expectations.expect(activeStarter.waitUntilEntered(), "the starter is actively executing");
        attachment.lease.detach();
        const auto forced = task.handle.tryTakeResult();
        activeStarter.release();
        service.join();
        const auto groupState = group.handle.snapshot();
        expectations.expect(forced.has_value() && forced->state() == TaskState::Failed &&
                                !task.handle.tryTakeResult().has_value(),
                            "fallback racing an active starter publishes the mailbox once");
        expectations.expect(groupState.has_value() && groupState->runningTasks == 0 &&
                                groupState->finishedTasks == 1,
                            "active-starter fallback performs one group terminal transition");
    }
}

void testLeaseAndSchedulerDestructionSafety(Expectations& expectations) {
    std::optional<GpuTaskCompletion<int>> staleToken;
    TaskHandle<int> detachedHandle;
    {
        TaskScheduler scheduler(config());
        std::atomic_size_t wakes = 0;
        {
            auto attachment = attach(scheduler, 50, wakes);
            auto submission = scheduler.submitGpu<int>(
                TaskRequest("Lease destruction", owner(7)), generation(50), {1, 1},
                [&staleToken](TaskContext&, GpuTaskCompletion<int> completion) {
                    staleToken.emplace(std::move(completion));
                });
            detachedHandle = submission.handle;
            static_cast<void>(attachment.lease.dispatchOne());
        }
        const auto detached = detachedHandle.tryTakeResult();
        expectations.expect(detached.has_value() && detached->state() == TaskState::Failed,
                            "lease destruction terminalizes its live generation");
    }
    staleToken.reset();

    std::optional<GpuTaskCompletion<int>> destructionToken;
    std::optional<bloom::runtime::GpuExecutorLease> externalLease;
    TaskHandle<int> destructionHandle;
    {
        auto scheduler = std::make_unique<TaskScheduler>(config());
        std::atomic_size_t wakes = 0;
        auto attachment = attach(*scheduler, 51, wakes);
        externalLease.emplace(std::move(attachment.lease));
        auto submission = scheduler->submitGpu<int>(
            TaskRequest("Scheduler destruction", owner(7)), generation(51), {1, 1},
            [&destructionToken](TaskContext&, GpuTaskCompletion<int> completion) {
                destructionToken.emplace(std::move(completion));
            });
        destructionHandle = submission.handle;
        static_cast<void>(externalLease->dispatchOne());
        scheduler.reset();
    }
    const auto destroyed = destructionHandle.tryTakeResult();
    expectations.expect(destroyed.has_value() && destroyed->state() == TaskState::Cancelled,
                        "scheduler destruction force-terminalizes a lagging service");
    destructionToken.reset();
    externalLease.reset();
    expectations.expect(!destructionHandle.cancellationRequested(),
                        "stale lease and token teardown cannot retain or access scheduler state");
}

} // namespace

int main() {
    Expectations expectations;
    testAttachmentAndSynchronousCompletion(expectations);
    testRunningPersistenceCancellationAndDrop(expectations);
    testQueuedScopeCancellation(expectations);
    testWrongThreadMoveOverwriteAndThrow(expectations);
    testBoundsCoalescingAndAccounting(expectations);
    testDeviceLossRecoveryAndShutdown(expectations);
    testDispatchTransitionAndActiveStarterRaces(expectations);
    testLeaseAndSchedulerDestructionSafety(expectations);

    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " GPU task executor expectation(s) failed\n";
        return 1;
    }
    return 0;
}
