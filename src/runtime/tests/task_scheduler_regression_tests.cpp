#include <bloom/runtime/task_scheduler.hpp>

#include "task_scheduler_internal.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
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
using bloom::runtime::TaskPriority;
using bloom::runtime::TaskRequest;
using bloom::runtime::TaskResult;
using bloom::runtime::TaskScheduler;
using bloom::runtime::TaskSchedulerConfig;
using bloom::runtime::TaskState;
using bloom::runtime::TaskSubmissionStatus;

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

[[nodiscard]] TaskSchedulerConfig testConfig(const std::size_t cpuCapacity = 64,
                                             const std::size_t historyCapacity = 256,
                                             const std::size_t cpuWorkers = 1,
                                             const std::size_t groupCapacity = 64) {
    return {.cpuWorkerCount = cpuWorkers,
            .blockingIoWorkerCount = 1,
            .cpuQueueCapacity = cpuCapacity,
            .blockingIoQueueCapacity = 16,
            .terminalHistoryCapacity = historyCapacity,
            .diagnosticsPerTask = 8,
            .groupRegistryCapacity = groupCapacity};
}

[[nodiscard]] TaskOwner owner(const std::uint64_t value,
                              const TaskOwnerKind kind = TaskOwnerKind::Composition) {
    return {.kind = kind, .id = TaskOwnerId::fromRaw(value)};
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

[[nodiscard]] bool awaitQuiescence(const TaskScheduler& scheduler) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (scheduler.isQuiescent()) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

[[nodiscard]] TaskHandle<void> startBlocker(TaskScheduler& scheduler, Gate& gate,
                                            Expectations& expectations) {
    auto blocker =
        scheduler.submit<void>(TaskRequest("Worker gate", owner(1000), TaskPriority::Interactive),
                               [&gate](TaskContext& context) {
                                   gate.enterAndWait(context);
                                   return context.isCancellationRequested()
                                              ? TaskResult<void>::cancelled()
                                              : TaskResult<void>::succeeded();
                               });
    expectations.expect(blocker.accepted() && gate.waitUntilEntered(),
                        "blocking fixture occupies the CPU worker");
    return blocker.handle;
}

void expectImmediateCancellation(Expectations& expectations, const TaskHandle<int>& handle,
                                 const std::string& label) {
    const auto result = handle.tryTakeResult();
    expectations.expect(result.has_value() && result->state() == TaskState::Cancelled,
                        label + " publishes a cancelled result before returning");
}

void testImmediateQueuedCancellationAndCapacity(Expectations& expectations) {
    TaskScheduler scheduler(testConfig(1));
    Gate gate;
    const auto blocker = startBlocker(scheduler, gate, expectations);

    auto byHandle =
        scheduler.submit<int>(TaskRequest("Handle cancellation", owner(1)),
                              [](TaskContext&) { return TaskResult<int>::succeeded(1); });
    byHandle.handle.cancel();
    expectImmediateCancellation(expectations, byHandle.handle, "handle cancellation");

    auto byId = scheduler.submit<int>(TaskRequest("ID cancellation", owner(1)),
                                      [](TaskContext&) { return TaskResult<int>::succeeded(2); });
    expectations.expect(scheduler.cancel(byId.handle.id()), "ID cancellation is accepted");
    expectImmediateCancellation(expectations, byId.handle, "ID cancellation");

    const TaskOwner ownedScope = owner(2, TaskOwnerKind::PanelRequest);
    auto byOwner =
        scheduler.submit<int>(TaskRequest("Owner cancellation", ownedScope),
                              [](TaskContext&) { return TaskResult<int>::succeeded(3); });
    expectations.expect(scheduler.cancelOwner(ownedScope) == 1,
                        "owner cancellation counts queued work");
    expectImmediateCancellation(expectations, byOwner.handle, "owner cancellation");

    const TaskOwner groupOwner = owner(3);
    auto group = scheduler.createGroup(groupOwner, "Cancellation group");
    TaskRequest groupedRequest("Group cancellation", groupOwner);
    groupedRequest.groupId = group.handle.id();
    auto byGroup = scheduler.submit<int>(
        std::move(groupedRequest), [](TaskContext&) { return TaskResult<int>::succeeded(4); });
    group.handle.cancel();
    expectImmediateCancellation(expectations, byGroup.handle, "group cancellation");

    TaskRequest rejectedRequest("Cancelled group child", groupOwner);
    rejectedRequest.groupId = group.handle.id();
    auto rejected = scheduler.submit<void>(
        std::move(rejectedRequest), [](TaskContext&) { return TaskResult<void>::succeeded(); });
    expectations.expect(rejected.status == TaskSubmissionStatus::CancelledGroup,
                        "cancelled groups reject later child submissions");

    auto capacityProbe =
        scheduler.submit<int>(TaskRequest("Capacity probe", owner(4)),
                              [](TaskContext&) { return TaskResult<int>::succeeded(5); });
    expectations.expect(capacityProbe.accepted(),
                        "each queued cancellation releases queue capacity immediately");
    capacityProbe.handle.cancel();
    expectImmediateCancellation(expectations, capacityProbe.handle, "capacity probe cleanup");

    gate.release();
    expectations.expect(awaitResult(blocker).has_value(), "blocking fixture completes");
    expectations.expect(awaitQuiescence(scheduler), "cancellation fixture reaches quiescence");
}

void testRunningCoalescingAndDiagnosticPreservation(Expectations& expectations) {
    TaskScheduler scheduler(testConfig());
    Gate gate;
    TaskRequest oldRequest("Old running preview", owner(5), TaskPriority::Visible);
    oldRequest.coalescingKey = "viewer";
    auto old = scheduler.submit<int>(std::move(oldRequest), [&gate](TaskContext& context) {
        gate.enterAndWait(context);
        return TaskResult<int>::succeeded(10,
                                          {{.code = "bloom.runtime.coalesced-warning",
                                            .severity = bloom::runtime::DiagnosticSeverity::Warning,
                                            .summary = "Preserved warning",
                                            .detail = {},
                                            .suggestedAction = {}}});
    });
    expectations.expect(old.accepted() && gate.waitUntilEntered(),
                        "old coalesced generation begins running");

    TaskRequest replacementRequest("Current preview", owner(5), TaskPriority::Visible);
    replacementRequest.coalescingKey = "viewer";
    auto replacement = scheduler.submit<int>(
        std::move(replacementRequest), [](TaskContext&) { return TaskResult<int>::succeeded(11); });
    expectations.expect(replacement.accepted() && old.handle.cancellationRequested(),
                        "admitted replacement requests cancellation of running equivalent work");
    gate.release();

    const auto oldResult = awaitResult(old.handle);
    const auto replacementResult = awaitResult(replacement.handle);
    expectations.expect(oldResult.has_value() && oldResult->state() == TaskState::Cancelled,
                        "late success from a coalesced generation becomes cancelled");
    expectations.expect(oldResult.has_value() && oldResult->diagnostics().size() == 1,
                        "cancellation preserves the task result diagnostics");
    expectations.expect(replacementResult.has_value() && replacementResult->value() == 11,
                        "replacement generation succeeds");
    expectations.expect(awaitQuiescence(scheduler), "running coalescing reaches quiescence");
}

void testRejectedReplacementPreservesOldWork(Expectations& expectations) {
    TaskScheduler scheduler(testConfig());
    Gate gate;
    const auto blocker = startBlocker(scheduler, gate, expectations);
    TaskRequest oldRequest("Retained preview", owner(6));
    oldRequest.coalescingKey = "preview";
    auto old = scheduler.submit<int>(std::move(oldRequest),
                                     [](TaskContext&) { return TaskResult<int>::succeeded(20); });

    TaskRequest invalidRequest("Invalid replacement", owner(6));
    invalidRequest.coalescingKey = "preview";
    bloom::runtime::TaskFunction<int> emptyFunction;
    auto invalid = scheduler.submit<int>(std::move(invalidRequest), std::move(emptyFunction));
    expectations.expect(invalid.status == TaskSubmissionStatus::InvalidRequest,
                        "invalid replacement is rejected before admission");
    expectations.expect(!old.handle.cancellationRequested(),
                        "rejected replacement does not cancel admitted work");

    gate.release();
    expectations.expect(awaitResult(blocker).has_value(), "replacement fixture releases blocker");
    const auto oldResult = awaitResult(old.handle);
    expectations.expect(oldResult.has_value() && oldResult->value() == 20,
                        "previous work survives rejected replacement admission");
    expectations.expect(awaitQuiescence(scheduler), "replacement fixture reaches quiescence");
}

void testTerminalResultCoherenceAndMultiworkerStress(Expectations& expectations) {
    constexpr std::size_t kTaskCount = 160;
    TaskScheduler scheduler(testConfig(256, 256, 4));
    std::mutex handlesMutex;
    std::vector<TaskHandle<int>> handles;
    handles.reserve(kTaskCount);
    std::vector<std::jthread> submitters;
    submitters.reserve(4);
    for (std::size_t producer = 0; producer < 4; ++producer) {
        submitters.emplace_back([producer, &scheduler, &handles, &handlesMutex] {
            for (std::size_t index = 0; index < kTaskCount / 4; ++index) {
                const int value = static_cast<int>(producer * 100 + index);
                auto item = scheduler.submit<int>(
                    TaskRequest("Concurrent task", owner(7)), [value](TaskContext& context) {
                        context.reportProgress(
                            {.phase = "Work", .subphase = {}, .completed = 1, .total = 1});
                        std::this_thread::yield();
                        return TaskResult<int>::succeeded(value);
                    });
                if (item.accepted()) {
                    std::lock_guard lock(handlesMutex);
                    handles.push_back(std::move(item.handle));
                }
            }
        });
    }
    submitters.clear();
    expectations.expect(handles.size() == kTaskCount,
                        "multiworker stress admits every bounded concurrent submission");

    for (std::size_t index = 0; index < handles.size(); index += 3) {
        handles[index].cancel();
    }
    std::vector<bool> consumed(handles.size(), false);
    std::size_t remaining = handles.size();
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (remaining != 0 && std::chrono::steady_clock::now() < deadline) {
        for (std::size_t index = 0; index < handles.size(); ++index) {
            if (consumed[index]) {
                continue;
            }
            const auto snapshot = scheduler.snapshot(handles[index].id());
            if (!snapshot.has_value() || !bloom::runtime::isTerminal(snapshot->state)) {
                continue;
            }
            const auto result = handles[index].tryTakeResult();
            expectations.expect(result.has_value(),
                                "a visible terminal snapshot always has a published result");
            if (result.has_value()) {
                consumed[index] = true;
                --remaining;
            }
        }
        std::this_thread::yield();
    }
    expectations.expect(remaining == 0, "all concurrent task results become observable");
    expectations.expect(awaitQuiescence(scheduler), "multiworker stress reaches quiescence");
}

void testReprioritizationOrder(Expectations& expectations) {
    TaskScheduler scheduler(testConfig());
    Gate gate;
    const auto blocker = startBlocker(scheduler, gate, expectations);
    std::mutex orderMutex;
    std::vector<int> order;
    auto recordOrder = [&orderMutex, &order](const int value) {
        return [&orderMutex, &order, value](TaskContext&) {
            std::lock_guard lock(orderMutex);
            order.push_back(value);
            return TaskResult<void>::succeeded();
        };
    };
    auto background = scheduler.submit<void>(
        TaskRequest("Reprioritized", owner(8), TaskPriority::Background), recordOrder(1));
    auto visible = scheduler.submit<void>(TaskRequest("Visible", owner(8), TaskPriority::Visible),
                                          recordOrder(2));
    expectations.expect(scheduler.reprioritize(background.handle.id(), TaskPriority::Interactive),
                        "queued task accepts a valid priority change");
    gate.release();
    expectations.expect(awaitResult(blocker).has_value(), "priority fixture releases blocker");
    expectations.expect(awaitResult(background.handle).has_value() &&
                            awaitResult(visible.handle).has_value(),
                        "reprioritization tasks complete");
    expectations.expect(order.size() == 2 && order.front() == 1,
                        "reprioritized interactive work runs before visible work");
    expectations.expect(awaitQuiescence(scheduler), "priority fixture reaches quiescence");
}

void testGroupRegistryBoundAndReclamation(Expectations& expectations) {
    TaskScheduler scheduler(testConfig(8, 16, 1, 2));
    auto first = scheduler.createGroup(owner(9), "First");
    auto second = scheduler.createGroup(owner(9), "Second");
    auto full = scheduler.createGroup(owner(9), "Full");
    expectations.expect(first.accepted() && second.accepted(),
                        "group registry accepts work within its configured bound");
    expectations.expect(full.status == TaskSubmissionStatus::GroupRegistryFull,
                        "live groups enforce the configured registry bound");
    first.handle = TaskGroupHandle{};
    auto reclaimed = scheduler.createGroup(owner(9), "Reclaimed");
    expectations.expect(reclaimed.accepted(),
                        "expired weak group entries are reclaimed on creation");
    expectations.expect(scheduler.isQuiescent(), "group-only scheduler remains quiescent");
}

void testGroupProgressAggregation(Expectations& expectations) {
    TaskScheduler scheduler(testConfig());
    const TaskOwner groupOwner = owner(90);
    auto group = scheduler.createGroup(groupOwner, "Progress group");
    Gate gate;
    TaskRequest request("Grouped progress", groupOwner);
    request.groupId = group.handle.id();
    auto task = scheduler.submit<void>(std::move(request), [&gate](TaskContext& context) {
        context.reportProgress({.phase = "Evaluate", .subphase = {}, .completed = 1, .total = 4});
        gate.enterAndWait(context);
        return TaskResult<void>::succeeded();
    });
    expectations.expect(task.accepted() && gate.waitUntilEntered(),
                        "grouped progress fixture begins running");
    const auto running = group.handle.snapshot();
    expectations.expect(running.has_value() && running->runningTasks == 1 &&
                            running->progress.completed == 250 && running->progress.total == 1'000,
                        "group progress aggregates a determinate child in fixed-point units");
    gate.release();
    expectations.expect(awaitResult(task.handle).has_value(), "grouped progress task completes");
    const auto finished = group.handle.snapshot();
    expectations.expect(finished.has_value() && finished->finishedTasks == 1 &&
                            finished->progress.completed == 1'000 &&
                            finished->progress.total == 1'000,
                        "finished group progress reaches its authoritative terminal aggregate");
    expectations.expect(awaitQuiescence(scheduler), "group progress fixture reaches quiescence");
}

void testShutdownHandoffAndTeardownContract(Expectations& expectations) {
    TaskScheduler scheduler(testConfig(64, 128));
    Gate gate;
    const auto blocker = startBlocker(scheduler, gate, expectations);
    std::vector<TaskHandle<int>> queued;
    for (int index = 0; index < 64; ++index) {
        auto item =
            scheduler.submit<int>(TaskRequest("Shutdown queue", owner(10)), [index](TaskContext&) {
                return TaskResult<int>::succeeded(index);
            });
        expectations.expect(item.accepted(), "shutdown fixture fills its bounded queue");
        queued.push_back(std::move(item.handle));
    }

    const auto started = std::chrono::steady_clock::now();
    scheduler.beginShutdown();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expectations.expect(elapsed < 250ms,
                        "shutdown initiation only performs bounded cancellation handoff");
    expectations.expect(!scheduler.isAccepting(), "shutdown closes admission immediately");
    for (const auto& handle : queued) {
        const auto result = awaitResult(handle);
        expectations.expect(result.has_value() && result->state() == TaskState::Cancelled,
                            "shutdown finalizer publishes queued cancellation independently");
    }
    expectations.expect(!scheduler.isQuiescent(),
                        "active non-wakeable work prevents premature quiescence");
    gate.release();
    const auto blockerResult = awaitResult(blocker);
    expectations.expect(blockerResult.has_value() && blockerResult->state() == TaskState::Cancelled,
                        "running work observes shutdown cancellation at completion");
    expectations.expect(awaitQuiescence(scheduler),
                        "owner can prove the non-blocking destruction precondition");
}

void testWorkerConstructionRollbackAndConfigBounds(Expectations& expectations) {
    const auto started = std::chrono::steady_clock::now();
    bool threw = false;
    try {
        auto scheduler = bloom::runtime::detail::TaskSchedulerTestAccess::create(
            testConfig(8, 16, 2), [](const std::size_t workerIndex) {
                if (workerIndex == 1) {
                    throw std::runtime_error("injected worker construction failure");
                }
            });
        static_cast<void>(scheduler);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expectations.expect(threw, "worker construction seam injects a deterministic failure");
    expectations.expect(std::chrono::steady_clock::now() - started < 1s,
                        "partially created worker pools roll back without hanging");

    TaskSchedulerConfig invalid = testConfig();
    invalid.cpuWorkerCount = 257;
    bool rejected = false;
    try {
        TaskScheduler scheduler(invalid);
        static_cast<void>(scheduler);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    expectations.expect(rejected, "unreasonable worker counts are rejected before allocation");
}

struct ThrowingMove {
    ThrowingMove(ThrowingMove&&) noexcept(false) {}
};

static_assert(!bloom::runtime::TaskResultValue<ThrowingMove>);
static_assert(bloom::runtime::TaskResultValue<std::shared_ptr<const int>>);

} // namespace

int main() {
    Expectations expectations;
    testImmediateQueuedCancellationAndCapacity(expectations);
    testRunningCoalescingAndDiagnosticPreservation(expectations);
    testRejectedReplacementPreservesOldWork(expectations);
    testTerminalResultCoherenceAndMultiworkerStress(expectations);
    testReprioritizationOrder(expectations);
    testGroupRegistryBoundAndReclamation(expectations);
    testGroupProgressAggregation(expectations);
    testShutdownHandoffAndTeardownContract(expectations);
    testWorkerConstructionRollbackAndConfigBounds(expectations);

    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " runtime regression expectation(s) failed\n";
        return 1;
    }
    return 0;
}
