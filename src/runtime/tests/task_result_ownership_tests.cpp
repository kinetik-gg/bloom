#include <bloom/runtime/task_gpu_executor.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace bloom::runtime;
using namespace std::chrono_literals;

void require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Predicate> void await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, "bounded fixture wait expired");
        std::this_thread::yield();
    }
}

TaskSchedulerConfig config() {
    auto result = TaskSchedulerConfig::defaults();
    result.cpuWorkerCount = 1;
    result.blockingIoWorkerCount = 1;
    result.rowBandWorkerCount = kSerialRowBandWorkers;
    result.terminalHistoryCapacity = 2;
    return result;
}

TaskOwner owner() { return {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(1)}; }

TaskDiagnostic failure() {
    return {.code = "test.forced",
            .summary = "Forced test terminal",
            .detail = {},
            .suggestedAction = {}};
}

class Gate final {
  public:
    ~Gate() { release(); }
    void hold() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }
    void entered() {
        std::unique_lock lock(mutex_);
        require(condition_.wait_for(lock, 2s, [this] { return entered_; }), "worker entered");
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

using Payload = std::shared_ptr<int>;

void abandonedAndCopiedHandles() {
    for (const auto executor : {TaskExecutor::Cpu, TaskExecutor::BlockingIo}) {
        TaskScheduler scheduler(config());
        auto notifications = scheduler.subscribeCompletions();
        auto payload = std::make_shared<int>(42);
        const std::weak_ptr<int> lifetime = payload;
        auto submission = scheduler.submit<Payload>(
            TaskRequest("Owned terminal payload", owner(), TaskPriority::Visible, executor),
            [payload](TaskContext&) { return TaskResult<Payload>::succeeded(payload); });
        require(submission.accepted(), "CPU/IO admitted");
        payload.reset();
        await([&] { return scheduler.isQuiescent() && notifications.subscription.serial() == 1; });
        require(!lifetime.expired(), "unconsumed live handle owns result");
        auto copy = submission.handle;
        const auto id = copy.id();
        submission.handle = {};
        require(!lifetime.expired(), "copied handle remains result owner");
        copy = {};
        require(lifetime.expired(), "last handle releases payload despite history");
        const auto snapshot = scheduler.snapshot(id);
        require(snapshot && snapshot->state == TaskState::Succeeded,
                "history preserves successful metadata without payload");
        require(notifications.subscription.serial() == 1, "one postcleanup terminal event");
    }

    TaskScheduler scheduler(config());
    Gate gate;
    auto payload = std::make_shared<int>(7);
    const std::weak_ptr<int> lifetime = payload;
    auto submission = scheduler.submit<Payload>(TaskRequest("Abandon running", owner()),
                                                [payload, &gate](TaskContext&) {
                                                    gate.hold();
                                                    return TaskResult<Payload>::succeeded(payload);
                                                });
    gate.entered();
    const auto id = submission.handle.id();
    payload.reset();
    submission.handle.cancel();
    submission.handle = {};
    require(!lifetime.expired(), "executing capture remains alive after abandonment/cancel");
    gate.release();
    await([&] { return scheduler.isQuiescent(); });
    require(lifetime.expired(), "abandoned running result and capture released at terminal");
    const auto snapshot = scheduler.snapshot(id);
    require(snapshot && snapshot->state == TaskState::Cancelled,
            "expired consumer preserves cancellation normalization");
}

void abandonedSuccessBeforeTerminal() {
    for (const auto executor : {TaskExecutor::Cpu, TaskExecutor::BlockingIo}) {
        TaskScheduler scheduler(config());
        auto notifications = scheduler.subscribeCompletions();
        Gate gate;
        auto payload = std::make_shared<int>(6);
        const std::weak_ptr<int> lifetime = payload;
        auto submission = scheduler.submit<Payload>(
            TaskRequest("Abandoned successful producer", owner(), TaskPriority::Visible, executor),
            [payload, &gate](TaskContext&) {
                gate.hold();
                return TaskResult<Payload>::succeeded(payload);
            });
        gate.entered();
        const auto id = submission.handle.id();
        payload.reset();
        submission.handle = {};
        require(!lifetime.expired(), "running body retains abandoned success capture");
        gate.release();
        await([&] { return notifications.subscription.serial() == 1; });
        require(scheduler.isQuiescent() && lifetime.expired(),
                "successful abandoned CPU/IO payload disposed before completion wake");
        const auto snapshot = scheduler.snapshot(id);
        require(snapshot && snapshot->state == TaskState::Succeeded,
                "expired success consumer does not change terminal outcome");
    }
}

void historyAndSchedulerLifetime() {
    TaskHandle<Payload> handle;
    std::weak_ptr<int> lifetime;
    {
        TaskScheduler scheduler(config());
        auto payload = std::make_shared<int>(11);
        lifetime = payload;
        auto submission = scheduler.submit<Payload>(
            TaskRequest("Retain across teardown", owner()),
            [payload](TaskContext&) { return TaskResult<Payload>::succeeded(payload); });
        handle = submission.handle;
        payload.reset();
        await([&] { return scheduler.isQuiescent(); });
        for (int index = 0; index < 5; ++index) {
            const auto other =
                scheduler.submit<void>(TaskRequest("Evict history", owner()),
                                       [](TaskContext&) { return TaskResult<void>::succeeded(); });
            require(other.accepted(), "history eviction task admitted");
            await([&] { return scheduler.isQuiescent(); });
        }
        require(!scheduler.snapshot(handle.id()), "original history evicted");
        require(!lifetime.expired(), "history eviction does not destroy handle result");
    }
    auto result = handle.tryTakeResult();
    require(result && result->state() == TaskState::Succeeded && result->value() &&
                **result->value() == 11,
            "live handle delivers after scheduler destruction");
    require(!handle.tryTakeResult(), "take exactly once after teardown");
    result.reset();
    require(lifetime.expired(), "taken payload releases independently of handle");
}

void concurrentTakeAndRejection() {
    TaskScheduler scheduler(config());
    auto submission =
        scheduler.submit<Payload>(TaskRequest("Concurrent take", owner()), [](TaskContext&) {
            return TaskResult<Payload>::succeeded(std::make_shared<int>(5));
        });
    await([&] { return scheduler.isQuiescent(); });
    std::atomic_size_t winners = 0;
    std::vector<std::jthread> takers;
    takers.reserve(8);
    for (int index = 0; index < 8; ++index) {
        takers.emplace_back([handle = submission.handle, &winners] {
            if (handle.tryTakeResult()) {
                ++winners;
            }
        });
    }
    takers.clear();
    require(winners == 1, "concurrent copied handles have exactly one winner");
    scheduler.beginShutdown();
    auto payload = std::make_shared<int>(4);
    const std::weak_ptr<int> lifetime = payload;
    auto rejected = scheduler.submit<Payload>(
        TaskRequest("Reject after shutdown", owner()),
        [payload](TaskContext&) { return TaskResult<Payload>::succeeded(payload); });
    payload.reset();
    require(!rejected.accepted() && lifetime.expired(), "rejected admission releases captures");
}

struct Reentry {
    std::function<void()> callback;
    std::atomic_size_t calls = 0;
};

class CallbackReset final {
  public:
    explicit CallbackReset(Reentry& observer) noexcept : observer_(observer) {}
    CallbackReset(const CallbackReset&) = delete;
    CallbackReset& operator=(const CallbackReset&) = delete;
    ~CallbackReset() { observer_.callback = {}; }

  private:
    Reentry& observer_;
};

// The moved-from object deliberately remains active. TaskResultValue does not promise inert
// moved-from destructors or move assignment; this catches disposal under mailbox/result locks.
struct ReentrantValue {
    // Fixture state outlives all values; moving leaves this non-owning observer active.
    Reentry* observer;
    Payload resource;
    ReentrantValue(Reentry& state, Payload value) : observer(&state), resource(std::move(value)) {}
    ReentrantValue(ReentrantValue&& other) noexcept
        : observer(other.observer), resource(std::move(other.resource)) {}
    ReentrantValue(const ReentrantValue&) = delete;
    ReentrantValue& operator=(const ReentrantValue&) = delete;
    ReentrantValue& operator=(ReentrantValue&&) = delete;
    ~ReentrantValue() noexcept {
        if (observer && observer->callback) {
            ++observer->calls;
            observer->callback();
        }
    }
};
static_assert(TaskResultValue<ReentrantValue>);
static_assert(!std::is_move_assignable_v<ReentrantValue>);

void reentrantTakeAndCancellation() {
    auto observer = std::make_shared<Reentry>();
    TaskScheduler scheduler(config());
    auto submission = scheduler.submit<ReentrantValue>(
        TaskRequest("Reentrant take", owner()), [observer](TaskContext&) {
            return TaskResult<ReentrantValue>::succeeded(
                ReentrantValue(*observer, std::make_shared<int>(17)));
        });
    await([&] { return scheduler.isQuiescent(); });
    const CallbackReset takeReset(*observer);
    observer->callback = [&] {
        static_cast<void>(scheduler.snapshots());
        require(!submission.handle.tryTakeResult(), "mailbox empty during moved-from destruction");
    };
    auto result = submission.handle.tryTakeResult();
    require(result && result->state() == TaskState::Succeeded, "reentrant move delivers value");
    require(observer->calls > 0, "moved-from destructor actually reentered");
    result.reset();
    observer->callback = {};

    Gate gate;
    TaskHandle<ReentrantValue> handle;
    const CallbackReset cancellationReset(*observer);
    observer->callback = [&] {
        static_cast<void>(scheduler.snapshots());
        static_cast<void>(handle.tryTakeResult());
    };
    auto held = scheduler.submit<ReentrantValue>(
        TaskRequest("Reentrant cancellation", owner()), [observer, &gate](TaskContext&) {
            gate.hold();
            return TaskResult<ReentrantValue>::succeeded(
                ReentrantValue(*observer, std::make_shared<int>(18)));
        });
    handle = held.handle;
    gate.entered();
    handle.cancel();
    gate.release();
    await([&] { return scheduler.isQuiescent(); });
    auto cancelled = handle.tryTakeResult();
    require(cancelled && cancelled->state() == TaskState::Cancelled && !cancelled->value(),
            "cancellation payload destructor reenters before mailbox publication");
    observer->callback = {};
}

void forcedGpuAndLateCompletion() {
    const auto generation = GpuServiceGeneration::fromRaw(1);
    if (!generation) {
        throw std::runtime_error("valid generation");
    }
    for (int mode = 0; mode < 3; ++mode) {
        for (const bool keepHandle : {false, true}) {
            auto observer = std::make_shared<Reentry>();
            TaskScheduler scheduler(config());
            auto notifications = scheduler.subscribeCompletions();
            auto attachment = scheduler.attachGpuExecutor(*generation, [] {});
            require(attachment.attached(), "fake GPU attached");
            std::optional<GpuTaskCompletion<ReentrantValue>> completion;
            auto submission = scheduler.submitGpu<ReentrantValue>(
                TaskRequest("Forced fake GPU", owner()), *generation, {16, 32},
                [&completion](TaskContext&, GpuTaskCompletion<ReentrantValue> token) {
                    completion.emplace(std::move(token));
                });
            const auto id = submission.handle.id();
            require(attachment.lease.dispatchOne() == GpuDispatchStatus::Dispatched,
                    "fake GPU dispatch retained token");
            if (!keepHandle) {
                submission.handle = {};
            }
            const CallbackReset forcedReset(*observer);
            observer->callback = [&] { static_cast<void>(scheduler.snapshots()); };
            if (mode == 0) {
                attachment.lease.detach();
            } else if (mode == 1) {
                require(attachment.lease.reportDeviceLost(failure()), "device loss wins terminal");
            } else {
                scheduler.beginShutdown();
                require(attachment.lease.forceShutdownFallback(), "forced shutdown wins terminal");
            }
            await([&] { return scheduler.isQuiescent(); });
            const auto expected = mode == 2 ? TaskState::Cancelled : TaskState::Failed;
            const auto terminal = scheduler.snapshot(id);
            require(terminal && terminal->state == expected, "forced metadata preserved");
            require(notifications.subscription.serial() == 1, "forced event exactly once");
            auto payload = std::make_shared<int>(9);
            const std::weak_ptr<int> lifetime = payload;
            if (!completion) {
                throw std::runtime_error("late token remains live");
            }
            require(std::move(*completion).succeed(ReentrantValue(*observer, std::move(payload))),
                    "late token consumed without second terminal");
            completion.reset();
            require(lifetime.expired(), "late payload cannot reinstate sealed pending storage");
            require(notifications.subscription.serial() == 1, "late completion no extra event");
            if (keepHandle) {
                const auto result = submission.handle.tryTakeResult();
                require(result && result->state() == expected && !result->value(),
                        "live handle preserves forced winner");
            }
            require(observer->calls > 0,
                    "late moved-from destruction reentered unlocked scheduler");
            observer->callback = {};
        }
    }
}

void abandonedGpuSuccessAndWrongThreadOrdering() {
    const auto generation = GpuServiceGeneration::fromRaw(1);
    if (!generation) {
        throw std::runtime_error("valid fake GPU generation");
    }
    for (const bool wrongThread : {false, true}) {
        auto observer = std::make_shared<Reentry>();
        TaskScheduler scheduler(config());
        auto notifications = scheduler.subscribeCompletions();
        std::atomic_size_t releases = 0;
        std::atomic_bool releasedBeforeTerminal = false;
        std::atomic_bool terminalObservedRelease = false;
        std::optional<GpuTaskCompletion<ReentrantValue>> token;
        TaskId id;
        auto payload = Payload(new int(31), [&](int* value) {
            const auto snapshot = scheduler.snapshot(id);
            releasedBeforeTerminal = notifications.subscription.serial() == 0 && snapshot &&
                                     snapshot->state == TaskState::Running;
            ++releases;
            delete value;
        });
        const std::weak_ptr<int> lifetime = payload;
        auto attachment = scheduler.attachGpuExecutor(*generation, [&] {
            if (notifications.subscription.serial() == 1) {
                terminalObservedRelease =
                    releases == 1 && lifetime.expired() && scheduler.isQuiescent();
            }
        });
        require(attachment.attached(), "fake GPU ordering attachment");
        auto submission = scheduler.submitGpu<ReentrantValue>(
            TaskRequest("GPU discarded consumer", owner()), *generation, {16, 32},
            [&token](TaskContext&, GpuTaskCompletion<ReentrantValue> completion) {
                token.emplace(std::move(completion));
            });
        id = submission.handle.id();
        require(attachment.lease.dispatchOne() == GpuDispatchStatus::Dispatched,
                "GPU ordering dispatch");
        // The wrong-thread case keeps a live consumer to distinguish rejected input from the
        // failure result. Ordinary success abandons the consumer while actual work is in flight.
        if (!wrongThread) {
            submission.handle = {};
        }
        const CallbackReset orderingReset(*observer);
        observer->callback = [&] { static_cast<void>(scheduler.snapshots()); };
        if (!token) {
            throw std::runtime_error("GPU ordering token retained");
        }
        if (wrongThread) {
            std::jthread intruder(
                [completion = std::move(*token), observer, value = std::move(payload)]() mutable {
                    static_cast<void>(
                        std::move(completion).succeed(ReentrantValue(*observer, std::move(value))));
                });
            intruder.join();
        } else {
            require(std::move(*token).succeed(ReentrantValue(*observer, std::move(payload))),
                    "abandoned ordinary GPU success consumed");
        }
        token.reset();
        require(notifications.subscription.serial() == 1 && scheduler.isQuiescent(),
                "one successful or rejected GPU terminal accounting event");
        require(releases == 1 && releasedBeforeTerminal && terminalObservedRelease,
                "discarded GPU input released before terminal metadata/accounting/notification");
        const auto snapshot = scheduler.snapshot(id);
        require(snapshot &&
                    snapshot->state == (wrongThread ? TaskState::Failed : TaskState::Succeeded),
                "GPU outcome remains independent of consumer lifetime");
        if (wrongThread) {
            const auto result = submission.handle.tryTakeResult();
            require(result && result->state() == TaskState::Failed && !result->value(),
                    "wrong-thread failure delivered without rejected payload");
        }
        observer->callback = {};
    }
}

void stagedGpuReplacement() {
    using Work = detail::GpuTypedTaskWork<ReentrantValue>;
    auto observer = std::make_shared<Reentry>();
    TaskScheduler scheduler(config());
    auto mailbox = std::make_shared<detail::ResultMailbox<TaskResult<ReentrantValue>>>();
    auto payload = std::make_shared<int>(23);
    const std::weak_ptr<int> lifetime = payload;
    auto work = std::make_shared<Work>(
        [observer, payload](TaskContext&, GpuTaskCompletion<ReentrantValue> completion) {
            static_cast<void>(std::move(completion).succeed(ReentrantValue(*observer, payload)));
        },
        mailbox);
    payload.reset();
    // An unattached neutral core intentionally stops before scheduler finalization, exposing the
    // staged-result race deterministically without changing production terminal seams/test hooks.
    const auto dispatch =
        scheduler.submit<void>(TaskRequest("Stage only", owner()), [work](TaskContext& context) {
            detail::GpuCompletionCore core;
            core.serviceThread = std::this_thread::get_id();
            work->dispatch(core, context);
            return TaskResult<void>::succeeded();
        });
    require(dispatch.accepted(), "staging driver admitted");
    await([&] { return scheduler.isQuiescent(); });
    require(!lifetime.expired(), "pending GPU result owns payload before forced replacement");
    const CallbackReset replacementReset(*observer);
    observer->callback = [&] {
        static_cast<void>(scheduler.snapshots());
        static_cast<void>(work->isValid());
        work->stageForcedFailure(failure());
    };
    work->stageForcedFailure(failure());
    require(lifetime.expired(), "forced replacement releases old payload outside result lock");
    require(observer->calls > 0, "forced replacement destructor reentered result/dispatch locks");
    require(work->publish(TaskState::Failed) == TaskState::Failed,
            "pre-staged forced result published");
    observer->callback = {};
}
} // namespace

int main() {
    try {
        abandonedAndCopiedHandles();
        abandonedSuccessBeforeTerminal();
        historyAndSchedulerLifetime();
        concurrentTakeAndRejection();
        reentrantTakeAndCancellation();
        forcedGpuAndLateCompletion();
        abandonedGpuSuccessAndWrongThreadOrdering();
        stagedGpuReplacement();
        std::cout << "CPU/IO/GPU result ownership, take-once, reentry and forced/late gates PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
