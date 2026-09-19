#pragma once

// Shared harness for the RAM preview two-deep pipeline tests. Split out so each focused test
// translation unit stays readable; every helper is inline, and nothing here owns a global.

#include <bloom/commands/command_stack.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_plan_cache.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <source_location>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::ui::ram_pipeline_test {

namespace chrono_literals = std::chrono_literals;

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

template <typename Predicate> inline bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 20'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::invoke(predicate)) {
            return true;
        }
        std::this_thread::yield();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return std::invoke(predicate);
}

[[nodiscard]] inline runtime::TaskSchedulerConfig twoWorkerConfig() {
    runtime::TaskSchedulerConfig config;
    config.cpuWorkerCount = 2;
    config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    config.blockingIoWorkerCount = 1;
    config.cpuQueueCapacity = 32;
    config.blockingIoQueueCapacity = 8;
    config.gpuPendingQueueCapacity = 8;
    config.gpuAdmittedStateCapacity = 16;
    config.gpuLiveContinuationCapacity = 8;
    config.gpuQueuedCommandByteCapacity = std::size_t{1} << 30U;
    config.gpuRequestOwnedByteCapacity = std::size_t{1} << 30U;
    config.terminalHistoryCapacity = 64;
    config.diagnosticsPerTask = 16;
    config.groupRegistryCapacity = 16;
    return config;
}

[[nodiscard]] inline core::RationalTime timeAt(const std::int64_t numerator,
                                               const std::int64_t denominator = 1) {
    const auto value = core::RationalTime::create(numerator, denominator);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

[[nodiscard]] inline document::CompositionFormat testFormat() {
    const auto rate = document::FrameRate::create(25, 1);
    if (!rate.has_value()) {
        std::abort();
    }
    const auto format =
        document::CompositionFormat::create(16, 12, core::PixelAspectRatio::square(), *rate);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
}

[[nodiscard]] inline document::NewProject makeTestProject(std::string projectName,
                                                          const core::RationalTime duration) {
    return document::makeNewProject(std::move(projectName), "Main", duration, testFormat());
}

// A rendezvous controlled by the test, keyed by the ordinal of the preparation call (counting from
// zero across the whole fixture). It both proves overlap (a call cannot return until a later call
// has started/finished) and can hold specific calls until the test releases them so a mid-run edit
// lands with both pipeline slots occupied.
class PrepCoordinator final {
  public:
    int arrive() {
        const int ordinal = calls_.fetch_add(1);
        const int now = active_.fetch_add(1) + 1;
        int observed = peak_.load();
        while (now > observed && !peak_.compare_exchange_weak(observed, now)) {
        }
        {
            std::lock_guard lock(mutex_);
            entered_.insert(ordinal);
            condition_.notify_all();
        }
        std::unique_lock lock(mutex_);
        if (barrierUntilArrival_ && ordinal == waitOrdinal_) {
            condition_.wait(lock, [&] { return entered_.count(untilOrdinal_) > 0; });
        }
        if (barrierUntilFinish_ && ordinal == waitOrdinal_) {
            condition_.wait(lock, [&] { return finished_.count(untilOrdinal_) > 0; });
        }
        if (holdOrdinals_.count(ordinal) > 0) {
            held_.insert(ordinal);
            condition_.notify_all();
            condition_.wait(lock, [&] { return released_; });
        }
        return ordinal;
    }

    void depart(const int ordinal) {
        {
            std::lock_guard lock(mutex_);
            finished_.insert(ordinal);
            condition_.notify_all();
        }
        active_.fetch_sub(1);
    }

    void armArrivalBarrier(const int waitOrdinal, const int untilOrdinal) {
        std::lock_guard lock(mutex_);
        barrierUntilArrival_ = true;
        waitOrdinal_ = waitOrdinal;
        untilOrdinal_ = untilOrdinal;
    }

    void armFinishBarrier(const int waitOrdinal, const int untilOrdinal) {
        std::lock_guard lock(mutex_);
        barrierUntilFinish_ = true;
        waitOrdinal_ = waitOrdinal;
        untilOrdinal_ = untilOrdinal;
    }

    void hold(const int ordinal) {
        std::lock_guard lock(mutex_);
        holdOrdinals_.insert(ordinal);
    }

    void releaseHeld() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

    [[nodiscard]] bool heldArrived(const std::size_t count) const {
        std::lock_guard lock(mutex_);
        return held_.size() >= count;
    }

    // Re-arms a coordinator between two independent runs of the same fixture.
    void resetForNextRun() {
        std::lock_guard lock(mutex_);
        entered_.clear();
        finished_.clear();
        held_.clear();
        holdOrdinals_.clear();
        barrierUntilArrival_ = false;
        barrierUntilFinish_ = false;
        released_ = false;
        waitOrdinal_ = -1;
        untilOrdinal_ = -1;
        calls_.store(0);
        active_.store(0);
        peak_.store(0);
    }

    [[nodiscard]] int calls() const noexcept { return calls_.load(); }
    [[nodiscard]] int peak() const noexcept { return peak_.load(); }

  private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::atomic<int> calls_{0};
    std::atomic<int> active_{0};
    std::atomic<int> peak_{0};
    std::set<int> entered_;
    std::set<int> finished_;
    std::set<int> held_;
    std::set<int> holdOrdinals_;
    bool barrierUntilArrival_ = false;
    bool barrierUntilFinish_ = false;
    bool released_ = false;
    int waitOrdinal_ = -1;
    int untilOrdinal_ = -1;
};

struct PipelineFixture final {
    runtime::NodeDefinitionRegistry definitions;
    runtime::SnapshotCompiler compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    CompiledPlanCacheHandle planCache = std::make_shared<CompiledPlanCache>();

    PipelineFixture() : compiler(definitions) {
        if (!runtime::registerBuiltInNodeDefinitions(definitions)) {
            std::abort();
        }
        definitions.freeze();
    }

    [[nodiscard]] PreviewPreparationFunction preparation(PrepCoordinator& coordinator) {
        auto pipeline = makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                       qualifiedProcessorProvider, planCache);
        return [pipeline, &coordinator](
                   const document::Snapshot& snapshot,
                   const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides,
                   runtime::TaskContext& context) {
            const int ordinal = coordinator.arrive();
            auto result = pipeline(snapshot, identity, limit, overrides, context);
            coordinator.depart(ordinal);
            return result;
        };
    }
};

struct SessionFixture final {
    document::Document document;
    commands::CommandStack commands;
    CompositionSession session;
    runtime::TaskScheduler scheduler;
    TaskUiBridge bridge;
    PipelineFixture pipelineFixture;
    PreviewFrameCacheHandle frameCache = std::make_shared<PreviewFrameCache>();
    PrepCoordinator coordinator;
    CompositionPreviewController controller;

    explicit SessionFixture(document::NewProject newProject)
        : document(std::move(newProject.project)), commands(document),
          session(document, commands, newProject.initialCompositionId),
          scheduler(twoWorkerConfig()), bridge(scheduler, nullptr, std::chrono::milliseconds{1}),
          controller(session, scheduler, bridge, pipelineFixture.preparation(coordinator), {},
                     frameCache) {}
};

inline void finishFixture(SessionFixture& fixture, Expectations& expectations) {
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the RAM pipeline fixture reaches asynchronous scheduler quiescence");
}

[[nodiscard]] inline bool isReady(const CompositionPreviewController& controller) {
    return controller.state().activity == PreviewActivity::Ready;
}

[[nodiscard]] inline bool animateSolidLayer(CompositionSession& session) {
    if (!session.addSolidLayer(QStringLiteral("Moving Solid"), core::Color4d{0.2, 0.4, 0.8, 1.0})) {
        return false;
    }
    if (session.currentTime() != timeAt(0) && !session.setCurrentTime(timeAt(0))) {
        return false;
    }
    if (!session.toggleKeyframe("position")) {
        return false;
    }
    if (!session.setCurrentTime(timeAt(23, 25))) {
        return false;
    }
    if (!session.setSelectedPosition(12.0, 9.0)) {
        return false;
    }
    return session.setCurrentTime(timeAt(0));
}

} // namespace bloom::ui::ram_pipeline_test
