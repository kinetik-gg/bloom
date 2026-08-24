#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using bloom::runtime::TaskContext;
using bloom::runtime::TaskGroupHandle;
using bloom::runtime::TaskHandle;
using bloom::runtime::TaskOwner;
using bloom::runtime::TaskOwnerId;
using bloom::runtime::TaskOwnerKind;
using bloom::runtime::TaskRequest;
using bloom::runtime::TaskResult;
using bloom::runtime::TaskScheduler;
using bloom::runtime::TaskSchedulerConfig;
using bloom::runtime::TaskState;

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
    void enterAndWait(TaskContext& context) {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [&] { return released_ || context.isCancellationRequested(); });
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

TaskSchedulerConfig testConfig(const std::size_t cpuCapacity = 64,
                               const std::size_t ioCapacity = 16,
                               const std::size_t historyCapacity = 64) {
    return {.cpuWorkerCount = 1,
            .blockingIoWorkerCount = 1,
            .cpuQueueCapacity = cpuCapacity,
            .blockingIoQueueCapacity = ioCapacity,
            .terminalHistoryCapacity = historyCapacity,
            .diagnosticsPerTask = 8};
}

TaskOwner owner(const std::uint64_t value, const TaskOwnerKind kind = TaskOwnerKind::Composition) {
    return {.kind = kind, .id = TaskOwnerId::fromRaw(value)};
}

template <typename Value>
std::optional<TaskResult<Value>> awaitResult(const TaskHandle<Value>& handle) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = handle.tryTakeResult()) {
            return result;
        }
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

bool awaitQuiescence(const TaskScheduler& scheduler) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (scheduler.isQuiescent()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

void testTypedResultAndProgress(Expectations& expectations) {
    using namespace bloom::runtime;
    TaskScheduler scheduler(testConfig());
    TaskRequest request("Compile composition", owner(1), TaskPriority::Visible);
    request.sourceVersion = {.documentRevision = 7, .requestGeneration = 4};
    auto submission = scheduler.submit<int>(std::move(request), [](TaskContext& context) {
        context.reportProgress(
            {.phase = "Compile", .subphase = "Graph", .completed = 5, .total = 10});
        context.reportProgress(
            {.phase = "Compile", .subphase = "Graph", .completed = 3, .total = 8});
        return TaskResult<int>::succeeded(42, {{.code = "bloom.runtime.test-warning",
                                                .severity = DiagnosticSeverity::Warning,
                                                .summary = "A deterministic warning",
                                                .detail = {},
                                                .suggestedAction = {}}});
    });

    expectations.expect(submission.accepted(), "typed task submission is accepted");
    const auto result = awaitResult(submission.handle);
    expectations.expect(result.has_value(), "typed result becomes available without a wait API");
    if (result.has_value()) {
        expectations.expect(result->state() == TaskState::Succeeded, "typed task succeeds");
        expectations.expect(result->value() == 42, "typed result retains its concrete value");
    }
    const auto snapshot = scheduler.snapshot(submission.handle.id());
    expectations.expect(snapshot.has_value(), "terminal task remains observable");
    if (snapshot.has_value()) {
        expectations.expect(snapshot->progress.completed == 5,
                            "progress cannot move backwards within a phase");
        expectations.expect(snapshot->progress.total == 8, "a phase may refine its total estimate");
        expectations.expect(snapshot->diagnostics.size() == 1,
                            "structured diagnostics reach the task snapshot");
        expectations.expect(snapshot->sourceVersion.documentRevision == 7,
                            "source revision remains attached to the task");
    }
}

void testBackpressureAndPoolSeparation(Expectations& expectations) {
    using namespace bloom::runtime;
    TaskScheduler scheduler(testConfig(1, 1));
    Gate cpuGate;
    auto running = scheduler.submit<void>(
        TaskRequest("CPU blocker", owner(2)), [&cpuGate](TaskContext& context) {
            cpuGate.enterAndWait(context);
            return context.isCancellationRequested() ? TaskResult<void>::cancelled()
                                                     : TaskResult<void>::succeeded();
        });
    expectations.expect(running.accepted() && cpuGate.waitUntilEntered(),
                        "CPU worker begins the blocking fixture");

    auto queued =
        scheduler.submit<void>(TaskRequest("Queued CPU task", owner(2)),
                               [](TaskContext&) { return TaskResult<void>::succeeded(); });
    auto rejected =
        scheduler.submit<void>(TaskRequest("Overflow CPU task", owner(2)),
                               [](TaskContext&) { return TaskResult<void>::succeeded(); });
    expectations.expect(queued.accepted(), "bounded queue accepts work up to its capacity");
    expectations.expect(rejected.status == TaskSubmissionStatus::QueueFull,
                        "bounded queue applies explicit backpressure");

    auto io = scheduler.submit<int>(TaskRequest("Independent I/O", owner(2),
                                                TaskPriority::Background, TaskExecutor::BlockingIo),
                                    [](TaskContext&) { return TaskResult<int>::succeeded(9); });
    const auto ioResult = awaitResult(io.handle);
    expectations.expect(ioResult.has_value() && ioResult->value() == 9,
                        "blocking-I/O work runs while the CPU pool is occupied");
    cpuGate.release();
    expectations.expect(awaitResult(running.handle).has_value(), "CPU fixture completes");
    expectations.expect(awaitResult(queued.handle).has_value(), "queued CPU task completes");
}

void testFairScheduling(Expectations& expectations) {
    using namespace bloom::runtime;
    TaskScheduler scheduler(testConfig(32));
    Gate gate;
    auto blocker =
        scheduler.submit<void>(TaskRequest("Scheduler gate", owner(3), TaskPriority::Interactive),
                               [&gate](TaskContext& context) {
                                   gate.enterAndWait(context);
                                   return TaskResult<void>::succeeded();
                               });
    expectations.expect(blocker.accepted() && gate.waitUntilEntered(),
                        "fairness fixture occupies the worker");

    std::mutex orderMutex;
    std::vector<int> order;
    auto record = [&orderMutex, &order](const int value) {
        return [&orderMutex, &order, value](TaskContext&) {
            std::lock_guard lock(orderMutex);
            order.push_back(value);
            return TaskResult<void>::succeeded();
        };
    };
    auto background = scheduler.submit<void>(
        TaskRequest("Background", owner(3), TaskPriority::Background), record(100));
    std::vector<TaskHandle<void>> interactive;
    for (int index = 0; index < 16; ++index) {
        auto submission = scheduler.submit<void>(
            TaskRequest("Interactive", owner(3), TaskPriority::Interactive), record(index));
        expectations.expect(submission.accepted(), "interactive fairness fixture is accepted");
        interactive.push_back(std::move(submission.handle));
    }
    gate.release();
    expectations.expect(awaitQuiescence(scheduler), "fairness fixture reaches quiescence");
    const auto backgroundPosition = std::ranges::find(order, 100);
    expectations.expect(background.accepted() && backgroundPosition != order.end(),
                        "background task is eventually selected");
    if (backgroundPosition != order.end()) {
        const auto offset =
            static_cast<std::size_t>(std::distance(order.begin(), backgroundPosition));
        expectations.expect(offset <= 12,
                            "continuous interactive work cannot starve background work");
    }
}

void testCoalescing(Expectations& expectations) {
    using namespace bloom::runtime;
    TaskScheduler scheduler(testConfig());
    Gate gate;
    auto blocker =
        scheduler.submit<void>(TaskRequest("Coalescing gate", owner(4), TaskPriority::Interactive),
                               [&gate](TaskContext& context) {
                                   gate.enterAndWait(context);
                                   return TaskResult<void>::succeeded();
                               });
    expectations.expect(blocker.accepted() && gate.waitUntilEntered(),
                        "coalescing fixture occupies the worker");

    TaskRequest oldRequestSpec("Old preview", owner(4), TaskPriority::Visible);
    oldRequestSpec.coalescingKey = "preview";
    auto oldRequest = scheduler.submit<int>(
        std::move(oldRequestSpec), [](TaskContext&) { return TaskResult<int>::succeeded(1); });
    TaskRequest otherOwnerSpec("Other preview", owner(5), TaskPriority::Visible);
    otherOwnerSpec.coalescingKey = "preview";
    auto otherOwner = scheduler.submit<int>(
        std::move(otherOwnerSpec), [](TaskContext&) { return TaskResult<int>::succeeded(2); });
    TaskRequest replacementSpec("Current preview", owner(4), TaskPriority::Visible);
    replacementSpec.coalescingKey = "preview";
    auto replacement = scheduler.submit<int>(
        std::move(replacementSpec), [](TaskContext&) { return TaskResult<int>::succeeded(3); });

    const auto oldResult = awaitResult(oldRequest.handle);
    expectations.expect(oldResult.has_value() && oldResult->state() == TaskState::Cancelled,
                        "new equivalent work cancels the queued generation");
    gate.release();
    const auto otherResult = awaitResult(otherOwner.handle);
    const auto replacementResult = awaitResult(replacement.handle);
    expectations.expect(otherResult.has_value() && otherResult->value() == 2,
                        "coalescing is isolated by stable owner");
    expectations.expect(replacementResult.has_value() && replacementResult->value() == 3,
                        "replacement work completes normally");
}

void testOwnerAndGroupCancellation(Expectations& expectations) {
    using namespace bloom::runtime;
    TaskScheduler scheduler(testConfig());
    const TaskOwner taskOwner = owner(6, TaskOwnerKind::Project);
    auto groupSubmission = scheduler.createGroup(taskOwner, "Project scan");
    expectations.expect(groupSubmission.accepted(), "valid task group is created");
    const TaskGroupHandle group = groupSubmission.handle;

    Gate cpuGate;
    Gate ioGate;
    TaskRequest groupTaskSpec("Grouped CPU task", taskOwner, TaskPriority::Foreground);
    groupTaskSpec.groupId = group.id();
    auto groupTask =
        scheduler.submit<void>(std::move(groupTaskSpec), [&cpuGate](TaskContext& context) {
            cpuGate.enterAndWait(context);
            return context.isCancellationRequested() ? TaskResult<void>::cancelled()
                                                     : TaskResult<void>::succeeded();
        });
    auto ownerTask =
        scheduler.submit<void>(TaskRequest("Owned I/O task", taskOwner, TaskPriority::Foreground,
                                           TaskExecutor::BlockingIo),
                               [&ioGate](TaskContext& context) {
                                   ioGate.enterAndWait(context);
                                   return context.isCancellationRequested()
                                              ? TaskResult<void>::cancelled()
                                              : TaskResult<void>::succeeded();
                               });
    expectations.expect(groupTask.accepted() && ownerTask.accepted() &&
                            cpuGate.waitUntilEntered() && ioGate.waitUntilEntered(),
                        "owner cancellation fixture starts in both executors");
    expectations.expect(scheduler.cancelOwner(taskOwner) == 2,
                        "owner cancellation finds every active owned task");
    cpuGate.release();
    ioGate.release();
    const auto groupResult = awaitResult(groupTask.handle);
    const auto ownerResult = awaitResult(ownerTask.handle);
    expectations.expect(groupResult.has_value() && groupResult->state() == TaskState::Cancelled,
                        "owner cancellation propagates through the task group");
    expectations.expect(ownerResult.has_value() && ownerResult->state() == TaskState::Cancelled,
                        "owner cancellation reaches the independent executor");
    const auto groupSnapshot = group.snapshot();
    expectations.expect(groupSnapshot.has_value() && groupSnapshot->totalTasks == 1 &&
                            groupSnapshot->finishedTasks == 1,
                        "task group retains bounded aggregate completion state");

    auto explicitGroupSubmission = scheduler.createGroup(owner(60), "Explicit cancellation");
    Gate explicitGate;
    TaskRequest explicitSpec("Explicitly grouped task", owner(60));
    explicitSpec.groupId = explicitGroupSubmission.handle.id();
    auto explicitlyGrouped =
        scheduler.submit<void>(std::move(explicitSpec), [&explicitGate](TaskContext& context) {
            explicitGate.enterAndWait(context);
            return TaskResult<void>::succeeded();
        });
    expectations.expect(explicitlyGrouped.accepted() && explicitGate.waitUntilEntered(),
                        "explicit group cancellation fixture starts");
    explicitGroupSubmission.handle.cancel();
    explicitGate.release();
    const auto explicitlyCancelled = awaitResult(explicitlyGrouped.handle);
    expectations.expect(explicitlyCancelled.has_value() &&
                            explicitlyCancelled->state() == TaskState::Cancelled,
                        "task-group cancellation reaches a running child task");
}

void testFailuresAndHistoryBound(Expectations& expectations) {
    using namespace bloom::runtime;
    TaskScheduler scheduler(testConfig(16, 8, 2));
    auto failed = scheduler.submit<void>(
        TaskRequest("Throwing task", owner(7)),
        [](TaskContext&) -> TaskResult<void> { throw std::runtime_error("fixture failure"); });
    const auto failedResult = awaitResult(failed.handle);
    expectations.expect(failedResult.has_value() && failedResult->state() == TaskState::Failed,
                        "worker exceptions become failed outcomes");
    if (failedResult.has_value()) {
        expectations.expect(!failedResult->diagnostics().empty() &&
                                failedResult->diagnostics().front().code ==
                                    "bloom.runtime.unhandled-exception",
                            "exception failure contains a stable diagnostic code");
    }

    std::vector<bloom::runtime::TaskId> ids;
    for (int index = 0; index < 3; ++index) {
        auto item =
            scheduler.submit<int>(TaskRequest("History task", owner(7)), [index](TaskContext&) {
                return TaskResult<int>::succeeded(index);
            });
        ids.push_back(item.handle.id());
        expectations.expect(awaitResult(item.handle).has_value(), "history task completes");
    }
    expectations.expect(scheduler.snapshots().size() == 2,
                        "terminal task history remains strictly bounded");
    expectations.expect(!scheduler.snapshot(ids.front()).has_value(),
                        "oldest terminal record is evicted first");
}

void testOrderedFailureDiagnostics(Expectations& expectations) {
    using namespace bloom::runtime;
    TaskScheduler scheduler(testConfig());
    const std::vector<TaskDiagnostic> expected{{.code = "bloom.runtime.primary-failure",
                                                .severity = DiagnosticSeverity::Error,
                                                .summary = "Primary failure",
                                                .detail = {},
                                                .suggestedAction = {}},
                                               {.code = "bloom.runtime.secondary-failure",
                                                .severity = DiagnosticSeverity::Warning,
                                                .summary = "Secondary failure",
                                                .detail = {},
                                                .suggestedAction = {}}};
    auto typed = scheduler.submit<int>(
        TaskRequest("Ordered typed failure", owner(70)),
        [expected](TaskContext&) { return TaskResult<int>::failed(expected); });
    auto untyped = scheduler.submit<void>(
        TaskRequest("Ordered void failure", owner(71)),
        [expected](TaskContext&) { return TaskResult<void>::failed(expected); });

    const auto typedResult = awaitResult(typed.handle);
    const auto untypedResult = awaitResult(untyped.handle);
    expectations.expect(typedResult.has_value() && typedResult->diagnostics() == expected,
                        "typed failed results preserve every diagnostic in source order");
    expectations.expect(untypedResult.has_value() && untypedResult->diagnostics() == expected,
                        "void failed results preserve every diagnostic in source order");
    const auto typedSnapshot = scheduler.snapshot(typed.handle.id());
    const auto untypedSnapshot = scheduler.snapshot(untyped.handle.id());
    expectations.expect(typedSnapshot.has_value() && typedSnapshot->diagnostics == expected,
                        "typed task snapshots preserve ordered failure diagnostics");
    expectations.expect(untypedSnapshot.has_value() && untypedSnapshot->diagnostics == expected,
                        "void task snapshots preserve ordered failure diagnostics");
}

void testStagedShutdown(Expectations& expectations) {
    using namespace bloom::runtime;
    TaskScheduler scheduler(testConfig());
    Gate gate;
    auto running = scheduler.submit<void>(
        TaskRequest("Running shutdown task", owner(8)), [&gate](TaskContext& context) {
            gate.enterAndWait(context);
            return context.isCancellationRequested() ? TaskResult<void>::cancelled()
                                                     : TaskResult<void>::succeeded();
        });
    expectations.expect(running.accepted() && gate.waitUntilEntered(),
                        "shutdown fixture begins running");
    auto queued =
        scheduler.submit<void>(TaskRequest("Queued shutdown task", owner(8)),
                               [](TaskContext&) { return TaskResult<void>::succeeded(); });
    scheduler.beginShutdown();
    expectations.expect(!scheduler.isAccepting(), "shutdown stops task admission immediately");
    auto rejected = scheduler.submit<void>(TaskRequest("Late task", owner(8)), [](TaskContext&) {
        return TaskResult<void>::succeeded();
    });
    expectations.expect(rejected.status == TaskSubmissionStatus::ShuttingDown,
                        "late submission reports staged shutdown");
    const auto queuedResult = awaitResult(queued.handle);
    expectations.expect(queuedResult.has_value() && queuedResult->state() == TaskState::Cancelled,
                        "queued work reaches a deterministic cancelled outcome");
    gate.release();
    const auto runningResult = awaitResult(running.handle);
    expectations.expect(runningResult.has_value() && runningResult->state() == TaskState::Cancelled,
                        "running cooperative work observes shutdown cancellation");
    expectations.expect(awaitQuiescence(scheduler), "staged shutdown reaches quiescence");
}

} // namespace

int main() {
    Expectations expectations;
    testTypedResultAndProgress(expectations);
    testBackpressureAndPoolSeparation(expectations);
    testFairScheduling(expectations);
    testCoalescing(expectations);
    testOwnerAndGroupCancellation(expectations);
    testFailuresAndHistoryBound(expectations);
    testOrderedFailureDiagnostics(expectations);
    testStagedShutdown(expectations);

    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " runtime task expectation(s) failed\n";
        return 1;
    }
    return 0;
}
