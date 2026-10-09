#include <bloom/runtime/task_scheduler.hpp>

#include "task_scheduler_internal.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <latch>
#include <limits>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace bloom::runtime;
using namespace std::chrono_literals;

struct Expectations final {
    int failures = 0;
    void expect(bool valid, const char* message,
                const std::source_location at = std::source_location::current()) {
        if (!valid) {
            ++failures;
            std::cerr << at.file_name() << ':' << at.line() << ": " << message << '\n';
        }
    }
};

TaskSchedulerConfig config() {
    auto result = TaskSchedulerConfig::defaults();
    result.cpuWorkerCount = 1;
    result.blockingIoWorkerCount = 1;
    result.rowBandWorkerCount = kSerialRowBandWorkers;
    return result;
}

TaskRequest request(const char* name, const TaskExecutor executor = TaskExecutor::Cpu) {
    TaskRequest result(name, {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(1)});
    result.executor = executor;
    return result;
}

bool awaitSerial(const TaskCompletionSubscription& subscription, std::uint64_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (subscription.serial() < expected && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return subscription.serial() == expected;
}

TaskDiagnostic failure() {
    return {.code = "bloom.test.completion",
            .severity = DiagnosticSeverity::Error,
            .summary = "Expected completion test failure",
            .detail = {},
            .suggestedAction = {}};
}

void testCpuAndIoTerminals(Expectations& checks) {
    TaskScheduler scheduler(config());
    auto first = scheduler.subscribeCompletions();
    auto second = scheduler.subscribeCompletions();
    checks.expect(first.status == TaskCompletionSubscribeStatus::Accepted &&
                      second.status == TaskCompletionSubscribeStatus::Accepted,
                  "two independent subscribers are admitted");
    std::uint64_t count = 0;
    for (const auto executor : {TaskExecutor::Cpu, TaskExecutor::BlockingIo}) {
        for (const auto outcome : {TaskState::Succeeded, TaskState::Failed, TaskState::Cancelled}) {
            auto submitted = scheduler.submit<int>(
                request("CPU/IO terminal", executor), [outcome](TaskContext&) {
                    if (outcome == TaskState::Failed)
                        return TaskResult<int>::failed(failure());
                    if (outcome == TaskState::Cancelled)
                        return TaskResult<int>::cancelled();
                    return TaskResult<int>::succeeded(7);
                });
            ++count;
            checks.expect(awaitSerial(first.subscription, count) &&
                              awaitSerial(second.subscription, count),
                          "every CPU/IO terminal advances each subscriber once");
            const auto result = submitted.handle.tryTakeResult();
            const auto snapshot = scheduler.snapshot(submitted.handle.id());
            checks.expect(result && result->state() == outcome && snapshot &&
                              snapshot->state == outcome && scheduler.isQuiescent(),
                          "wake follows mailbox, terminal snapshot/history and active accounting");
        }
    }
    auto thrown = scheduler.submit<void>(request("Throw"), [](TaskContext&) -> TaskResult<void> {
        throw std::runtime_error("expected");
    });
    checks.expect(awaitSerial(first.subscription, ++count), "unhandled failure wakes once");
    const auto thrownResult = thrown.handle.tryTakeResult();
    checks.expect(thrownResult && thrownResult->state() == TaskState::Failed,
                  "exception mailbox is present at wake");
    auto rejected = scheduler.submit<int>(request("Empty"), {});
    checks.expect(!rejected.accepted() && first.subscription.serial() == count,
                  "rejected admission publishes no terminal notice");
    auto late = scheduler.subscribeCompletions();
    checks.expect(late.subscription.serial() == count,
                  "subscription begins at current global serial without replaying old terminals");
    scheduler.beginShutdown();
    checks.expect(first.subscription.serial() == count,
                  "idle shutdown request is not a task terminal");
}

void testQueuedAndRunningCancellation(Expectations& checks) {
    TaskScheduler scheduler(config());
    auto watched = scheduler.subscribeCompletions();
    std::latch entered{1}, release{1};
    auto held = scheduler.submit<void>(request("Held"), [&](TaskContext&) {
        entered.count_down();
        release.wait();
        return TaskResult<void>::succeeded();
    });
    entered.wait();
    auto removed = scheduler.submit<void>(
        request("Removed"), [](TaskContext&) { return TaskResult<void>::succeeded(); });
    removed.handle.cancel();
    checks.expect(awaitSerial(watched.subscription, 1),
                  "queued removal wakes once after finalizer");
    const auto removedResult = removed.handle.tryTakeResult();
    checks.expect(removedResult && removedResult->state() == TaskState::Cancelled,
                  "removed queued result is available at wake");
    auto older = request("Older");
    older.coalescingKey = "completion-test";
    auto superseded = scheduler.submit<void>(
        std::move(older), [](TaskContext&) { return TaskResult<void>::succeeded(); });
    auto newer = request("Newer");
    newer.coalescingKey = "completion-test";
    auto newest = scheduler.submit<void>(
        std::move(newer), [](TaskContext&) { return TaskResult<void>::succeeded(); });
    checks.expect(awaitSerial(watched.subscription, 2), "coalesced queued terminal wakes once");
    const auto supersededResult = superseded.handle.tryTakeResult();
    checks.expect(supersededResult && supersededResult->state() == TaskState::Cancelled,
                  "coalescing notice follows canceled mailbox");
    held.handle.cancel();
    checks.expect(watched.subscription.serial() == 2,
                  "running cancellation request does not invent a terminal notice");
    release.count_down();
    checks.expect(awaitSerial(watched.subscription, 4), "held cancellation and newest finish once");
    const auto heldResult = held.handle.tryTakeResult();
    const auto newestResult = newest.handle.tryTakeResult();
    checks.expect(heldResult && heldResult->state() == TaskState::Cancelled && newestResult &&
                      newestResult->state() == TaskState::Succeeded && scheduler.isQuiescent(),
                  "running publication preserves cancellation and final accounting");
    scheduler.beginShutdown();
}

void testGpuTerminals(Expectations& checks) {
    TaskScheduler scheduler(config());
    auto watched = scheduler.subscribeCompletions();
    const auto generation = GpuServiceGeneration::fromRaw(1);
    if (!generation) {
        checks.expect(false, "valid GPU generation");
        return;
    }
    auto attachment = scheduler.attachGpuExecutor(*generation, [] {});
    std::uint64_t count = 0;
    for (const auto outcome : {TaskState::Succeeded, TaskState::Failed, TaskState::Cancelled}) {
        auto submitted = scheduler.submitGpu<int>(
            request("GPU terminal"), *generation, {1, 1},
            [outcome](TaskContext&, GpuTaskCompletion<int> completion) {
                if (outcome == TaskState::Succeeded)
                    static_cast<void>(std::move(completion).succeed(7));
                else if (outcome == TaskState::Failed)
                    static_cast<void>(std::move(completion).fail(failure()));
                else
                    static_cast<void>(std::move(completion).cancel());
            });
        checks.expect(attachment.lease.dispatchOne() == GpuDispatchStatus::Dispatched,
                      "GPU starter dispatches");
        checks.expect(awaitSerial(watched.subscription, ++count),
                      "GPU winning terminal wakes once");
        const auto result = submitted.handle.tryTakeResult();
        const auto snapshot = scheduler.snapshot(submitted.handle.id());
        checks.expect(result && result->state() == outcome && snapshot &&
                          snapshot->state == outcome && scheduler.isQuiescent(),
                      "GPU wake follows mailbox/history/accounting cleanup");
    }
    std::optional<GpuTaskCompletion<int>> token;
    auto running = scheduler.submitGpu<int>(request("Held GPU"), *generation, {1, 1},
                                            [&](TaskContext&, GpuTaskCompletion<int> completion) {
                                                token.emplace(std::move(completion));
                                            });
    static_cast<void>(attachment.lease.dispatchOne());
    auto queued = scheduler.submitGpu<int>(request("Queued GPU"), *generation, {1, 1},
                                           [](TaskContext&, GpuTaskCompletion<int> completion) {
                                               static_cast<void>(std::move(completion).succeed(9));
                                           });
    checks.expect(attachment.lease.forceShutdownFallback(), "forced GPU shutdown accepted");
    count += 2;
    checks.expect(awaitSerial(watched.subscription, count) && scheduler.isQuiescent(),
                  "forced running and queued GPU terminals each notify after finalizing cleanup");
    const auto runningResult = running.handle.tryTakeResult();
    const auto queuedResult = queued.handle.tryTakeResult();
    checks.expect(runningResult && queuedResult && runningResult->state() == TaskState::Cancelled &&
                      queuedResult->state() == TaskState::Cancelled,
                  "forced notices expose terminal mailboxes");
    if (token)
        static_cast<void>(std::move(*token).succeed(42));
    token.reset();
    checks.expect(watched.subscription.serial() == count,
                  "late forced token cannot publish a duplicate terminal notice");
    scheduler.beginShutdown();
}

void testSubscriptionLifetimeAndBounds(Expectations& checks) {
    auto scheduler = std::make_unique<TaskScheduler>(config());
    std::vector<TaskCompletionSubscription> subscriptions;
    for (std::size_t i = 0; i < kTaskCompletionSubscriptionCapacity; ++i) {
        auto admitted = scheduler->subscribeCompletions();
        checks.expect(admitted.status == TaskCompletionSubscribeStatus::Accepted,
                      "bounded subscriber slot is admitted");
        subscriptions.push_back(std::move(admitted.subscription));
    }
    checks.expect(scheduler->subscribeCompletions().status ==
                      TaskCompletionSubscribeStatus::CapacityReached,
                  "slot exhaustion is typed without polling fallback");
    subscriptions.back().close();
    subscriptions.back().close();
    auto reused = scheduler->subscribeCompletions();
    checks.expect(reused.status == TaskCompletionSubscribeStatus::Accepted,
                  "closed slot is reused even while old closed object remains alive");
    subscriptions.pop_back();
    subscriptions.push_back(std::move(reused.subscription));
    std::promise<bool> stopped;
    auto stoppedFuture = stopped.get_future();
    std::latch waiting{1};
    std::jthread waiter([&](const std::stop_token& stop) {
        waiting.count_down();
        stopped.set_value(!subscriptions.front().waitForChange(0, stop));
    });
    waiting.wait();
    waiter.request_stop();
    checks.expect(stoppedFuture.wait_for(2s) == std::future_status::ready && stoppedFuture.get(),
                  "stop interrupts an idle wait without another terminal or UI work");
    waiter.join();
    std::promise<bool> closed;
    auto closedFuture = closed.get_future();
    std::jthread closing([&](const std::stop_token& stop) {
        closed.set_value(!subscriptions.front().waitForChange(0, stop));
    });
    scheduler.reset();
    checks.expect(closedFuture.wait_for(2s) == std::future_status::ready && closedFuture.get() &&
                      subscriptions.front().state() == TaskCompletionSubscriptionState::Closed,
                  "scheduler teardown closes and wakes independent subscriptions without terminal");
    closing.request_stop();
    closing.join();
}

void testWaitPublicationCloseAndSubscribeRace(Expectations& checks) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        TaskScheduler scheduler(config());
        auto watched = scheduler.subscribeCompletions();
        std::latch entered{1}, release{1};
        auto task = scheduler.submit<void>(request("Racing publication"), [&](TaskContext&) {
            entered.count_down();
            release.wait();
            return TaskResult<void>::succeeded();
        });
        entered.wait();
        std::promise<bool> received;
        auto ready = received.get_future();
        std::jthread waiter([&](const std::stop_token& stop) {
            const auto changed = watched.subscription.waitForChange(0, stop);
            const auto snapshot = scheduler.snapshot(task.handle.id());
            received.set_value(changed && *changed == 1 && snapshot &&
                               snapshot->state == TaskState::Succeeded && scheduler.isQuiescent());
        });
        release.count_down();
        auto raced = scheduler.subscribeCompletions();
        checks.expect(
            ready.wait_for(2s) == std::future_status::ready && ready.get(),
            "wait wakes after terminal/history/accounting, including completion-before-wait");
        waiter.request_stop();
        waiter.join();
        checks.expect(awaitSerial(raced.subscription, 1) && task.handle.tryTakeResult().has_value(),
                      "subscribe race either sees serial baseline or receives next notice");
        watched.subscription.close();
        std::latch secondEntered{1}, secondRelease{1};
        auto racing = scheduler.submit<void>(request("Close race"), [&](TaskContext&) {
            secondEntered.count_down();
            secondRelease.wait();
            return TaskResult<void>::succeeded();
        });
        secondEntered.wait();
        std::promise<bool> closed;
        auto closedReady = closed.get_future();
        std::jthread closingWaiter([&](const std::stop_token& stop) {
            const auto result = raced.subscription.waitForChange(1, stop);
            closed.set_value(!result || *result == 2);
        });
        std::jthread closer([&] { raced.subscription.close(); });
        secondRelease.count_down();
        checks.expect(closedReady.wait_for(2s) == std::future_status::ready && closedReady.get(),
                      "concurrent close/publication safely delivers either notice or closed state");
        closingWaiter.request_stop();
        closingWaiter.join();
        closer.join();
        auto observer = scheduler.subscribeCompletions();
        checks.expect(awaitSerial(observer.subscription, 2) &&
                          racing.handle.tryTakeResult().has_value(),
                      "closing subscriber cannot consume or block another terminal result");
        scheduler.beginShutdown();
    }
}

void testShutdownAndAdditionalGpuPaths(Expectations& checks) {
    {
        TaskScheduler scheduler(config());
        auto watched = scheduler.subscribeCompletions();
        std::latch entered{1}, release{1};
        auto running = scheduler.submit<void>(request("Shutdown running"), [&](TaskContext&) {
            entered.count_down();
            release.wait();
            return TaskResult<void>::succeeded();
        });
        entered.wait();
        auto queued = scheduler.submit<void>(
            request("Shutdown queued"), [](TaskContext&) { return TaskResult<void>::succeeded(); });
        scheduler.beginShutdown();
        checks.expect(awaitSerial(watched.subscription, 1),
                      "shutdown removed queued work produces actual terminal notice");
        release.count_down();
        checks.expect(awaitSerial(watched.subscription, 2) &&
                          running.handle.tryTakeResult().has_value() &&
                          queued.handle.tryTakeResult().has_value() && scheduler.isQuiescent(),
                      "shutdown terminal notices converge after all finalizing work");
    }
    for (int mode = 0; mode < 4; ++mode) {
        TaskScheduler scheduler(config());
        auto watched = scheduler.subscribeCompletions();
        const auto generation = GpuServiceGeneration::fromRaw(1);
        if (!generation)
            std::terminate();
        auto attachment = scheduler.attachGpuExecutor(*generation, [] {});
        std::optional<GpuTaskCompletion<int>> token;
        auto task = scheduler.submitGpu<int>(
            request("GPU failure paths"), *generation, {1, 1},
            [&](TaskContext&, GpuTaskCompletion<int> completion) {
                if (mode == 0)
                    throw std::runtime_error("expected starter failure");
                if (mode == 1)
                    return; // Abandoned token publishes Failed after the starter returns.
                token.emplace(std::move(completion));
            });
        static_cast<void>(attachment.lease.dispatchOne());
        if (mode == 2)
            checks.expect(attachment.lease.reportDeviceLost(failure()),
                          "device loss force-terminalizes retained token");
        if (mode == 3)
            attachment.lease.detach();
        checks.expect(awaitSerial(watched.subscription, 1) && scheduler.isQuiescent(),
                      "starter throw/drop/device loss/detach each produces one finished notice");
        const auto result = task.handle.tryTakeResult();
        checks.expect(result && result->state() == TaskState::Failed,
                      "GPU failure path has mailbox at wake");
        token.reset();
        checks.expect(watched.subscription.serial() == 1,
                      "late abandoned token after forced terminal cannot increment serial");
        scheduler.beginShutdown();
    }
}

void testSerialExhaustion(Expectations& checks) {
    TaskScheduler scheduler(config());
    detail::TaskSchedulerTestAccess::setCompletionSerial(scheduler,
                                                         std::numeric_limits<std::uint64_t>::max());
    auto watched = scheduler.subscribeCompletions();
    auto task = scheduler.submit<void>(request("Last serial"),
                                       [](TaskContext&) { return TaskResult<void>::succeeded(); });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (watched.subscription.state() == TaskCompletionSubscriptionState::Open &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    checks.expect(
        watched.subscription.state() == TaskCompletionSubscriptionState::SerialExhausted &&
            !watched.subscription.waitForChange(watched.subscription.serial(), {}) &&
            scheduler.subscribeCompletions().status ==
                TaskCompletionSubscribeStatus::SerialExhausted &&
            task.handle.tryTakeResult().has_value() && scheduler.isQuiescent(),
        "serial exhaustion explicitly closes wake service without wrapping or losing result");
    scheduler.beginShutdown();
}
} // namespace

int main() {
    Expectations checks;
    testCpuAndIoTerminals(checks);
    testQueuedAndRunningCancellation(checks);
    testGpuTerminals(checks);
    testSubscriptionLifetimeAndBounds(checks);
    testWaitPublicationCloseAndSubscribeRace(checks);
    testShutdownAndAdditionalGpuPaths(checks);
    testSerialExhaustion(checks);
    return checks.failures == 0 ? 0 : 1;
}
