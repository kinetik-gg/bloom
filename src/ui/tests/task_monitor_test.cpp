#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/jobs_editor.hpp>
#include <bloom/ui/task_monitor_model.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableView>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "Failure: " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

template <typename Predicate>
bool spinUntil(Predicate&& predicate, const std::chrono::milliseconds timeout = 3s) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout.count()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::yield();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

bloom::runtime::TaskOwner owner(const std::uint64_t value) {
    return {.kind = bloom::runtime::TaskOwnerKind::Composition,
            .id = bloom::runtime::TaskOwnerId::fromRaw(value)};
}

struct CooperativeWork final {
    std::atomic<bool> entered = false;
    std::atomic<bool> release = false;
    std::atomic<std::uint64_t> progressReports = 0;

    bloom::runtime::TaskResult<void> run(bloom::runtime::TaskContext& context,
                                         const bool diagnostic) {
        entered.store(true, std::memory_order_release);
        if (diagnostic) {
            context.addDiagnostic({.code = "bloom.test.visible-diagnostic",
                                   .severity = bloom::runtime::DiagnosticSeverity::Warning,
                                   .summary = "<b>Plain diagnostic text</b>",
                                   .detail = "The full diagnostic detail remains inspectable.",
                                   .suggestedAction = "Cancel or let the test finish."});
        }

        std::uint64_t completed = 0;
        while (!release.load(std::memory_order_acquire) && !context.isCancellationRequested()) {
            completed = (completed + 1) % 101;
            context.reportProgress({.phase = "Analyzing",
                                    .subphase = "Visible fake work",
                                    .completed = completed,
                                    .total = 100});
            progressReports.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(1ms);
        }
        return context.isCancellationRequested() ? bloom::runtime::TaskResult<void>::cancelled()
                                                 : bloom::runtime::TaskResult<void>::succeeded();
    }
};

void testBridgeModelAndEditor(Expectations& expectations, QApplication& application) {
    namespace runtime = bloom::runtime;
    namespace ui = bloom::ui;

    runtime::TaskSchedulerConfig config;
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    config.cpuQueueCapacity = 8;
    config.blockingIoQueueCapacity = 4;
    config.terminalHistoryCapacity = 8;
    config.diagnosticsPerTask = 8;
    config.groupRegistryCapacity = 4;
    runtime::TaskScheduler scheduler(config);
    ui::TaskUiBridge bridge(scheduler, nullptr, 50ms);
    ui::TaskMonitorModel model(bridge);

    int pollCount = 0;
    int resetCount = 0;
    int dataChangeCount = 0;
    bool allPulsesOnUiThread = true;
    QObject::connect(&bridge, &ui::TaskUiBridge::snapshotsPolled, &application, [&] {
        ++pollCount;
        allPulsesOnUiThread =
            allPulsesOnUiThread && QThread::currentThread() == application.thread();
    });
    QObject::connect(&model, &QAbstractItemModel::modelReset, &application, [&] { ++resetCount; });
    QObject::connect(
        &model, &QAbstractItemModel::dataChanged, &application,
        [&](const QModelIndex&, const QModelIndex&, const QList<int>&) { ++dataChangeCount; });
    bridge.start();

    CooperativeWork firstWork;
    runtime::TaskRequest firstRequest("Long analysis", owner(7), runtime::TaskPriority::Foreground);
    firstRequest.sourceVersion = {.documentRevision = 9, .requestGeneration = 3};
    auto first = scheduler.submit<void>(
        std::move(firstRequest),
        [&firstWork](runtime::TaskContext& context) { return firstWork.run(context, true); });
    expectations.expect(first.accepted(), "the fake observable task is accepted");
    bridge.wake();
    expectations.expect(spinUntil([&] {
                            return firstWork.entered.load(std::memory_order_acquire) &&
                                   model.rowCount() == 1;
                        }),
                        "the bridge publishes a running task into the model");

    const QModelIndex firstIndex = model.indexForTask(first.handle.id());
    expectations.expect(firstIndex.isValid(), "the model resolves a task by stable TaskId");
    expectations.expect(
        model.columnCount() == ui::TaskMonitorModel::ColumnCount &&
            model.headerData(ui::TaskMonitorModel::JobColumn, Qt::Horizontal).toString() ==
                QStringLiteral("Job") &&
            model.headerData(ui::TaskMonitorModel::PhaseColumn, Qt::Horizontal).toString() ==
                QStringLiteral("Phase") &&
            model.headerData(ui::TaskMonitorModel::ProgressColumn, Qt::Horizontal).toString() ==
                QStringLiteral("Progress") &&
            model.headerData(ui::TaskMonitorModel::PriorityColumn, Qt::Horizontal).toString() ==
                QStringLiteral("Priority") &&
            model.headerData(ui::TaskMonitorModel::StateColumn, Qt::Horizontal).toString() ==
                QStringLiteral("State") &&
            model.headerData(ui::TaskMonitorModel::DurationColumn, Qt::Horizontal).toString() ==
                QStringLiteral("Duration"),
        "the monitor exposes the six exact Jobs columns");
    expectations.expect(
        model.data(firstIndex, ui::TaskMonitorModel::TaskIdRole).toULongLong() ==
                first.handle.id().value() &&
            model.data(firstIndex, ui::TaskMonitorModel::ProgressTotalRole).toULongLong() == 100 &&
            model.data(firstIndex, ui::TaskMonitorModel::CanCancelRole).toBool(),
        "stable identity, structured progress, and cancellation roles are available");

    const int pollsBeforeProgressBurst = pollCount;
    const int resetsBeforeProgressBurst = resetCount;
    const int changesBeforeProgressBurst = dataChangeCount;
    const auto reportsBeforeProgressBurst =
        firstWork.progressReports.load(std::memory_order_relaxed);
    QElapsedTimer burstTimer;
    burstTimer.start();
    while (burstTimer.elapsed() < 180) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::yield();
    }
    const int deliveredPolls = pollCount - pollsBeforeProgressBurst;
    const auto reportedProgress =
        firstWork.progressReports.load(std::memory_order_relaxed) - reportsBeforeProgressBurst;
    expectations.expect(deliveredPolls > 0 && deliveredPolls <= 5 &&
                            reportedProgress > static_cast<std::uint64_t>(deliveredPolls),
                        "rapid worker progress is coalesced by the fixed-rate UI poll");
    expectations.expect(resetCount == resetsBeforeProgressBurst &&
                            dataChangeCount > changesBeforeProgressBurst,
                        "same-identity progress updates in place without resetting rows");
    expectations.expect(allPulsesOnUiThread,
                        "every bridge pulse and model update is delivered on the UI thread");

    ui::EditorRegistry registry;
    expectations.expect(ui::registerJobsEditor(registry, model) && registry.editors().size() == 1 &&
                            registry.editors().front().id == "bloom.jobs" &&
                            !ui::registerJobsEditor(registry, model),
                        "bloom.jobs registers once as a replaceable editor");
    std::unique_ptr<QWidget> editor(registry.editors().front().create(nullptr));
    auto* table = editor->findChild<QTableView*>("jobsTable");
    auto* cancel = editor->findChild<QPushButton*>("cancelSelectedJobButton");
    auto* details = editor->findChild<QPlainTextEdit*>("jobDetails");
    expectations.expect(
        table != nullptr && cancel != nullptr && details != nullptr &&
            !editor->accessibleName().isEmpty() && !table->accessibleName().isEmpty() &&
            !cancel->accessibleName().isEmpty() && !details->accessibleName().isEmpty(),
        "Jobs controls expose stable object names and accessible text");
    if (table != nullptr) {
        table->selectRow(firstIndex.row());
        QCoreApplication::processEvents();
    }
    expectations.expect(
        details != nullptr && details->toPlainText().contains("Long analysis") &&
            details->toPlainText().contains("bloom.test.visible-diagnostic") &&
            details->toPlainText().contains("<b>Plain diagnostic text</b>") &&
            details->toPlainText().contains("Suggested action"),
        "the read-only details surface preserves complete diagnostics as plain text");

    if (cancel != nullptr) {
        cancel->click();
    }
    expectations.expect(cancel != nullptr && !cancel->isEnabled(),
                        "the stable-ID cancel action disables immediately after acceptance");
    expectations.expect(
        spinUntil([&] {
            const auto terminalIndex = model.indexForTask(first.handle.id());
            return terminalIndex.isValid() &&
                   model.data(terminalIndex, ui::TaskMonitorModel::TaskStateRole).toInt() ==
                       static_cast<int>(runtime::TaskState::Cancelled);
        }),
        "cooperative cancellation reaches an authoritative Cancelled snapshot");

    CooperativeWork secondWork;
    auto second = scheduler.submit<void>(
        runtime::TaskRequest("Disposable panel work", owner(8), runtime::TaskPriority::Visible),
        [&secondWork](runtime::TaskContext& context) { return secondWork.run(context, false); });
    expectations.expect(second.accepted(), "a second slow task is accepted");
    bridge.wake();
    expectations.expect(spinUntil([&] {
                            return secondWork.entered.load(std::memory_order_acquire) &&
                                   model.indexForTask(second.handle.id()).isValid();
                        }),
                        "the newest task becomes observable");
    expectations.expect(model.index(0, ui::TaskMonitorModel::JobColumn)
                                .data(ui::TaskMonitorModel::TaskIdRole)
                                .toULongLong() == second.handle.id().value(),
                        "rows are deterministically ordered newest TaskId first");

    editor.reset();
    const QModelIndex secondIndex = model.indexForTask(second.handle.id());
    expectations.expect(model.requestCancellation(secondIndex),
                        "panel destruction does not own or disable task cancellation");
    expectations.expect(
        spinUntil([&] {
            const auto terminalIndex = model.indexForTask(second.handle.id());
            return terminalIndex.isValid() &&
                   model.data(terminalIndex, ui::TaskMonitorModel::TaskStateRole).toInt() ==
                       static_cast<int>(runtime::TaskState::Cancelled);
        }),
        "work finishes safely after its Jobs widget is destroyed");

    editor.reset(registry.editors().front().create(nullptr));
    details = editor->findChild<QPlainTextEdit*>("jobDetails");
    expectations.expect(details != nullptr &&
                            details->toPlainText().contains("Disposable panel work") &&
                            details->toPlainText().contains("Cancelled"),
                        "a recreated Jobs editor sees retained terminal history");

    int signalSequence = 0;
    int finalPollSequence = 0;
    int quiescentSequence = 0;
    int quiescentCount = 0;
    bool finalSnapshotsAreTerminal = false;
    QObject::connect(&bridge, &ui::TaskUiBridge::snapshotsPolled, &application,
                     [&] { finalPollSequence = ++signalSequence; });
    QObject::connect(&bridge, &ui::TaskUiBridge::shutdownQuiescent, &application, [&] {
        quiescentSequence = ++signalSequence;
        ++quiescentCount;
        finalSnapshotsAreTerminal =
            std::ranges::all_of(bridge.snapshots(), [](const auto& snapshot) {
                return runtime::isTerminal(snapshot.state);
            });
    });
    bridge.beginShutdown();
    expectations.expect(spinUntil([&] { return quiescentCount == 1; }),
                        "shutdown remains event-driven until the scheduler is quiescent");
    expectations.expect(finalPollSequence > 0 && quiescentSequence > finalPollSequence &&
                            finalSnapshotsAreTerminal && !bridge.isRunning(),
                        "shutdown quiescence follows a stable terminal UI snapshot poll");
    bridge.stop();
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    Expectations expectations;
    testBridgeModelAndEditor(expectations, application);
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " task monitor expectation(s) failed\n";
        return 1;
    }
    return 0;
}
