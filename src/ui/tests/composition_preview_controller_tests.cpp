#include <bloom/commands/command_stack.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_editors.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using bloom::ui::SnapshotCompileResultHandle;

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

class WorkerGate final {
  public:
    void enterAndWait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    [[nodiscard]] bool entered() const {
        std::lock_guard lock(mutex_);
        return entered_;
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::invoke(predicate)) {
            return true;
        }
        std::this_thread::yield();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return std::invoke(predicate);
}

bloom::runtime::TaskSchedulerConfig testSchedulerConfig() {
    return {.cpuWorkerCount = 1,
            .blockingIoWorkerCount = 1,
            .cpuQueueCapacity = 16,
            .blockingIoQueueCapacity = 4,
            .terminalHistoryCapacity = 32,
            .diagnosticsPerTask = 8,
            .groupRegistryCapacity = 8};
}

SnapshotCompileResultHandle compiledResult(const bloom::document::Snapshot& snapshot,
                                           const bloom::document::CompositionId compositionId) {
    const auto* composition = snapshot.project().findComposition(compositionId);
    if (composition == nullptr) {
        return {};
    }
    auto plan = std::make_shared<bloom::runtime::CompiledCompositionPlan>(
        bloom::runtime::CompiledCompositionPlan{.sourceRevision = snapshot.revision(),
                                                .projectId = snapshot.project().id(),
                                                .compositionId = compositionId,
                                                .format = composition->format(),
                                                .operations = {},
                                                .output =
                                                    bloom::runtime::OperationIndex::fromRaw(0)});
    auto result = std::make_shared<bloom::runtime::SnapshotCompileResult>();
    result->status = bloom::runtime::SnapshotCompileStatus::Compiled;
    result->plan = std::move(plan);
    return result;
}

SnapshotCompileResultHandle unsupportedResult(const bloom::document::CompositionId compositionId) {
    auto result = std::make_shared<bloom::runtime::SnapshotCompileResult>();
    result->status = bloom::runtime::SnapshotCompileStatus::Unsupported;
    result->diagnostics.push_back({.code = bloom::runtime::CompileDiagnosticCode::UnsupportedNode,
                                   .severity = bloom::runtime::DiagnosticSeverity::Error,
                                   .subject = {.compositionId = compositionId,
                                               .nodeId = std::nullopt,
                                               .edgeId = std::nullopt,
                                               .parameterId = std::nullopt,
                                               .layerId = std::nullopt,
                                               .layerSlotId = std::nullopt,
                                               .field = {}},
                                   .summary = "The proof fixture is deliberately unsupported",
                                   .detail = {}});
    return result;
}

void reachQuiescence(bloom::ui::CompositionPreviewController& controller,
                     bloom::ui::TaskUiBridge& bridge, bloom::runtime::TaskScheduler& scheduler,
                     Expectations& expectations) {
    bool quiescentSignal = false;
    QObject::connect(&bridge, &bloom::ui::TaskUiBridge::shutdownQuiescent, &bridge,
                     [&quiescentSignal] { quiescentSignal = true; });
    controller.beginShutdown();
    bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return quiescentSignal && scheduler.isQuiescent(); }),
                        "preview fixture reaches asynchronous scheduler quiescence");
}

void testRevisionAndPanelSuppression(Expectations& expectations) {
    using namespace bloom;
    auto newProject =
        document::makeNewProject("Preview Test", "Main", core::RationalTime::fromInteger(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    WorkerGate firstRequest;
    std::atomic<int> invocationCount = 0;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&firstRequest, &invocationCount](const document::Snapshot& snapshot,
                                          const document::CompositionId requestedComposition,
                                          runtime::TaskContext&) {
            if (invocationCount.fetch_add(1) == 0) {
                firstRequest.enterAndWait();
            }
            return runtime::TaskResult<SnapshotCompileResultHandle>::succeeded(
                compiledResult(snapshot, requestedComposition));
        });
    auto* viewer = new ui::ViewerEditor(session, controller);

    expectations.expect(waitUntil([&] { return firstRequest.entered(); }),
                        "first revision preparation starts on the worker");
    const auto oldGeneration = controller.state().requestGeneration;
    std::vector<std::uint64_t> publishedReadyGenerations;
    QObject::connect(
        &controller, &ui::CompositionPreviewController::stateChanged, &controller, [&] {
            if (controller.state().status == ui::CompositionPreviewStatus::Ready) {
                publishedReadyGenerations.push_back(controller.state().requestGeneration);
            }
        });

    expectations.expect(
        session.addSolidLayer(QStringLiteral("Revision Solid"), core::Color4d{0.2, 0.4, 0.8, 1.0}),
        "document command creates a newer preview revision");
    const auto currentGeneration = controller.state().requestGeneration;
    expectations.expect(currentGeneration > oldGeneration &&
                            controller.state().status == ui::CompositionPreviewStatus::Preparing,
                        "new revision immediately supersedes the old preparation generation");

    delete viewer;
    firstRequest.release();
    expectations.expect(
        waitUntil([&] { return controller.state().status == ui::CompositionPreviewStatus::Ready; }),
        "current revision becomes ready after the replaced Viewer is destroyed");
    expectations.expect(controller.state().sourceRevision == session.snapshot().revision(),
                        "published plan identifies the active document revision");
    expectations.expect(!publishedReadyGenerations.empty() &&
                            std::ranges::all_of(publishedReadyGenerations,
                                                [currentGeneration](const auto generation) {
                                                    return generation == currentGeneration;
                                                }),
                        "the completed obsolete revision never publishes Ready");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

void testSameRevisionGenerationSuppression(Expectations& expectations) {
    using namespace bloom;
    auto newProject =
        document::makeNewProject("Generation Test", "Main", core::RationalTime::fromInteger(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    WorkerGate firstRequest;
    std::atomic<int> invocationCount = 0;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&firstRequest, &invocationCount](const document::Snapshot& snapshot,
                                          const document::CompositionId requestedComposition,
                                          runtime::TaskContext&) {
            if (invocationCount.fetch_add(1) == 0) {
                firstRequest.enterAndWait();
            }
            return runtime::TaskResult<SnapshotCompileResultHandle>::succeeded(
                compiledResult(snapshot, requestedComposition));
        });

    expectations.expect(waitUntil([&] { return firstRequest.entered(); }),
                        "first same-revision preparation starts");
    const auto revision = controller.state().sourceRevision;
    const auto firstGeneration = controller.state().requestGeneration;
    controller.requestRefresh();
    const auto secondGeneration = controller.state().requestGeneration;
    expectations.expect(controller.state().sourceRevision == revision &&
                            secondGeneration > firstGeneration,
                        "explicit refresh advances generation without inventing a revision");
    firstRequest.release();
    expectations.expect(
        waitUntil([&] { return controller.state().status == ui::CompositionPreviewStatus::Ready; }),
        "replacement generation completes");
    expectations.expect(controller.state().requestGeneration == secondGeneration,
                        "only the desired same-revision generation publishes");

    const auto callsBeforeSelection = invocationCount.load();
    const auto generationBeforeSelection = controller.state().requestGeneration;
    session.clearSelection();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    expectations.expect(invocationCount.load() == callsBeforeSelection &&
                            controller.state().requestGeneration == generationBeforeSelection,
                        "selection-only changes never prepare a composition plan");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

void testOutcomeMapping(Expectations& expectations) {
    using namespace bloom;
    enum class Outcome { Compiled, Unsupported, Cancelled, Failed };

    auto newProject =
        document::makeNewProject("Outcome Test", "Main", core::RationalTime::fromInteger(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    std::atomic outcome = Outcome::Compiled;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&outcome](const document::Snapshot& snapshot,
                   const document::CompositionId requestedComposition, runtime::TaskContext&) {
            switch (outcome.load()) {
            case Outcome::Compiled:
                return runtime::TaskResult<SnapshotCompileResultHandle>::succeeded(
                    compiledResult(snapshot, requestedComposition));
            case Outcome::Unsupported:
                return runtime::TaskResult<SnapshotCompileResultHandle>::succeeded(
                    unsupportedResult(requestedComposition));
            case Outcome::Cancelled:
                return runtime::TaskResult<SnapshotCompileResultHandle>::cancelled();
            case Outcome::Failed:
                return runtime::TaskResult<SnapshotCompileResultHandle>::failed(
                    std::vector<runtime::TaskDiagnostic>{
                        {.code = "bloom.preview.test-failure",
                         .severity = runtime::DiagnosticSeverity::Error,
                         .summary = "The proof preparation failed",
                         .detail = {},
                         .suggestedAction = {}},
                        {.code = "bloom.preview.secondary-test-failure",
                         .severity = runtime::DiagnosticSeverity::Warning,
                         .summary = "Secondary diagnostic",
                         .detail = {},
                         .suggestedAction = {}}});
            }
            return runtime::TaskResult<SnapshotCompileResultHandle>::failed(
                {.code = "bloom.preview.invalid-test-outcome",
                 .severity = runtime::DiagnosticSeverity::Error,
                 .summary = "Invalid proof outcome",
                 .detail = {},
                 .suggestedAction = {}});
        });

    expectations.expect(
        waitUntil([&] { return controller.state().status == ui::CompositionPreviewStatus::Ready; }),
        "compiled preparation maps to Ready");
    outcome.store(Outcome::Unsupported);
    controller.requestRefresh();
    expectations.expect(waitUntil([&] {
                            return controller.state().status ==
                                   ui::CompositionPreviewStatus::Unsupported;
                        }),
                        "semantic compiler rejection maps to Unsupported");
    expectations.expect(controller.state().message.contains(QStringLiteral("deliberately")),
                        "Unsupported preserves the compiler's useful summary");
    outcome.store(Outcome::Failed);
    controller.requestRefresh();
    expectations.expect(waitUntil([&] {
                            return controller.state().status ==
                                   ui::CompositionPreviewStatus::Failed;
                        }),
                        "failed task maps to Failed");
    expectations.expect(
        controller.state().message == QStringLiteral("The proof preparation failed") &&
            controller.state().diagnostics.size() == 2 &&
            controller.state().diagnostics[0].code == "bloom.preview.test-failure" &&
            controller.state().diagnostics[1].code == "bloom.preview.secondary-test-failure",
        "Failed preserves every task diagnostic in source order");
    outcome.store(Outcome::Cancelled);
    controller.requestRefresh();
    expectations.expect(waitUntil([&] {
                            return controller.state().status ==
                                   ui::CompositionPreviewStatus::Cancelled;
                        }),
                        "cancelled task maps to Cancelled");

    reachQuiescence(controller, bridge, scheduler, expectations);
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testRevisionAndPanelSuppression(expectations);
    testSameRevisionGenerationSuppression(expectations);
    testOutcomeMapping(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
