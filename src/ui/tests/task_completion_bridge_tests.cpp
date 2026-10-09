#include <bloom/ui/task_ui_bridge.hpp>

#include <QCoreApplication>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <latch>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace bloom;
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
runtime::TaskSchedulerConfig config() {
    auto value = runtime::TaskSchedulerConfig::defaults();
    value.cpuWorkerCount = 1;
    value.blockingIoWorkerCount = 1;
    value.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    return value;
}
runtime::TaskRequest request() {
    return {"Completion bridge proof",
            {runtime::TaskOwnerKind::PanelRequest, runtime::TaskOwnerId::fromRaw(1)}};
}
template <typename Condition> bool spinUntil(Condition condition, const bool events = true) {
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (!condition() && std::chrono::steady_clock::now() < end) {
        if (events)
            QCoreApplication::processEvents();
        else
            std::this_thread::yield();
    }
    return condition();
}

void testStoppedMonitorAndBurst(Expectations& checks) {
    runtime::TaskScheduler scheduler(config());
    ui::TaskUiBridge bridge(scheduler); // Ordinary production 100 ms progress default.
    int polls = 0;
    int wakes = 0;
    std::vector<runtime::TaskHandle<void>> handles;
    std::vector<double> latencyMicros;
    std::size_t delivered = 0;
    QObject::connect(&bridge, &ui::TaskUiBridge::snapshotsPolled, &bridge, [&] { ++polls; });
    QObject::connect(&bridge, &ui::TaskUiBridge::resultsAvailable, &bridge, [&] {
        ++wakes;
        for (const auto& handle : handles) {
            if (handle.tryTakeResult()) {
                ++delivered;
                const auto snapshot = scheduler.snapshot(handle.id());
                if (snapshot && snapshot->finishedAt)
                    latencyMicros.push_back(
                        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() -
                                                                  *snapshot->finishedAt)
                            .count());
            }
        }
    });
    bridge.start();
    checks.expect(spinUntil([&] { return wakes > 0; }),
                  "start queues initial drain after consumers connect");
    bridge.stop();
    const auto priorPolls = polls;
    const auto priorWakes = bridge.completionWakeCount();
    std::latch entered{1}, release{1};
    auto held = scheduler.submit<void>(request(), [&](runtime::TaskContext&) {
        entered.count_down();
        release.wait();
        return runtime::TaskResult<void>::succeeded();
    });
    handles.push_back(held.handle);
    entered.wait();
    int busyPulses = 0;
    QTimer busyHeartbeat;
    busyHeartbeat.setInterval(1);
    QObject::connect(&busyHeartbeat, &QTimer::timeout, &bridge, [&] { ++busyPulses; });
    busyHeartbeat.start();
    checks.expect(
        spinUntil([&] { return busyPulses >= 4; }) && polls == priorPolls &&
            bridge.completionWakeCount() == priorWakes,
        "held long worker leaves UI responsive with stopped Jobs and no completion polling");
    busyHeartbeat.stop();
    for (int i = 0; i < 63; ++i) {
        const auto submitted = scheduler.submit<void>(request(), [](runtime::TaskContext&) {
            return runtime::TaskResult<void>::succeeded();
        });
        checks.expect(submitted.accepted(), "bounded burst is admitted");
        handles.push_back(submitted.handle);
    }
    release.count_down();
    checks.expect(spinUntil([&] { return scheduler.isQuiescent(); }, false),
                  "burst terminals finish before UI event queue is drained");
    checks.expect(
        spinUntil([&] { return delivered == 64; }) && polls == priorPolls && !bridge.isRunning(),
        "all 64 results deliver while Jobs monitor stays stopped with zero snapshot polls");
    checks.expect(bridge.completionWakeCount() - priorWakes <= 2,
                  "burst queues a bounded coalesced wake instead of one GUI callback per task");
    std::cout << "COMPLETION_BRIDGE burstTerminals=64 wakes="
              << bridge.completionWakeCount() - priorWakes
              << " additionalJobsPolls=" << polls - priorPolls << " delivered=" << delivered
              << " heldWorkerUiPulses=" << busyPulses;
    double maximum = 0;
    for (const auto value : latencyMicros)
        maximum = std::max(maximum, value);
    std::cout << " maxTerminalToUiMicros=" << maximum << '\n';
    const auto idleWakes = bridge.completionWakeCount();
    int pulses = 0;
    QTimer heartbeat;
    heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, &bridge, [&] { ++pulses; });
    heartbeat.start();
    checks.expect(
        spinUntil([&] { return pulses >= 4; }) && bridge.completionWakeCount() == idleWakes,
        "idle bridge has no recurring completion wake while event loop remains responsive");
    heartbeat.stop();
    scheduler.beginShutdown();
}

void testDuringDrainAndAlreadyReady(Expectations& checks) {
    runtime::TaskScheduler scheduler(config());
    auto initial = scheduler.submit<void>(
        request(), [](runtime::TaskContext&) { return runtime::TaskResult<void>::succeeded(); });
    checks.expect(spinUntil([&] { return scheduler.isQuiescent(); }, false),
                  "owned handle is already complete before bridge admission");
    ui::TaskUiBridge bridge(scheduler);
    std::optional<runtime::TaskHandle<void>> second;
    int delivered = 0;
    int wakes = 0;
    QObject::connect(&bridge, &ui::TaskUiBridge::resultsAvailable, &bridge, [&] {
        ++wakes;
        if (initial.handle.tryTakeResult()) {
            ++delivered;
            const auto submitted = scheduler.submit<void>(request(), [](runtime::TaskContext&) {
                return runtime::TaskResult<void>::succeeded();
            });
            second = submitted.handle;
            checks.expect(spinUntil([&] { return scheduler.isQuiescent(); }, false),
                          "second completion is deliberately inside first synchronous UI drain");
            return;
        }
        if (second && second->tryTakeResult())
            ++delivered;
    });
    bridge.wake();
    bridge.stop();
    checks.expect(
        spinUntil([&] { return delivered == 2; }) && wakes >= 2,
        "historical owned result and completion during drain both deliver without lost wake");
    scheduler.beginShutdown();
}

void testDestructionAndObserverDeletion(Expectations& checks) {
    runtime::TaskScheduler scheduler(config());
    {
        auto bridge = std::make_unique<ui::TaskUiBridge>(scheduler);
        bridge.reset(); // Idle serial wait: no task and no UI acknowledgement will arrive.
    }
    int oldSignals = 0;
    auto bridge = std::make_unique<ui::TaskUiBridge>(scheduler);
    QObject::connect(bridge.get(), &ui::TaskUiBridge::resultsAvailable,
                     QCoreApplication::instance(), [&] { ++oldSignals; });
    auto task = scheduler.submit<void>(
        request(), [](runtime::TaskContext&) { return runtime::TaskResult<void>::succeeded(); });
    checks.expect(spinUntil([&] { return scheduler.isQuiescent(); }, false),
                  "terminal is ready while old UI drain stays queued");
    bridge->wake(); // Guarantees a pending drain and a waiter that may be awaiting its UI ack.
    bridge.reset();
    QCoreApplication::processEvents();
    checks.expect(
        oldSignals == 0 && task.handle.tryTakeResult().has_value(),
        "destroying queued bridge interrupts ack wait, discards receiver events, keeps result");
    bridge = std::make_unique<ui::TaskUiBridge>(scheduler);
    QPointer<ui::TaskUiBridge> guarded(bridge.get());
    QObject::connect(bridge.get(), &ui::TaskUiBridge::resultsAvailable,
                     QCoreApplication::instance(), [&] { bridge.reset(); });
    bridge->wake();
    checks.expect(
        spinUntil([&] { return guarded.isNull(); }),
        "observer can delete bridge synchronously after neutral ack without later access");
    scheduler.beginShutdown();
}

void testConstructionRefusal(Expectations& checks) {
    runtime::TaskScheduler scheduler(config());
    std::vector<runtime::TaskCompletionSubscription> held;
    for (std::size_t i = 0; i < runtime::kTaskCompletionSubscriptionCapacity; ++i) {
        auto subscribed = scheduler.subscribeCompletions();
        held.push_back(std::move(subscribed.subscription));
    }
    bool refused = false;
    try {
        ui::TaskUiBridge bridge(scheduler);
    } catch (const std::exception& error) {
        refused = std::string(error.what()).find("could not be admitted") != std::string::npos;
    }
    checks.expect(refused && scheduler.isQuiescent(),
                  "construction reports bounded subscription refusal with safe partial cleanup");
    held.back().close();
    ui::TaskUiBridge recovered(scheduler);
    recovered.wake();
    recovered.stop();
    checks.expect(spinUntil([&] { return recovered.completionWakeCount() > 0; }),
                  "released slot admits functional adapter after failed construction");
    scheduler.beginShutdown();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    Expectations checks;
    testStoppedMonitorAndBurst(checks);
    testDuringDrainAndAlreadyReady(checks);
    testDestructionAndObserverDeletion(checks);
    testConstructionRefusal(checks);
    return checks.failures == 0 ? 0 : 1;
}
