// The Export panel (ADR 0033) from hand-built snapshots: the adapter's single drain, the sounds on
// success and failure, the rows and their status wording for every state, a calm Waiting for the
// Viewer, typed availability reasons on every control, the replace confirmation, the Linux-only
// note, an absent size estimate, and one command per gesture.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#include "asset_drop.hpp"
#include "export_job_results.hpp"
#include "export_job_row.hpp"
#include "export_output_row.hpp"
#include "export_panel_text.hpp"
#include "export_test_support.hpp"
#include "window_fixture.hpp"

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/export_editor.hpp>
#include <bloom/ui/export_queue_edits.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/progress_bar.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/task_monitor_model.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/ui_feedback.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QImage>
#include <QTemporaryDir>
#include <QDropEvent>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <atomic>
#include <cstdio>
#include <thread>

namespace {
using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::test;
namespace ep = bloom::ui::exportpanel;

struct TestContext {
    bool ok = true;
    void expect(const bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ok = false;
        }
    }
};

class FakePlayer final : public SoundPlayer {
  public:
    void play(const FeedbackSound sound, float) noexcept override { played.push_back(sound); }
    void shutdown() noexcept override {}
    std::vector<FeedbackSound> played;
};
class FakeNotifier final : public Notifier {
  public:
    void post(const SystemNotification& notification) override { posted.push_back(notification); }
    void setActivationHandler(std::function<void()>) override {}
    std::vector<SystemNotification> posted;
};

struct Rig {
    document::NewProject seed = document::makeNewProject("Export Test", "Main",
                                                         core::RationalTime::fromInteger(10));
    document::Document doc{std::move(seed.project)};
    commands::CommandStack stack{doc};
    CompositionSession session{doc, stack, seed.initialCompositionId};
    FakeExportPort port;
    FakePlayer* player = nullptr;
    FakeNotifier* notifier = nullptr;
    std::unique_ptr<UiFeedback> feedback;
    std::unique_ptr<ExportQueueAdapter> adapter;
    EditorRegistry registry;
    std::unique_ptr<EditorArea> area;
    ExportEditor* editor = nullptr;
    bool active = true;

    explicit Rig(const bool withEditor = true) {
        auto fakePlayer = std::make_unique<FakePlayer>();
        auto fakeNotifier = std::make_unique<FakeNotifier>();
        player = fakePlayer.get();
        notifier = fakeNotifier.get();
        feedback = std::make_unique<UiFeedback>(std::move(fakePlayer), std::move(fakeNotifier),
                                                [this] { return active; });
        adapter = std::make_unique<ExportQueueAdapter>(port, feedback.get());
        if (withEditor)
            open();
    }
    void open() {
        (void)registerExportEditor(registry, session, adapter.get());
        area = std::make_unique<EditorArea>(registry, "bloom.export", QString{});
        area->resize(900, 760);
        area->show();
        QApplication::processEvents();
        editor = area->findChild<ExportEditor*>();
    }
    [[nodiscard]] document::CompositionId composition() const { return session.compositionId(); }
    template <typename T> T* find(const char* name) const {
        return area->findChild<T*>(QString::fromLatin1(name));
    }
    [[nodiscard]] ep::JobRow* row(const std::uint64_t id) const {
        return editor != nullptr ? editor->jobRow(document::ExportJobId::fromRaw(id)) : nullptr;
    }
    [[nodiscard]] ep::OutputRow* output(const std::uint64_t job, const std::uint64_t output) const {
        const auto* shown = row(job);
        return shown != nullptr ? shown->outputRow(document::ExportOutputId::fromRaw(output)) : nullptr;
    }
    // Opens a job and one of its outputs, and the output's Quality group.
    ep::OutputRow* open(const std::uint64_t job, const std::uint64_t output) const {
        if (auto* shown = row(job))
            shown->setExpanded(true);
        auto* out = this->output(job, output);
        if (out != nullptr) {
            out->setExpanded(true);
            out->qualitySection()->setCollapsed(false);
        }
        QApplication::processEvents();
        return out;
    }
    // The texts the artist can read right now.
    [[nodiscard]] QStringList visibleTexts() const {
        QStringList texts;
        for (auto* label : area->findChildren<QLabel*>())
            if (label->isVisibleTo(area.get()) && !label->text().isEmpty())
                texts.push_back(label->text());
        for (auto* button : area->findChildren<kit::KButton*>())
            if (button->isVisibleTo(area.get()))
                texts.push_back(button->text());
        return texts;
    }
    [[nodiscard]] int visiblePrimaries() const {
        int count = 0;
        for (auto* button : area->findChildren<kit::KButton*>())
            count += button->isVisibleTo(area.get()) && button->variant() == kit::KButton::Variant::Primary ? 1 : 0;
        return count;
    }
    [[nodiscard]] bool shown(const char* name) const {
        auto* widget = area->findChild<QWidget*>(QString::fromLatin1(name));
        return widget != nullptr && widget->isVisibleTo(area.get());
    }
    // Publishes a snapshot and lets the adapter see it, as the completion drain would.
    void publish(const std::function<void(host::ExportQueueSnapshot&)>& change) {
        port.publish(change);
        adapter->drain();
        QApplication::processEvents();
    }
    void threeJobs() {
        publish([this](host::ExportQueueSnapshot& snapshot) { fillThreeJobs(snapshot, composition()); });
    }
};
void adapterTests(TestContext& context) {
    Rig rig(false);
    int changes = 0;
    QObject::connect(rig.adapter.get(), &ExportQueueAdapter::queueChanged, [&] { ++changes; });
    rig.port.onPump = [](FakeExportPort& port) {
        if (port.pumps == 1)
            port.publish([](host::ExportQueueSnapshot&) {});
    };
    rig.adapter->drain();
    context.expect(rig.port.pumps == 1, "a drain pumps exactly once");
    context.expect(changes == 1, "a moved revision emits queueChanged once");
    rig.adapter->drain();
    context.expect(rig.port.pumps == 2 && changes == 1, "an unmoved revision emits nothing");

    // Sounds on terminal events: success and failure announce, a drained queue is silent.
    rig.port.raise(host::ExportJobSucceeded{.job = document::ExportJobId::fromRaw(1)});
    rig.adapter->drain();
    context.expect(rig.player->played.size() == 1 &&
                       rig.player->played[0] == FeedbackSound::RenderSucceeded,
                   "a succeeded job plays the success sound");
    rig.active = false;
    rig.port.raise(host::ExportJobFailed{.job = document::ExportJobId::fromRaw(2),
                                         .reason = {.reason = "Disk full"}});
    QTest::qWait(120);
    rig.adapter->drain();
    context.expect(rig.player->played.size() == 2 &&
                       rig.player->played[1] == FeedbackSound::RenderFailed,
                   "a failed job plays the failure sound");
    context.expect(rig.notifier->posted.size() == 1 && rig.notifier->posted[0].failure,
                   "a failure while Bloom is inactive posts a system notification");
    rig.port.raise(host::ExportQueueDrained{.succeeded = 1, .failed = 1});
    QTest::qWait(120);
    rig.adapter->drain();
    context.expect(rig.player->played.size() == 2, "a drained queue makes no second sound");
    // The same events are never announced twice.
    rig.adapter->drain();
    context.expect(rig.player->played.size() == 2, "an event is announced once");
}


void jobsLink(TestContext& context) {
    Rig rig(false);
    rig.threeJobs();
    runtime::TaskScheduler scheduler;
    TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds{5});
    TaskMonitorModel model(bridge);
    model.setExportQueue(rig.adapter.get());
    bridge.start();
    // One export run of three tasks under one group, and one ordinary task.
    const runtime::TaskOwner exportOwner{.kind = runtime::TaskOwnerKind::Export,
                                         .id = runtime::TaskOwnerId::fromRaw(7)};
    rig.port.taskOwners[7] = 1;
    auto group = scheduler.createGroup(exportOwner, "Export run");
    std::atomic<bool> release = false;
    std::vector<runtime::TaskHandle<void>> handles;
    for (const char* name : {"Render frames", "Encode", "Verify"}) {
        runtime::TaskRequest request(name, exportOwner, runtime::TaskPriority::Foreground);
        request.groupId = group.handle.id();
        auto submitted = scheduler.submit<void>(std::move(request), [&release](runtime::TaskContext& c) {
            while (!release.load() && !c.isCancellationRequested())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return runtime::TaskResult<void>::succeeded();
        });
        context.expect(submitted.accepted(), "the export tasks are accepted");
        handles.push_back(std::move(submitted.handle));
    }
    bridge.wake();
    QElapsedTimer timer;
    timer.start();
    while (model.rowCount() != 1 && timer.elapsed() < 3000)
        QTest::qWait(10);
    context.expect(model.rowCount() == 1, "one export run is one row, however many tasks it has");
    if (model.rowCount() == 1) {
        const auto index = model.index(0, TaskMonitorModel::JobColumn);
        context.expect(model.data(index).toString() == "Export: Shot 010",
                       "the row is named after the job");
        context.expect(model.data(index, TaskMonitorModel::ExportJobRole).toULongLong() == 1,
                       "the row knows its export job");
        context.expect(!model.data(index, TaskMonitorModel::CanCancelRole).toBool(),
                       "an export row has no cancel");
        context.expect(model.data(index, TaskMonitorModel::ProgressTotalRole).toULongLong() == 240 &&
                           model.data(index, TaskMonitorModel::ProgressCompletedRole).toULongLong() == 120,
                       "progress comes from the queue snapshot");
    }
    release = true;
    bridge.beginShutdown();
    timer.restart();
    while (!scheduler.isQuiescent() && timer.elapsed() < 5000)
        QTest::qWait(10);
}


void addingRevealsAndSelects(TestContext& context) {
    Rig rig(false);
    int reveals = 0;
    qulonglong added = 0;
    QObject::connect(rig.adapter.get(), &ExportQueueAdapter::revealRequested, [&] { ++reveals; });
    QObject::connect(rig.adapter.get(), &ExportQueueAdapter::jobAdded,
                     [&](const qulonglong job) { added = job; });
    const auto result = rig.adapter->addCompositionToQueue(rig.composition());
    context.expect(result.accepted && reveals == 1 && added == 100,
                   "adding a job reveals the panel and announces the new job");
    // A panel opened afterwards still selects it.
    rig.open();
    context.expect(rig.editor->selectedJob() &&
                       rig.editor->selectedJob()->value() == 100,
                   "a panel opened after the add selects the new job");
    // A refused add reveals nothing.
    rig.port.nextEdit = {false, "The queue is full.", std::nullopt, std::nullopt, {}};
    const auto refused = rig.adapter->addCompositionToQueue(rig.composition());
    context.expect(!refused.accepted && reveals == 1, "a refused add does not reveal the panel");
}

void menusAndAssets(TestContext& context) {
    FakeExportPort* portRef = nullptr;
    std::unique_ptr<FakeExportPort> port = std::make_unique<FakeExportPort>();
    portRef = port.get();
    ExportQueueAdapter adapter(*port);
    WindowFixture fixture([&](WindowFixture& f) { f.exportQueue = &adapter; });
    auto* action = fixture.window->addToExportQueueAction();
    context.expect(action != nullptr && action->isEnabled(),
                   "File > Add to Export Queue is enabled with a composition");
    context.expect(action->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_M), "its shortcut is Ctrl+M");
    action->trigger();
    context.expect(!portRef->calls.empty() && portRef->calls.back().starts_with("addJobs "),
                   "the menu item adds a job");
    QApplication::processEvents();
    auto* host = fixture.window->workspaceHost();
    bool shown = false;
    for (auto* area : host->findChildren<EditorArea*>())
        shown = shown || area->editorId() == "bloom.export";
    context.expect(shown, "adding a job from the menu shows the Export panel");
    // The Assets composition row offers it too.
    auto* assets = fixture.window->findChild<AssetsEditor*>();
    context.expect(assets != nullptr, "the window has an Assets editor");
    if (assets != nullptr) {
        auto* tree = assets->findChild<QTreeWidget*>(QStringLiteral("assetsTree"));
        QTreeWidgetItem* row = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0, 0x0100 + 1).isValid() && (*it)->data(0, Qt::UserRole + 1).toULongLong() != 0)
                row = *it;
        if (row != nullptr) {
            tree->setCurrentItem(row);
            const auto before = portRef->calls.size();
            QStringList names;
            QTimer::singleShot(0, tree, [&] {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (menu == nullptr)
                    return;
                for (auto* a : menu->actions())
                    if (a->objectName() == "assetsAddToExportQueueAction") {
                        names.push_back(a->objectName());
                        a->trigger();
                    }
                menu->close();
            });
            emit tree->customContextMenuRequested(tree->visualItemRect(row).center());
            QApplication::processEvents();
            context.expect(!names.isEmpty() && portRef->calls.size() == before + 1,
                           "the Assets composition row adds a job to the queue");
        } else {
            context.expect(false, "a composition row exists in Assets");
        }
    }
}


void editsAreUndoable(TestContext& context) {
    Rig rig(false);
    ExportQueueEdits edits(
        rig.session,
        [] {
            host::ExportDefaultPreset preset;
            preset.presetId = "builtin:h264-mp4";
            preset.destination = host::defaultExportDestination(preset.settings.format);
            return preset;
        },
        [](const std::string& id) { return std::optional(ExportPresetChoice{id, {}}); });
    const auto jobs = [&] { return rig.session.snapshot().project().exportQueue().jobs; };
    context.expect(edits.addJob(rig.composition()).accepted && jobs().size() == 1,
                   "Add to Export Queue adds a job to the project");
    context.expect(!jobs()[0].outputs.empty() && jobs()[0].outputs[0].id.isValid(),
                   "the new job has an output with an allocated id");
    const auto job = jobs()[0].id;
    const auto output = jobs()[0].outputs[0].id;
    context.expect(edits.editJob(job, jobEdit([](auto& edit) {
                           edit.name = std::string("Hero");
                           edit.enabled = false;
                       })).accepted,
                   "editing a job's name and enabled flag is accepted");
    context.expect(jobs()[0].name == "Hero" && !jobs()[0].enabled, "the edit is in the project");
    (void)rig.session.undo();
    context.expect(jobs()[0].name.empty() && jobs()[0].enabled,
                   "one undo reverts every member of one edit");
    context.expect(edits.editOutput(output, outputEdit([](auto& edit) { edit.importIntoAssets = true; })).accepted &&
                       jobs()[0].outputs[0].importIntoAssets,
                   "Import into Assets is project truth");
    (void)rig.session.undo();
    context.expect(!jobs()[0].outputs[0].importIntoAssets, "and undoable");
    (void)rig.session.undo();
    context.expect(jobs().empty(), "undo removes the added job");
    (void)rig.session.redo();
    context.expect(jobs().size() == 1 && jobs()[0].id == job, "redo restores the same job id");
    context.expect(!edits.removeOutput(output).accepted, "a job keeps at least one output");
    // Adding an output in a chosen format is one command, so one undo removes it whole.
    ExportQueueEdits formats(
        rig.session,
        [] {
            host::ExportDefaultPreset preset;
            preset.presetId = "builtin:h264-mp4";
            preset.destination = host::defaultExportDestination(preset.settings.format);
            return preset;
        },
        [](const std::string& id) {
            ExportPresetChoice choice{id, {}};
            choice.settings.format = document::ExportFormat::Png;
            return std::optional(choice);
        });
    const auto added = formats.addOutput(job, "builtin:png-sequence");
    context.expect(added.accepted && added.addedOutput.has_value() && jobs()[0].outputs.size() == 2,
                   "Add output in a format adds an output and reports its id");
    context.expect(jobs()[0].outputs.size() == 2 &&
                       jobs()[0].outputs[1].presetId == "builtin:png-sequence" &&
                       jobs()[0].outputs[1].settings.format == document::ExportFormat::Png,
                   "the added output has the chosen format");
    (void)rig.session.undo();
    context.expect(jobs()[0].outputs.size() == 1, "one undo removes the output added with a format");
}



// ---------------------------------------------------------------------------------------------
// The job list

host::ExportJobView jobIn(Rig& rig, const std::uint64_t id, const host::ExportJobState state,
                          const std::uint64_t output = 0) {
    auto job = makeJob(id, "Shot " + std::to_string(id), rig.composition(),
                       {makeOutput(output != 0 ? output : id * 10 + 1)});
    job.state = state;
    return job;
}

void listStructure(TestContext& context) {
    Rig rig;
    context.expect(rig.editor != nullptr, "the Export editor is created by the registry");
    // One list: no splitter, no detail pane, no separate progress strip, no table.
    context.expect(rig.area->findChild<QSplitter*>() == nullptr, "there is no splitter");
    context.expect(rig.area->findChild<QWidget*>("exportDetailPane") == nullptr &&
                       rig.area->findChild<QWidget*>("exportRunStrip") == nullptr &&
                       rig.area->findChild<QTreeWidget*>() == nullptr,
                   "there is no detail pane, run strip or tree");
    rig.threeJobs();
    context.expect(rig.editor->jobRows().size() == 3, "three jobs make three rows");
    auto* first = rig.row(1);
    context.expect(first != nullptr && first->nameText() == "Shot 010", "the name shows");
    context.expect(first->compositionText() == "Main", "the composition shows");
    context.expect(first->rangeText().contains("Work area") && first->rangeText().contains("0–239"),
                   "the range chip reads the range");
    context.expect(first->statusText() == "Rendering 50%", "a rendering job reads its percent");
    context.expect(rig.row(2)->statusText() == "Up next", "a queued job reads Up next");
    context.expect(rig.row(3)->statusText() == "Failed", "a failed job reads Failed");
    context.expect(rig.row(3)->findChild<QLabel*>("exportJobStatus")->toolTip().contains("verification"),
                   "a failure reads as a plain sentence in its status tooltip");

    // New jobs open; their outputs stay folded until asked.
    context.expect(first->expanded() && first->outputRows().size() == 1 &&
                       !first->outputRows()[0]->expanded(),
                   "a job opens with its output folded");
    auto* output = first->outputRows()[0];
    context.expect(output->destinationSummary().contains("out.mp4") &&
                       output->statusText() == "Writing 50%",
                   "a folded output shows its format, where it saves and its state");
    context.expect(!rig.shown("exportOutputBody"), "a folded output hides its settings");
    QTest::mouseClick(output->findChild<QWidget*>("exportOutputDisclosure"), Qt::LeftButton);
    context.expect(output->expanded() && rig.shown("exportOutputBody"),
                   "the caret opens an output's settings");
    context.expect(output->formatText() == "H.264 MP4", "an open output names its format");
    // Folding the job hides its whole body; its header stays.
    QTest::mouseClick(first->findChild<QWidget*>("exportJobDisclosure"), Qt::LeftButton);
    context.expect(!first->expanded() && !first->findChild<QWidget*>("exportJobBody")->isVisibleTo(rig.area.get()) &&
                       first->findChild<QWidget*>("exportJobHeader")->isVisibleTo(rig.area.get()),
                   "the job's caret folds the body and keeps the header");
    // A job that finishes folds itself away; one that fails stays open to say why.
    rig.row(2)->setExpanded(true);
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        snapshot.jobs[1].state = host::ExportJobState::Done;
        snapshot.jobs[2].outputs[0].state = host::ExportOutputState::Failed;
    });
    context.expect(!rig.row(2)->expanded() && rig.row(3)->expanded(),
                   "a finished job folds and a failed job stays open");
    // The Render group and the outputs sit in the open job, in that order.
    context.expect(rig.row(3)->renderSection() != nullptr &&
                       rig.row(3)->findChild<QWidget*>("exportRangeKind") != nullptr,
                   "an open job holds its Render group");
}

void emptyState(TestContext& context) {
    Rig rig;
    auto* text = rig.find<QLabel>("exportEmptyText");
    context.expect(text != nullptr && text->isVisibleTo(rig.area.get()) &&
                       text->text() == "Nothing to render yet.",
                   "an empty list says there is nothing to render, in plain words");
    auto* add = rig.find<kit::KButton>("exportAddActiveButton");
    context.expect(add != nullptr && add->text() == "Add composition" &&
                       add->variant() == kit::KButton::Variant::Primary,
                   "the empty state offers Add composition as its Primary");
    context.expect(rig.find<QLabel>("exportEmptyHelp")->text().contains("Assets") &&
                       rig.find<QLabel>("exportEmptyHelp")->text().contains("Ctrl+M"),
                   "the empty state says how else to add one");
    context.expect(!rig.shown("exportStartAllButton") && rig.visiblePrimaries() == 1,
                   "an empty list has Add composition as its only Primary, and no Start All");
    const auto words = rig.visibleTexts().join('\n');
    context.expect(!words.contains("queue", Qt::CaseInsensitive),
                   "the empty state does not use the word queue");
}

void contextualButton(TestContext& context) {
    using State = host::ExportJobState;
    struct Case {
        State state;
        const char* text;
    };
    const Case cases[] = {{State::Ready, "Start"},        {State::Waiting, "Stop"},
                          {State::Preparing, "Stop"},     {State::Rendering, "Pause"},
                          {State::Pausing, "Stop"},       {State::Paused, "Resume"},
                          {State::Verifying, "Stop"},     {State::Stopping, "Stop"},
                          {State::Done, "Retry"},         {State::DoneWithWarnings, "Retry"},
                          {State::Failed, "Retry"},       {State::Stopped, "Retry"}};
    Rig rig;
    for (const auto& item : cases) {
        rig.publish([&](host::ExportQueueSnapshot& snapshot) {
            snapshot.jobs = {jobIn(rig, 1, item.state)};
        });
        auto* row = rig.row(1);
        auto* button = row != nullptr ? row->jobButton() : nullptr;
        context.expect(button != nullptr && button->text() == item.text,
                       (std::string("the job button for state ") + std::to_string(static_cast<int>(item.state)) +
                        " is " + item.text).c_str());
        // One job button, whatever the state; its variant is never Primary.
        context.expect(row != nullptr && row->findChildren<kit::KButton*>("exportJobButton").size() == 1 &&
                           button->variant() == kit::KButton::Variant::Secondary,
                       "there is one job button and it is Secondary");
    }
    // A rendering job that cannot pause offers Stop instead; a refused verb says why.
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto job = jobIn(rig, 1, State::Rendering);
        refuse(job.commands, host::ExportCommand::Pause, host::ExportUnavailableCode::NotRendering,
               "It is not rendering yet.");
        snapshot.jobs = {job};
    });
    context.expect(rig.row(1)->jobButton()->text() == "Stop", "a job that cannot pause offers Stop");
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto job = jobIn(rig, 1, State::Ready);
        refuse(job.commands, host::ExportCommand::Start, host::ExportUnavailableCode::PreflightBlocked,
               "Choose where to save this output.");
        snapshot.jobs = {job};
    });
    auto* button = rig.row(1)->jobButton();
    context.expect(button->text() == "Start" && !button->isEnabled() &&
                       button->toolTip() == "Choose where to save this output.",
                   "a refused Start stays Start, disabled, with the reason");
    // Each verb sends its command.
    rig.publish([&](host::ExportQueueSnapshot& snapshot) { snapshot.jobs = {jobIn(rig, 1, State::Ready)}; });
    rig.port.calls.clear();
    QTest::mouseClick(rig.row(1)->jobButton(), Qt::LeftButton);
    context.expect(rig.port.calls == std::vector<std::string>{"start 1"}, "Start starts the job");
    rig.publish([&](host::ExportQueueSnapshot& snapshot) { snapshot.jobs = {jobIn(rig, 1, State::Rendering)}; });
    rig.port.calls.clear();
    QTest::mouseClick(rig.row(1)->jobButton(), Qt::LeftButton);
    context.expect(rig.port.calls == std::vector<std::string>{"pause 1"}, "Pause pauses the job");
    rig.publish([&](host::ExportQueueSnapshot& snapshot) { snapshot.jobs = {jobIn(rig, 1, State::Paused)}; });
    rig.port.calls.clear();
    QTest::mouseClick(rig.row(1)->jobButton(), Qt::LeftButton);
    context.expect(rig.port.calls == std::vector<std::string>{"resume 1"}, "Resume resumes the job");
    rig.publish([&](host::ExportQueueSnapshot& snapshot) { snapshot.jobs = {jobIn(rig, 1, State::Waiting)}; });
    rig.port.calls.clear();
    QTest::mouseClick(rig.row(1)->jobButton(), Qt::LeftButton);
    context.expect(rig.port.calls == std::vector<std::string>{"stop 1"}, "Stop stops the job");
    rig.publish([&](host::ExportQueueSnapshot& snapshot) { snapshot.jobs = {jobIn(rig, 1, State::Failed)}; });
    rig.port.calls.clear();
    QTest::mouseClick(rig.row(1)->jobButton(), Qt::LeftButton);
    context.expect(rig.port.calls == std::vector<std::string>{"retry 1"}, "Retry runs the job again");
    // The whole set stays reachable from the overflow menu.
    const auto menu = rig.row(1)->buildMenu();
    QStringList names;
    for (auto* action : menu->actions())
        names.push_back(action->objectName());
    for (const auto* name : {"exportStartAction", "exportPauseAction", "exportResumeAction", "exportStopAction",
                             "exportRetryAction", "exportDuplicateAction", "exportMoveUpAction",
                             "exportMoveDownAction", "exportRemoveAction", "exportCopyCliAction",
                             "exportCopyPythonAction"})
        context.expect(names.contains(QString::fromLatin1(name)), name);
}

void startAllAndPrimary(TestContext& context) {
    using State = host::ExportJobState;
    Rig rig;
    context.expect(!rig.shown("exportStartAllButton"), "Start All is hidden with nothing in the list");
    // Jobs but none ready: no Start All, so no Primary.
    rig.threeJobs();
    context.expect(!rig.shown("exportStartAllButton") && rig.visiblePrimaries() == 0,
                   "Start All is hidden while no job is ready, and then nothing is Primary");
    context.expect(rig.shown("exportStopAllButton") &&
                       rig.find<kit::KButton>("exportStopAllButton")->variant() == kit::KButton::Variant::Danger,
                   "Stop All appears, as Danger, while a job runs");
    // One ready job: Start All is the only Primary.
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        snapshot.jobs = {jobIn(rig, 1, State::Ready), jobIn(rig, 2, State::Ready)};
        snapshot.activeJob.reset();
        refuse(snapshot.commands, host::ExportCommand::StopAll, host::ExportUnavailableCode::NothingToStop,
               "No job is running.");
    });
    auto* startAll = rig.find<kit::KButton>("exportStartAllButton");
    context.expect(rig.shown("exportStartAllButton") && startAll->variant() == kit::KButton::Variant::Primary,
                   "Start All shows, as Primary, while a job is ready");
    context.expect(rig.visiblePrimaries() == 1, "Start All is the only Primary");
    context.expect(!rig.shown("exportStopAllButton"), "Stop All is hidden while nothing runs");
    // Start All starts what can run and asks no second question.
    rig.port.calls.clear();
    QTest::mouseClick(startAll, Qt::LeftButton);
    context.expect(rig.port.calls == std::vector<std::string>{"startAll"} &&
                       QApplication::activeModalWidget() == nullptr,
                   "Start All starts the jobs that can run, with no dialog");
    // A job waiting for the artist's OK is left waiting, and the header says so.
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto waiting = jobIn(rig, 2, State::Ready);
        waiting.outputs[0].preflight.push_back({.code = host::ExportPreflightCode::DestinationExists,
                                                .target = host::ExportPreflightTarget::Destination,
                                                .severity = host::ExportPreflightSeverity::NeedsConfirmation,
                                                .reason = "out.mp4 already exists."});
        refuse(waiting.commands, host::ExportCommand::Start,
               host::ExportUnavailableCode::NeedsReplaceConfirmation, "Replace existing files?");
        snapshot.jobs = {jobIn(rig, 1, State::Ready), waiting};
    });
    QTest::mouseClick(rig.find<kit::KButton>("exportStartAllButton"), Qt::LeftButton);
    context.expect(rig.editor->startNoteText() == "1 started, 1 waiting for your OK",
                   "the header says what started and what waits for the OK");
    context.expect(rig.visiblePrimaries() == 1, "still one Primary with a job waiting for the OK");
    // The jobs that were waiting are answered: the note goes away.
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        snapshot.jobs = {jobIn(rig, 1, State::Ready), jobIn(rig, 2, State::Ready)};
    });
    context.expect(rig.editor->startNoteText().isEmpty(), "the note leaves when nobody waits");
    // A job that cannot start is not ready: Start All hides once only such jobs remain.
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto job = jobIn(rig, 1, State::Ready);
        refuse(job.commands, host::ExportCommand::Start, host::ExportUnavailableCode::PreflightBlocked,
               "Choose where to save this output.");
        snapshot.jobs = {job};
    });
    context.expect(!rig.shown("exportStartAllButton") && rig.visiblePrimaries() == 0,
                   "Start All is hidden when the only job cannot start");
    context.expect(rig.row(1)->statusText() == "Needs attention" || rig.row(1)->statusText() == "Needs a location",
                   "and that job says what it needs");
}

void statusWording(TestContext& context) {
    using State = host::ExportJobState;
    Rig rig;
    const auto statusOf = [&](const State state, const host::ExportDeviceRoute route = host::ExportDeviceRoute::None) {
        auto job = jobIn(rig, 1, state);
        job.progress.route = route;
        if (state == State::Failed)
            job.failure = host::ExportFailure{.reason = "boom", .summary = "It failed."};
        return ep::jobStatus(job);
    };
    context.expect(statusOf(State::Ready).text == "Ready", "Ready");
    context.expect(statusOf(State::Waiting).text == "Up next", "Waiting reads Up next");
    context.expect(statusOf(State::Preparing).text == "Getting ready", "Preparing reads Getting ready");
    context.expect(statusOf(State::Rendering).text == "Rendering", "Rendering");
    context.expect(statusOf(State::Rendering, host::ExportDeviceRoute::WaitingForViewer).text ==
                       "Paused while you work in the Viewer",
                   "waiting for the Viewer is a calm plain sentence");
    context.expect(statusOf(State::Rendering, host::ExportDeviceRoute::CpuReference).text ==
                       "Rendering (slower: no GPU available)",
                   "the CPU route says why it is slower");
    context.expect(statusOf(State::Rendering, host::ExportDeviceRoute::RecoveringGpu).text == "Reconnecting GPU",
                   "recovering the GPU reads Reconnecting GPU");
    context.expect(statusOf(State::Verifying).text == "Checking the file", "Verifying reads Checking the file");
    context.expect(statusOf(State::Done).text == "Done", "Done");
    context.expect(statusOf(State::DoneWithWarnings).text == "Done, with a note", "Done, with a note");
    context.expect(statusOf(State::Failed).text == "Failed", "Failed");
    context.expect(statusOf(State::Stopped).text == "Stopped", "Stopped");
    // A ready job that cannot start has its own words.
    const auto refused = [&](const host::ExportUnavailableCode code, const bool locationMissing) {
        auto job = jobIn(rig, 1, State::Ready);
        refuse(job.commands, host::ExportCommand::Start, code, "reason");
        if (locationMissing)
            job.outputs[0].preflight.push_back({.code = host::ExportPreflightCode::DestinationMissing,
                                                .target = host::ExportPreflightTarget::Destination,
                                                .severity = host::ExportPreflightSeverity::Blocking,
                                                .reason = "Choose where to save this output."});
        return ep::jobStatus(job).text;
    };
    context.expect(refused(host::ExportUnavailableCode::DestinationCheckPending, false) ==
                       "Checking where it saves…",
                   "a pending check reads Checking where it saves");
    context.expect(refused(host::ExportUnavailableCode::NeedsReplaceConfirmation, false) == "Waiting for your OK",
                   "a replace question reads Waiting for your OK");
    context.expect(refused(host::ExportUnavailableCode::PreflightBlocked, true) == "Needs a location",
                   "a missing location reads Needs a location");
    context.expect(refused(host::ExportUnavailableCode::PreflightBlocked, false) == "Needs attention",
                   "any other blocker reads Needs attention");
    // Neutral ink for every status; only a failure is red.
    for (int value = 1; value <= 12; ++value) {
        auto job = jobIn(rig, 1, static_cast<State>(value));
        if (job.state == State::Failed)
            job.failure = host::ExportFailure{.reason = "boom"};
        const auto status = ep::jobStatus(job);
        context.expect(!status.text.isEmpty(), "every job state has status text");
        const auto ink = ep::toneColor(status.tone);
        context.expect(ink != kit::Color::Warn && ink != kit::Color::Ok, "no status is amber or green");
        context.expect((ink == kit::Color::Error) == (job.state == State::Failed),
                       "only Failed is red");
    }
    // Output states.
    const auto outputOf = [&](const host::ExportOutputState state, const State jobState) {
        auto job = jobIn(rig, 1, jobState);
        job.outputs[0].state = state;
        job.progress.outputCount = 1;
        job.progress.fraction = 0.32;
        return ep::outputStatus(job.outputs[0], job).text;
    };
    context.expect(outputOf(host::ExportOutputState::Pending, State::Waiting) == "Up next", "an output waits as Up next");
    context.expect(outputOf(host::ExportOutputState::Pending, State::Ready) == "Ready", "a ready output reads Ready");
    context.expect(outputOf(host::ExportOutputState::Rendering, State::Rendering) == "Writing 32%",
                   "a writing output shows its percent");
    context.expect(outputOf(host::ExportOutputState::Published, State::Done) == "Saved", "a published output reads Saved");
    context.expect(outputOf(host::ExportOutputState::Failed, State::Failed) == "Failed", "a failed output reads Failed");
    // No state's wording is an engine word.
    for (const auto* word : {"Published", "Verifying", "Preparing", "Not ready", "Needs confirmation"})
        for (int value = 1; value <= 12; ++value) {
            auto job = jobIn(rig, 1, static_cast<State>(value));
            if (job.state == State::Failed)
                job.failure = host::ExportFailure{.reason = "boom"};
            context.expect(ep::jobStatus(job).text != word, "no status uses an internal word");
        }
}

void waitingForViewer(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto job = makeJob(1, "Shot 010", rig.composition(), {makeOutput(11)});
        job.state = host::ExportJobState::Rendering;
        job.progress.framesTotal = 240;
        job.progress.framesCompleted = 60;
        job.progress.elapsed = std::chrono::milliseconds(31000);
        job.progress.route = host::ExportDeviceRoute::WaitingForViewer;
        job.progress.remaining = std::chrono::milliseconds(9000); // never shown while waiting
        snapshot.jobs = {job};
        snapshot.activeJob = job.id;
    });
    auto* row = rig.row(1);
    context.expect(row->statusText() == "Paused while you work in the Viewer",
                   "waiting for the Viewer says so in plain words");
    context.expect(row->progressShown() && !row->progressText().contains("left"),
                   "waiting for the Viewer shows progress and no ETA");
    context.expect(row->findChild<kit::KProgressBar*>()->busy(), "waiting for the Viewer shows a busy bar");
    const auto status = ep::jobStatus(rig.port.snapshot()->jobs[0]);
    context.expect(ep::toneColor(status.tone) == kit::Color::Muted,
                   "waiting for the Viewer is neutral ink, never an error colour");
}

void progressInTheRow(TestContext& context) {
    Rig rig;
    rig.threeJobs();
    auto* row = rig.row(1);
    context.expect(row->progressShown(), "a rendering job shows its progress in its own row");
    context.expect(row->progressText().startsWith("121 / 240") && row->progressText().contains("about 0:42 left"),
                   "the row shows the frame count and the time left");
    auto* bar = row->findChild<kit::KProgressBar*>();
    context.expect(bar != nullptr && !bar->busy() && bar->completed() == 120 && bar->total() == 240,
                   "the bar fills with the frames done");
    const auto words = rig.visibleTexts().join('\n');
    context.expect(words.contains("Elapsed 0:07"), "elapsed time is shown in the open job");
    context.expect(!rig.row(2)->progressShown() && !rig.row(3)->progressShown(),
                   "a job that is not running shows no progress");
    // Settings are locked while it renders, and the row says so.
    context.expect(row->reasonsText().contains("Settings are locked while this job renders"),
                   "a running job says its settings are locked");
}

void replacePromptInline(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto output = makeOutput(11);
        output.preflight.push_back({.code = host::ExportPreflightCode::DestinationExists,
                                    .target = host::ExportPreflightTarget::Destination,
                                    .severity = host::ExportPreflightSeverity::NeedsConfirmation,
                                    .reason = "out.mp4 already exists."});
        auto job = makeJob(1, "Shot 010", rig.composition(), {output});
        refuse(job.commands, host::ExportCommand::Start,
               host::ExportUnavailableCode::NeedsReplaceConfirmation, "Replace existing files?");
        snapshot.jobs = {job};
    });
    auto* row = rig.row(1);
    auto* output = rig.output(1, 11);
    context.expect(row->statusText() == "Waiting for your OK", "the job says it waits for the OK");
    context.expect(row->expanded() && output->expanded(),
                   "the job and the output open on their own when they ask");
    context.expect(output->replacePromptText() == "A file called out.mp4 is already in renders.",
                   "the prompt names the file and the folder");
    auto* replace = output->findChild<kit::KButton*>("exportReplaceButton");
    auto* other = output->findChild<kit::KButton*>("exportChooseNameButton");
    context.expect(replace != nullptr && replace->text() == "Replace it" && replace->isVisibleTo(rig.area.get()) &&
                       replace->variant() == kit::KButton::Variant::Secondary,
                   "Replace it is a Secondary button in the output");
    context.expect(other != nullptr && other->text() == "Choose another name" &&
                       other->variant() == kit::KButton::Variant::Secondary,
                   "Choose another name is Secondary too");
    context.expect(rig.visiblePrimaries() == 0 && !rig.shown("exportStartAllButton"),
                   "no button of the prompt is Primary, and Start All waits");
    rig.port.calls.clear();
    QTest::mouseClick(replace, Qt::LeftButton);
    context.expect(rig.port.calls == std::vector<std::string>{"confirmReplace 1 11"},
                   "Replace it calls confirmReplace(job, output)");
    QTest::mouseClick(other, Qt::LeftButton);
    context.expect(QApplication::activeModalWidget() == nullptr, "the prompt opens no dialog");
    // Answered: the prompt leaves and the job is ready.
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto& job = snapshot.jobs[0];
        job.outputs[0].replaceConfirmed = true;
        job.commands = allAvailable();
    });
    context.expect(rig.output(1, 11)->replacePromptText().isEmpty() && rig.row(1)->statusText() == "Ready" &&
                       rig.shown("exportStartAllButton"),
                   "once answered the prompt leaves and the job is ready");
}

void visibleReasons(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto output = makeOutput(11);
        output.preflight.push_back({.code = host::ExportPreflightCode::DestinationMissing,
                                    .target = host::ExportPreflightTarget::Destination,
                                    .severity = host::ExportPreflightSeverity::Blocking,
                                    .reason = "Choose where to save this output."});
        auto job = makeJob(1, "Shot 010", rig.composition(), {output});
        refuse(job.commands, host::ExportCommand::Start, host::ExportUnavailableCode::PreflightBlocked,
               "Choose where to save this output.");
        refuse(job.commands, host::ExportCommand::AddOutput, host::ExportUnavailableCode::OutputLimitReached,
               "A job holds at most 16 outputs.");
        job.outputs[0].commands = allAvailable();
        refuse(job.outputs[0].commands, host::ExportCommand::RemoveOutput, host::ExportUnavailableCode::LastOutput,
               "A job keeps at least one output.");
        snapshot.jobs = {job};
        refuse(snapshot.commands, host::ExportCommand::StopAll, host::ExportUnavailableCode::NothingToStop,
               "No job is running.");
        refuse(snapshot.commands, host::ExportCommand::RemoveFinished,
               host::ExportUnavailableCode::NothingFinished, "No job has finished.");
    });
    auto* row = rig.row(1);
    rig.open(1, 11);
    context.expect(row->statusText() == "Needs a location", "the status names what is missing");
    context.expect(rig.shown("exportStartReason") &&
                       row->findChild<QLabel*>("exportStartReason")->text() == "Choose where to save this output.",
                   "the reason Start cannot run is a visible line in the job");
    context.expect(rig.output(1, 11)->reasonsText().contains("Choose where to save this output."),
                   "the output shows its own blocker under Save to");
    context.expect(rig.shown("exportAddOutputReason") &&
                       row->findChild<QLabel*>("exportAddOutputReason")->text() == "A job holds at most 16 outputs." &&
                       !row->findChild<kit::KButton*>("exportAddOutputButton")->isEnabled(),
                   "a reached output limit is said next to Add output");
    auto menu = rig.output(1, 11)->buildMenu();
    QAction* remove = nullptr;
    for (auto* action : menu->actions())
        if (action->objectName() == "exportRemoveOutputAction")
            remove = action;
    context.expect(remove != nullptr && !remove->isEnabled() && remove->text().contains("keeps at least one") ,
                   "Remove output says in its own text why it is off when it is the last");
    context.expect(!rig.shown("exportStopAllButton") && !rig.shown("exportRemoveFinishedButton"),
                   "buttons that have nothing to do are not shown disabled");
    // A running job's controls are off, with the reason in the job.
    rig.threeJobs();
    context.expect(rig.row(1)->reasonsText().contains("Settings are locked while this job renders"),
                   "locked settings are explained in the row");
    auto* name = rig.row(1)->findChild<kit::KDropdown*>("exportRangeKind");
    context.expect(name != nullptr && !name->isEnabled() && name->toolTip().contains("locked"),
                   "a running job's controls are disabled with the reason");
}

void noRawCodes(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto job = makeJob(1, "Shot 010", rig.composition(), {makeOutput(11)});
        job.state = host::ExportJobState::Failed;
        job.failure = host::ExportFailure{
            .code = host::ExportFailureCode::AnalysisRefused,
            .output = job.outputs[0].id,
            .frame = std::nullopt,
            .diagnosticCode = "output.analysis.facet=3",
            .reason = "This format cannot keep everything the frame needs. Choose OpenEXR.",
            .summary = std::string(host::exportFailureSummary(host::ExportFailureCode::AnalysisRefused))};
        job.outputs[0].state = host::ExportOutputState::Failed;
        host::ExportOutputVerification verification;
        verification.output = job.outputs[0].id;
        verification.overall = host::ExportCheckOutcome::Failed;
        verification.checks.push_back({.check = host::ExportCheck::AnalysisPolicy,
                                       .outcome = host::ExportCheckOutcome::Failed,
                                       .code = "output.analysis.facet=3"});
        job.outputs[0].verification = verification;
        snapshot.jobs = {job};
    });
    rig.open(1, 11);
    auto visible = rig.visibleTexts();
    for (auto* widget : rig.area->findChildren<QWidget*>())
        if (widget->isVisibleTo(rig.area.get()))
            visible.push_back(widget->toolTip());
    const auto all = visible.join('\n');
    context.expect(all.contains("OpenEXR"), "the failure line is a sentence with a remedy");
    for (const auto* raw : {"stage=", "code=", "facet", "output.analysis", "export.", "0x", "digest", "Digest",
                            "CPU reference", "preflight", "publication", "verification:"})
        context.expect(!all.contains(QString::fromLatin1(raw)), (std::string("no raw word in view: ") + raw).c_str());
    // The raw detail lives only behind Show details, and can be copied.
    auto* details = rig.find<QPlainTextEdit>("exportFailureDetails");
    context.expect(details != nullptr && details->toPlainText().contains("facet=3") &&
                       !details->isVisibleTo(rig.area.get()),
                   "raw detail is behind Show details, closed by default");
    auto* toggle = rig.find<kit::KButton>("exportDetailsToggle");
    context.expect(toggle != nullptr && toggle->text() == "Show details" &&
                       toggle->variant() == kit::KButton::Variant::Secondary,
                   "Show details is a Secondary button");
    toggle->setChecked(true);
    context.expect(details->isVisibleTo(rig.area.get()), "Show details opens the raw text");
    auto* copy = rig.find<kit::KButton>("exportCopyDetailsButton");
    context.expect(copy != nullptr && copy->variant() == kit::KButton::Variant::Secondary,
                   "Copy details is a Secondary button");
}

void resultsInPlainWords(TestContext& context) {
    QSettings().remove(QStringLiteral("export/job/log/collapsed"));
    Rig rig;
    rig.threeJobs();
    // A failed job: the sentence, then what the check found, in plain words.
    context.expect(rig.row(3)->results()->summaryText().startsWith("Couldn't save Shot 030."),
                   "a failed job says it couldn't save");
    const auto lines = rig.row(3)->results()->lineTexts().join('\n');
    context.expect(lines.contains("wrong number of frames") && lines.contains("expected 240, found 239"),
                   "a failed check reads expected against found");
    // A job that ran and passed.
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto job = makeJob(1, "Shot 010", rig.composition(), {makeOutput(11)});
        job.state = host::ExportJobState::Done;
        job.outputs[0].state = host::ExportOutputState::Published;
        host::ExportOutputVerification verification;
        verification.output = job.outputs[0].id;
        verification.overall = host::ExportCheckOutcome::Passed;
        for (const auto check : {host::ExportCheck::Reopen, host::ExportCheck::FrameCount, host::ExportCheck::ColourTags,
                                 host::ExportCheck::Timing, host::ExportCheck::ArtifactDigest})
            verification.checks.push_back({.check = check, .outcome = host::ExportCheckOutcome::Passed});
        verification.artifacts.publishedArtifacts = 1;
        verification.artifacts.totalBytes = 38'000'000;
        verification.artifacts.firstPath = "/project/renders/out.mp4";
        verification.provenance.gpuFrames = 240;
        verification.provenance.framesReused = 40;
        job.outputs[0].verification = verification;
        job.outputs[0].import.status = host::ExportAssetImportStatus::Failed;
        job.outputs[0].import.reason = "The project is read-only.";
        snapshot.jobs = {job};
        snapshot.activeJob.reset();
    });
    rig.row(1)->setExpanded(true);
    auto* results = rig.row(1)->results();
    context.expect(results->isVisibleTo(rig.area.get()), "a job that ran shows its result");
    context.expect(results->summaryText().startsWith("Saved 1 file,") &&
                       results->summaryText().endsWith("checked and matches what you asked for."),
                   "the result is one plain sentence");
    const auto passed = results->lineTexts();
    context.expect(passed.contains("File opens") && passed.contains("Right number of frames") &&
                       passed.contains("Rendered on the GPU"),
                   "the checks read as File opens, Right number of frames, Rendered on the GPU");
    context.expect(std::ranges::any_of(passed, [](const QString& line) { return line.startsWith("Colours tagged"); }),
                   "the colour tags read as Colours tagged as ...");
    context.expect(std::ranges::any_of(passed, [](const QString& line) { return line.contains("40 of 280 frames reused"); }),
                   "reused frames are reported");
    context.expect(!passed.join('\n').contains("Timing") && !passed.join('\n').contains("Digest"),
                   "a passed check that needs no words is not listed");
    context.expect(std::ranges::any_of(passed, [](const QString& line) {
                       return line.startsWith("Couldn't add to Assets: The project is read-only.");
                   }),
                   "an import that failed is said separately, in neutral ink");
    auto* show = results->findChild<kit::KButton*>("exportShowInFolderButton");
    context.expect(show != nullptr && show->variant() == kit::KButton::Variant::Secondary,
                   "a saved output has a Secondary Show in folder");
    // The Log is folded by default.
    context.expect(results->logSection()->isCollapsed(), "the Log is folded by default");
}

void formatSettings(TestContext& context) {
    QSettings().remove(QStringLiteral("export/output/quality/collapsed"));
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        snapshot.jobs = {makeJob(1, "Movie", rig.composition(), {makeOutput(11)}),
                         makeJob(2, "Frames", rig.composition(),
                                 {makeOutput(21, "builtin:exr-sequence", document::ExportFormat::OpenExr,
                                             "seq/shot.####.exr")}),
                         makeJob(3, "Sound", rig.composition(),
                                 {makeOutput(31, "builtin:wav", document::ExportFormat::Wav, "a.wav")})};
    });
    auto* movie = rig.output(1, 11);
    context.expect(movie->qualitySection()->isCollapsed(), "Quality is folded by default");
    rig.open(1, 11);
    rig.open(2, 21);
    rig.open(3, 31);
    const auto labelsOf = [&](ep::OutputRow* row) {
        QStringList labels;
        for (auto* label : row->findChildren<QLabel*>("propertiesRowLabel"))
            if (label->isVisibleTo(rig.area.get()))
                labels.push_back(label->toolTip().isEmpty() ? label->text() : label->toolTip());
        return labels;
    };
    const auto movieLabels = labelsOf(rig.output(1, 11));
    for (const auto* label : {"Format", "Save to", "Quality target", "Use", "Display", "View", "Include the look",
                              "Cached frames", "Include audio", "Sample rate", "After render"})
        context.expect(movieLabels.contains(QString::fromLatin1(label)),
                       (std::string("a movie shows ") + label).c_str());
    for (const auto* label : {"Compression", "Colour space", "File numbering", "File names", "Bake look",
                              "Reuse cached frames", "Encoder", "Bit rate"})
        context.expect(!movieLabels.contains(QString::fromLatin1(label)),
                       (std::string("a movie does not show ") + label).c_str());
    const auto imageLabels = labelsOf(rig.output(2, 21));
    for (const auto* label : {"Compression", "Colour space", "File numbering", "File names", "Include the look"})
        context.expect(imageLabels.contains(QString::fromLatin1(label)),
                       (std::string("an EXR sequence shows ") + label).c_str());
    for (const auto* label : {"Quality target", "Use", "Display", "View", "Include audio", "Sample rate"})
        context.expect(!imageLabels.contains(QString::fromLatin1(label)),
                       (std::string("an EXR sequence does not show ") + label).c_str());
    const auto soundLabels = labelsOf(rig.output(3, 31));
    context.expect(soundLabels.contains("Sample rate") && !soundLabels.contains("Include the look") &&
                       !soundLabels.contains("Compression") && !soundLabels.contains("Quality target"),
                   "a sound file shows only its sample rate");
    // Colour space, Display and View are dropdowns of what the project offers, never text boxes.
    auto* image = rig.output(2, 21);
    auto* space = image->findChild<kit::KDropdown*>("exportColourSpace");
    context.expect(space != nullptr && image->findChild<kit::KLineEdit*>("exportColourSpace") == nullptr &&
                       space->count() >= 2 && space->itemText(0) == "Working colour space",
                   "Colour space is a dropdown that starts with the working space");
    auto* display = rig.output(1, 11)->findChild<kit::KDropdown*>("exportDisplay");
    auto* view = rig.output(1, 11)->findChild<kit::KDropdown*>("exportView");
    context.expect(display != nullptr && view != nullptr &&
                       rig.output(1, 11)->findChild<kit::KLineEdit*>("exportDisplay") == nullptr &&
                       rig.output(1, 11)->findChild<kit::KLineEdit*>("exportView") == nullptr &&
                       display->itemText(0) == "Project default" && view->itemText(0) == "Project default",
                   "Display and View are dropdowns that start with the project default");
    rig.port.calls.clear();
    if (space->count() >= 2) {
        space->chooseIndex(1);
        context.expect(rig.port.calls.size() == 1 && rig.port.calls[0] == "editOutput 2 21 settings",
                       "choosing a colour space is one command");
    }
    rig.port.calls.clear();
    if (display->count() >= 2) {
        display->chooseIndex(1);
        context.expect(rig.port.calls.size() == 1 && rig.port.calls[0] == "editOutput 1 11 settings",
                       "choosing a display is one command");
    }
    rig.port.calls.clear();
    auto* cached = rig.output(1, 11)->findChild<kit::KSwitch*>("exportUseCachedFrames");
    cached->setChecked(!cached->isChecked());
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0] == "editOutput 1 11 settings",
                   "Reuse cached frames is one command");
    rig.port.calls.clear();
    image->findChild<kit::KDropdown*>("exportSequenceStartMode")->chooseIndex(1);
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0] == "editOutput 2 21 settings",
                   "File numbering from a number is one command");
}

void rangeFields(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        snapshot.jobs = {makeJob(1, "Shot 010", rig.composition(), {makeOutput(11)})};
    });
    const auto fieldsShown = [&](const bool first, const bool last, const bool frame) {
        auto* row = rig.row(1);
        return row->findChild<kit::KValueField*>("exportRangeFirst")->isVisibleTo(rig.area.get()) == first &&
               row->findChild<kit::KValueField*>("exportRangeLast")->isVisibleTo(rig.area.get()) == last &&
               row->findChild<kit::KValueField*>("exportRangeFrame")->isVisibleTo(rig.area.get()) == frame;
    };
    context.expect(fieldsShown(false, false, false), "Work area has no frame fields");
    auto* kind = rig.row(1)->findChild<kit::KDropdown*>("exportRangeKind");
    context.expect(kind->itemText(0) == "Work area" && kind->itemText(1) == "Whole composition" &&
                       kind->itemText(2) == "Custom frames" && kind->itemText(3) == "One frame",
                   "the range choices read in plain words");
    rig.port.calls.clear();
    kind->chooseIndex(2);
    const auto custom = static_cast<int>(document::ExportRangeKind::Custom);
    const auto one = static_cast<int>(document::ExportRangeKind::Frame);
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0].starts_with("editJob 1 range=" + std::to_string(custom) + ":"),
                   "choosing Custom frames is one command");
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        snapshot.jobs[0].record.render.range = {document::ExportRangeKind::Custom, 10, 40};
    });
    context.expect(fieldsShown(true, true, false), "Custom frames shows First and Last");
    rig.port.calls.clear();
    auto* last = rig.row(1)->findChild<kit::KValueField*>("exportRangeLast");
    last->setIntegerValue(80);
    emit last->editFinished();
    context.expect(rig.port.calls.size() == 1 &&
                       rig.port.calls[0] == "editJob 1 range=" + std::to_string(custom) + ":10-80",
                   "editing the last frame is one command");
    rig.port.calls.clear();
    last->setIntegerValue(5);
    emit last->editFinished();
    context.expect(rig.port.calls.empty() && rig.row(1)->reasonsText().contains("last frame comes before"),
                   "a last frame before the first is refused in the row");
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        snapshot.jobs[0].record.render.range = {document::ExportRangeKind::Frame, 7, 7};
    });
    context.expect(fieldsShown(false, false, true), "One frame shows a Frame field only");
    rig.port.calls.clear();
    auto* frame = rig.row(1)->findChild<kit::KValueField*>("exportRangeFrame");
    frame->setIntegerValue(12);
    emit frame->editFinished();
    context.expect(rig.port.calls.size() == 1 &&
                       rig.port.calls[0] == "editJob 1 range=" + std::to_string(one) + ":12-12",
                   "editing the frame is one command");
}

void addOutputWithFormat(TestContext& context) {
    Rig rig;
    rig.threeJobs();
    auto* row = rig.row(2);
    context.expect(row->outputRows().size() == 1, "the job starts with one output");
    rig.port.calls.clear();
    row->addOutputWithFormat("builtin:prores-422hq-mov");
    context.expect(rig.port.calls == std::vector<std::string>{"addOutput 2 builtin:prores-422hq-mov"},
                   "Add output with a format is one command that names the format");
    context.expect(row->outputRows().size() == 2, "the new output appears");
    auto* added = row->outputRows().back();
    context.expect(added->expanded() && added->formatText() == "ProRes 422 HQ",
                   "the new output opens in the format that was chosen");
    // The menu lists the same groups as the Format dropdown, video first.
    auto* button = row->findChild<kit::KButton*>("exportAddOutputButton");
    context.expect(button != nullptr && button->variant() == kit::KButton::Variant::Secondary &&
                       button->toolTip() == "Render this job in another format too.",
                   "Add output is Secondary and says what it does");
    // Remove output goes through the output's overflow menu.
    rig.port.calls.clear();
    auto menu = added->buildMenu();
    for (auto* action : menu->actions())
        if (action->objectName() == "exportRemoveOutputAction")
            action->trigger();
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0].starts_with("removeOutput 2 "),
                   "Remove output is one command");
}

void editsAreOneCommand(TestContext& context) {
    Rig rig;
    rig.threeJobs();
    rig.port.calls.clear();
    // Rename in place: double click or F2, commit with Return or Escape.
    auto* row = rig.row(2);
    row->startRename();
    auto* rename = row->findChild<kit::KLineEdit*>("exportJobRename");
    context.expect(rename != nullptr && rename->isVisibleTo(rig.area.get()), "F2 opens the name for editing");
    rename->setText("Hero");
    emit rename->inlineEdit()->accepted("Hero", kit::KInlineEdit::Ending::Escape);
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0] == "editJob 2 name=Hero",
                   "renaming a job is one command");
    context.expect(!rename->isVisibleTo(rig.area.get()), "the name goes back to a label");
    rig.port.calls.clear();
    row->findChild<kit::KSwitch*>("exportJobEnabled")->setChecked(false);
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0] == "editJob 2 enabled=0",
                   "the Include in Start All switch is one command");
    rig.port.calls.clear();
    rig.open(2, 21)->findChild<kit::KSwitch*>("exportOutputEnabled")->setChecked(false);
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0] == "editOutput 2 21 enabled=0",
                   "an output's switch is one command");
    rig.port.calls.clear();
    const auto moveMenu = row->buildMenu();
    for (auto* action : moveMenu->actions())
        if (action->objectName() == "exportMoveUpAction")
            action->trigger();
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0] == "moveJob 2 0",
                   "Move up is one MoveJob");
    rig.port.calls.clear();
    auto* range = row->findChild<kit::KDropdown*>("exportRangeKind");
    range->chooseIndex(2);
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0].starts_with("editJob 2 range="),
                   "choosing a custom range is one command");
    rig.port.calls.clear();
    auto* format = rig.output(2, 21)->findChild<kit::KDropdown*>("exportPresetPicker");
    format->chooseIndex(format->findData(QString("builtin:prores-422hq-mov")));
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0].starts_with("applyPreset 2 21"),
                   "choosing a format applies it in one command");
    // Video first, then image sequences, as disabled group titles.
    context.expect(format->itemText(0) == "Video" && !format->isItemEnabled(0),
                   "the format list starts with the Video group");
    int sequences = -1;
    for (int i = 0; i < format->count(); ++i)
        if (format->itemText(i) == "Image sequences")
            sequences = i;
    context.expect(sequences > 0, "Image sequences is the second group");
    rig.port.calls.clear();
    rig.output(2, 21)->findChild<kit::KSwitch*>("exportImportIntoAssets")->setChecked(true);
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0] == "editOutput 2 21 import=1",
                   "Add to Assets when done is one per-output command");
    // A job label colour clears from the overflow menu.
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        snapshot.jobs[1].record.labelColor = std::array<std::uint8_t, 3>{200, 120, 60};
    });
    rig.port.calls.clear();
    const auto labelMenu = rig.row(2)->buildMenu();
    for (auto* action : labelMenu->actions())
        if (action->objectName() == "exportClearLabelAction")
            action->trigger();
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0] == "editJob 2 label=clear",
                   "Remove label colour is one command");
    // A refusal is shown in the row, not swallowed.
    rig.port.nextEdit = {false, "Edits are locked while the job runs.", std::nullopt, std::nullopt, {}};
    rig.row(2)->findChild<kit::KSwitch*>("exportJobEnabled")->setChecked(false);
    context.expect(rig.row(2)->reasonsText().contains("Edits are locked while the job runs."),
                   "a refused edit shows its reason");
    rig.port.nextEdit = {true, {}, std::nullopt, std::nullopt, {}};
}

void destinationFields(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto output = makeOutput(11);
        output.preflight.push_back({.code = host::ExportPreflightCode::DestinationCheckPending,
                                    .target = host::ExportPreflightTarget::Destination,
                                    .severity = host::ExportPreflightSeverity::Blocking,
                                    .reason = "Checking destination."});
        snapshot.jobs = {makeJob(1, "Shot 010", rig.composition(), {output})};
    });
    auto* output = rig.open(1, 11);
    context.expect(output->reasonsText().contains("Checking where it saves…"),
                   "a pending destination says Checking where it saves");
    context.expect(!rig.shown("exportReplaceBox"), "the replace prompt is gone when nothing exists");
    context.expect(!rig.shown("exportEstimate"), "an absent estimate shows nothing");
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        snapshot.jobs[0].outputs[0].estimatedBytes = 12'000'000;
        snapshot.jobs[0].outputs[0].freeBytesAtDestination = 200'000'000'000;
    });
    auto* facts = rig.find<QLabel>("exportEstimate");
    context.expect(facts->isVisibleTo(rig.area.get()) && facts->text().startsWith("About ") &&
                       facts->text().contains("free"),
                   "a given estimate and the free space are shown under Save to");
    context.expect(output->resolvedText().contains("out.mp4"), "the resolved file shows under Save to");
    // A sequence says what it writes.
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto seq = makeOutput(21, "builtin:exr-sequence", document::ExportFormat::OpenExr, "seq/shot.####.exr");
        seq.destination.fileCount = 240;
        seq.destination.firstPath = "/project/seq/shot.0000.exr";
        seq.destination.lastPath = "/project/seq/shot.0239.exr";
        snapshot.jobs = {makeJob(1, "Shot 010", rig.composition(), {seq})};
    });
    context.expect(rig.output(1, 21)->resolvedText().contains("shot.0000.exr") &&
                       rig.output(1, 21)->resolvedText().contains("shot.0239.exr (240 files)"),
                   "a sequence shows its first and last file and the count");
    // Browse is inside the field, and the tokens help is a small Secondary "?".
    auto* field = rig.output(1, 21)->findChild<kit::KPathField*>("exportDestination");
    context.expect(field != nullptr && field->findChild<QAction*>("exportBrowseAction") != nullptr &&
                       rig.output(1, 21)->findChild<kit::KButton*>("exportBrowseButton") == nullptr,
                   "Browse lives inside the Save to field");
    auto* help = rig.output(1, 21)->findChild<kit::KButton*>("exportTokenHelpButton");
    context.expect(help != nullptr && help->text() == "?" && help->variant() == kit::KButton::Variant::Secondary,
                   "the name tokens help is a small Secondary ? button");
    context.expect(!rig.visibleTexts().join('\n').contains("Tokens:"),
                   "the tokens paragraph is not always on screen");
    // Typing a path is one command.
    rig.port.calls.clear();
    field->setText("/exports/out.mp4");
    emit field->editingFinished();
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0].starts_with("editOutput 1 21"),
                   "typing where to save is one command");
    const auto overwrite = rig.output(1, 21)->findChild<kit::KDropdown*>("exportOverwrite");
    context.expect(overwrite->itemText(0) == "Ask me first" && overwrite->itemText(1) == "Replace it" &&
                       overwrite->itemText(2) == "Don't render",
                   "the overwrite choices read in plain words");
}

void platformNotSupported(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        snapshot.jobs = {makeJob(1, "Shot 010", rig.composition(), {makeOutput(11)})};
        for (const auto command : {host::ExportCommand::StartAll})
            refuse(snapshot.commands, command, host::ExportUnavailableCode::PlatformNotSupported,
                   "Export runs on Linux in this version");
        refuse(snapshot.jobs[0].commands, host::ExportCommand::Start,
               host::ExportUnavailableCode::PlatformNotSupported, "Export runs on Linux in this version");
    });
    auto* note = rig.find<QLabel>("exportPlatformNote");
    context.expect(note != nullptr && note->isVisibleTo(rig.area.get()) &&
                       note->text() == "Export runs on Linux in this version",
                   "an unsupported platform says so under the header");
    context.expect(!rig.find<kit::KButton>("exportStartAllButton")->isEnabled() &&
                       !rig.row(1)->jobButton()->isEnabled(),
                   "Start and Start All are disabled");
    rig.row(1)->startRename();
    context.expect(rig.row(1)->findChild<kit::KLineEdit*>("exportJobRename")->isVisibleTo(rig.area.get()),
                   "editing still works where export cannot run");
    context.expect(rig.row(1)->statusText() == "Ready", "an unsupported platform does not turn a job into a fault");
}

void logAndVerification(TestContext& context) {
    Rig rig;
    rig.threeJobs();
    rig.port.logs[3] = {{.sequence = 1, .sinceStart = std::chrono::milliseconds(1000),
                         .code = "frame.ok", .message = "Frame 1 written"}};
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        snapshot.jobs[2].logEntryCount = 1;
        snapshot.jobs[2].lastLogSequence = 1;
    });
    auto* results = rig.row(3)->results();
    context.expect(results != nullptr && results->logText().contains("Frame 1 written"),
                   "the log shows the run's entries");
    rig.port.logs[3].push_back({.sequence = 2, .sinceStart = std::chrono::milliseconds(2000),
                                .code = "frame.ok", .message = "Frame 2 written"});
    rig.port.logGap = true;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) { snapshot.jobs[2].lastLogSequence = 2; });
    context.expect(results->logText().contains("Frame 2 written") &&
                       results->logText().contains("Earlier entries were trimmed"),
                   "new entries append and a trimmed gap is reported");
    context.expect(results->logText().count("Frame 1 written") == 1, "entries are not re-read");
}

void narrowAndWide(TestContext& context) {
    Rig rig;
    rig.threeJobs();
    context.expect(!rig.row(1)->twoLineHeader(), "a wide header is one line");
    rig.area->resize(420, 760);
    QApplication::processEvents();
    context.expect(rig.row(1)->twoLineHeader() && rig.row(2)->twoLineHeader(),
                   "a narrow header folds onto two lines");
    auto* scroll = rig.find<QScrollArea>("exportJobList");
    context.expect(scroll != nullptr && !scroll->horizontalScrollBar()->isVisible(),
                   "a narrow list needs no horizontal scrolling");
    rig.row(1)->setExpanded(true);
    rig.open(1, 11);
    context.expect(scroll->widget()->width() <= scroll->viewport()->width(),
                   "an open job fits a narrow list: nothing is pushed past the edge");
    rig.area->resize(900, 760);
    QApplication::processEvents();
    context.expect(!rig.row(1)->twoLineHeader(), "the header is one line again when wide");
}

void emptyAndAdd(TestContext& context) {
    Rig rig;
    auto* add = rig.find<kit::KButton>("exportAddActiveButton");
    context.expect(add != nullptr && add->isEnabled(), "Add composition is enabled");
    QTest::mouseClick(add, Qt::LeftButton);
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0].starts_with("addJobs "),
                   "Add composition adds a job");
    context.expect(rig.editor->jobRows().size() == 1, "the new job appears in the list");
    context.expect(rig.editor->selectedJob().has_value(), "the new job is selected");
    context.expect(rig.shown("exportAddCompositionButton") &&
                       rig.find<kit::KButton>("exportAddCompositionButton")->variant() ==
                           kit::KButton::Variant::Secondary,
                   "with jobs, the header offers Add composition as a Secondary button");
    // A refusal leaves the button with its reason.
    Rig closed;
    closed.publish([](host::ExportQueueSnapshot& snapshot) {
        refuse(snapshot.commands, host::ExportCommand::AddJob, host::ExportUnavailableCode::QueueFull,
               "The queue is full.");
    });
    context.expect(!closed.find<kit::KButton>("exportAddActiveButton")->isEnabled() &&
                       closed.find<kit::KButton>("exportAddActiveButton")->toolTip() == "The queue is full.",
                   "Add gives the queue's reason when it cannot add");
}

void dragFromAssets(TestContext& context) {
    Rig rig;
    rig.threeJobs();
    rig.port.calls.clear();
    QMimeData mime;
    mime.setData(kCompositionMimeType, QByteArray::number(static_cast<qulonglong>(rig.composition().value())));
    const QPoint where(60, 90);
    QDragEnterEvent enter(where, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(rig.editor, &enter);
    context.expect(enter.isAccepted(), "the list accepts a composition dragged from Assets");
    QDropEvent drop(where, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(rig.editor, &drop);
    context.expect(drop.isAccepted() && rig.port.calls.size() == 1 && rig.port.calls[0].starts_with("addJobs "),
                   "dropping a composition from Assets adds a job");
    context.expect(rig.editor->jobRows().size() == 4, "the dropped composition is a new row");
    // Anything else is refused.
    QMimeData other;
    other.setText("not a composition");
    QDragEnterEvent refused(where, Qt::CopyAction, &other, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(rig.editor, &refused);
    context.expect(!refused.isAccepted(), "a drag that is not a composition is refused");
    // Over the empty page too.
    Rig empty;
    mime.setData(kCompositionMimeType, QByteArray::number(static_cast<qulonglong>(empty.composition().value())));
    QDragEnterEvent emptyEnter(where, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(empty.editor, &emptyEnter);
    QDropEvent emptyDrop(where, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(empty.editor, &emptyDrop);
    context.expect(empty.port.calls.size() == 1, "a drop on the empty list adds a job too");
}

void panelsNeverPump(TestContext& context) {
    Rig rig;
    rig.threeJobs();
    const int pumps = rig.port.pumps;
    // Selecting, editing and refreshing a panel never pumps; a second panel changes nothing.
    rig.editor->selectJob(document::ExportJobId::fromRaw(2));
    (void)rig.adapter->start(document::ExportJobId::fromRaw(2));
    context.expect(rig.port.pumps == pumps, "panels and commands never pump; only drain() does");
}

void availabilityReasons(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto job = makeJob(1, "Shot 010", rig.composition(), {makeOutput(11)});
        refuse(job.commands, host::ExportCommand::Start, host::ExportUnavailableCode::PreflightBlocked,
               "Choose a destination.");
        refuse(job.commands, host::ExportCommand::Retry, host::ExportUnavailableCode::NotFinished,
               "Only a finished job can run again.");
        snapshot.jobs = {job};
    });
    auto* start = rig.row(1)->jobButton();
    context.expect(!start->isEnabled() && start->toolTip() == "Choose a destination.",
                   "Start gives its reason (as a tooltip and as a line in the job)");
    context.expect(rig.row(1)->reasonsText().contains("Choose a destination."), "and as a visible line");
    // The overflow menu lists reasons as tooltips.
    const auto menu = rig.row(1)->buildMenu();
    bool reasoned = false;
    for (auto* action : menu->actions())
        if (action->objectName() == "exportRetryAction")
            reasoned = !action->isEnabled() && action->toolTip().contains("finished");
    context.expect(reasoned, "a menu action that cannot run carries its reason");
}

// ---------------------------------------------------------------------------------------------
// Dragging compositions from Assets

void dropManyFromAssets(TestContext& context) {
    QTemporaryDir directory;
    QImage image(8, 8, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    context.expect(image.save(directory.filePath("still.png")), "a footage file is written");
    runtime::TaskScheduler scheduler;
    ProjectHost host(scheduler);
    CompositionSession session(*host.liveDocumentAndStack().first, *host.liveDocumentAndStack().second,
                               host.lowestCompositionId());
    TaskUiBridge bridge(scheduler);
    AssetController controller(session, host, scheduler, bridge);
    std::vector<document::CompositionId> comps;
    for (const char* name : {"Alpha", "Beta", "Gamma"}) {
        commands::Transaction seed("Seed", session.snapshot().revision());
        seed.emplace<commands::AddComposition>(name, document::CompositionFormat{},
                                               core::RationalTime::fromInteger(10));
        const auto made = session.executeTransaction(std::move(seed));
        const auto id = made.outputId<document::CompositionId>(commands::kAddCompositionOutput);
        context.expect(id.has_value(), "a composition is seeded");
        if (id.has_value())
            comps.push_back(*id);
    }
    if (comps.size() != 3)
        return;
    (void)session.setComposition(comps[0]);
    controller.importFiles({directory.filePath("still.png")}, std::nullopt);
    QElapsedTimer timer;
    timer.start();
    while (session.snapshot().project().assets().empty() && timer.elapsed() < 20000)
        QTest::qWait(10);
    context.expect(!session.snapshot().project().assets().empty(), "the footage is imported");
    if (session.snapshot().project().assets().empty())
        return;
    const auto asset = session.snapshot().project().assets().front().id;

    FakeExportPort port;
    ExportQueueAdapter adapter(port);
    EditorRegistry registry;
    (void)registerExportEditor(registry, session, &adapter);
    EditorArea area(registry, "bloom.export", QString{});
    area.resize(900, 760);
    area.show();
    QApplication::processEvents();
    auto* editor = area.findChild<ExportEditor*>();
    const auto payload = [&](std::vector<std::variant<document::AssetId, document::CompositionId>> items) {
        auto* mime = new QMimeData;
        mime->setData(kLayerItemsMimeType, layerItemsMimePayload(session, items));
        return mime;
    };
    const auto dropAt = [&](QMimeData* mime, const QPoint where) {
        QDragEnterEvent enter(where, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor, &enter);
        QDropEvent drop(where, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor, &drop);
        QApplication::processEvents();
        return std::pair{enter.isAccepted(), drop.isAccepted()};
    };
    const auto idsOf = [](const std::vector<document::CompositionId>& ids) {
        std::string text;
        for (const auto id : ids)
            text += " " + std::to_string(id.value());
        return text;
    };

    // One composition, onto the empty list: the page lights up while dragging, then one job.
    {
        std::unique_ptr<QMimeData> mime(payload({comps[1]}));
        QDragEnterEvent enter(QPoint(40, 60), Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor, &enter);
        context.expect(enter.isAccepted() && editor->dropIndicatorShown(),
                       "dragging a composition over the empty list lights the list");
        QDragLeaveEvent leave;
        QApplication::sendEvent(editor, &leave);
        context.expect(!editor->dropIndicatorShown(), "leaving takes the highlight away");
        port.calls.clear();
        const auto [entered, dropped] = dropAt(mime.get(), QPoint(40, 60));
        context.expect(entered && dropped && port.calls == std::vector<std::string>{"addJobs " + std::to_string(comps[1].value())},
                       "dropping one composition adds one job");
        context.expect(editor->jobRows().size() == 1 && editor->selectedJob().has_value(),
                       "the new job is shown and selected");
        context.expect(!editor->dropIndicatorShown(), "the highlight is gone after the drop");
    }
    // Three compositions, in the order they were selected, are one command.
    {
        port.publish([](host::ExportQueueSnapshot& snapshot) { snapshot.jobs.clear(); });
        adapter.drain();
        port.calls.clear();
        std::unique_ptr<QMimeData> mime(payload({comps[2], comps[0], comps[1]}));
        (void)dropAt(mime.get(), QPoint(40, 60));
        context.expect(port.calls == std::vector<std::string>{"addJobs" + idsOf({comps[2], comps[0], comps[1]})},
                       "dropping three compositions is one command, in selection order");
        const auto snap = port.snapshot();
        context.expect(snap->jobs.size() == 3 && snap->jobs[0].record.composition == comps[2] &&
                           snap->jobs[1].record.composition == comps[0] &&
                           snap->jobs[2].record.composition == comps[1],
                       "the jobs are in the order of the drag");
        context.expect(editor->jobRows().size() == 3 && editor->selectedJob() == snap->jobs[0].id,
                       "all three are shown and the first is selected");
    }
    // Footage in the selection is ignored; footage alone is refused.
    {
        port.calls.clear();
        std::unique_ptr<QMimeData> mixed(payload({comps[0], asset, comps[2]}));
        (void)dropAt(mixed.get(), QPoint(40, 700));
        context.expect(port.calls == std::vector<std::string>{"addJobs" + idsOf({comps[0], comps[2]}) + " @3"},
                       "a mixed selection adds only its compositions");
        port.calls.clear();
        std::unique_ptr<QMimeData> footage(payload({asset}));
        const auto [entered, dropped] = dropAt(footage.get(), QPoint(40, 60));
        context.expect(!entered && !dropped && port.calls.empty(),
                       "footage alone is not accepted: the pointer shows not allowed");
        QDragEnterEvent over(QPoint(40, 60), Qt::CopyAction, footage.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor, &over);
        context.expect(!editor->dropIndicatorShown(), "nothing droppable shows no indicator");
    }
    // Between jobs: the line shows where the drop lands, and the jobs go there.
    {
        port.publish([&](host::ExportQueueSnapshot& snapshot) {
            snapshot.jobs = {makeJob(1, "One", comps[0], {makeOutput(11)}), makeJob(2, "Two", comps[0], {makeOutput(21)}),
                             makeJob(3, "Three", comps[0], {makeOutput(31)})};
        });
        adapter.drain();
        QApplication::processEvents();
        auto* list = area.findChild<QScrollArea*>("exportJobList");
        const auto rows = editor->jobRows();
        context.expect(rows.size() == 3, "three jobs are listed");
        const auto toEditor = [&](const int listY) { return editor->mapFrom(list->widget(), QPoint(40, listY)); };
        std::unique_ptr<QMimeData> mime(payload({comps[1], comps[2]}));
        // Just below the first job's centre line: after job one.
        const auto between = toEditor(rows[0]->geometry().bottom());
        context.expect(editor->dropIndexAt(between) == 1, "the pointer between jobs one and two means index 1");
        QDragEnterEvent enter(between, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor, &enter);
        context.expect(editor->dropIndicatorShown() && editor->dropIndicatorY() == rows[1]->geometry().top(),
                       "a line shows at the top of job two");
        const auto afterAll = toEditor(rows[2]->geometry().bottom() + 40);
        context.expect(editor->dropIndexAt(afterAll) == 3, "below the last job means the end");
        const auto beforeAll = toEditor(rows[0]->geometry().top());
        context.expect(editor->dropIndexAt(beforeAll) == 0, "above the first job means the start");
        port.calls.clear();
        QDropEvent drop(between, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor, &drop);
        context.expect(port.calls == std::vector<std::string>{"addJobs" + idsOf({comps[1], comps[2]}) + " @1"},
                       "dropping between jobs inserts at that position");
        const auto snap = port.snapshot();
        context.expect(snap->jobs.size() == 5 && snap->jobs[0].id.value() == 1 && snap->jobs[1].record.composition == comps[1] &&
                           snap->jobs[2].record.composition == comps[2] && snap->jobs[3].id.value() == 2,
                       "the new jobs sit between the old ones");
        context.expect(!editor->dropIndicatorShown(), "the line goes away after the drop");
    }
    bridge.beginShutdown();
    timer.restart();
    while (!scheduler.isQuiescent() && timer.elapsed() < 5000)
        QTest::qWait(10);
}

void addJobsIsOneUndo(TestContext& context) {
    Rig rig(false);
    ExportQueueEdits edits(
        rig.session,
        [] {
            host::ExportDefaultPreset preset;
            preset.presetId = "builtin:h264-mp4";
            preset.destination = host::defaultExportDestination(preset.settings.format);
            return preset;
        },
        [](const std::string& id) { return std::optional(ExportPresetChoice{id, {}}); });
    const auto jobs = [&] { return rig.session.snapshot().project().exportQueue().jobs; };
    context.expect(edits.addJob(rig.composition()).accepted && jobs().size() == 1, "a first job exists");
    const auto first = jobs()[0].id;
    const auto result = edits.addJobs({rig.composition(), rig.composition(), rig.composition()}, std::size_t{0});
    context.expect(result.accepted && result.addedJobs.size() == 3 && jobs().size() == 4,
                   "three compositions add three jobs and report them in order");
    context.expect(jobs()[0].id == result.addedJobs[0] && jobs()[2].id == result.addedJobs[2] && jobs()[3].id == first,
                   "they are placed at the index given");
    (void)rig.session.undo();
    context.expect(jobs().size() == 1 && jobs()[0].id == first, "one undo removes all three");
    (void)rig.session.redo();
    context.expect(jobs().size() == 4 && jobs()[0].id == result.addedJobs[0], "one redo brings them back");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("BloomExportPanelTests"));
    QApplication::setApplicationName(QStringLiteral("ExportPanel"));
    QStandardPaths::setTestModeEnabled(true);
    kit::installKinetikTheme(application);
    TestContext context;
    adapterTests(context);
    panelsNeverPump(context);
    emptyState(context);
    listStructure(context);
    contextualButton(context);
    startAllAndPrimary(context);
    statusWording(context);
    waitingForViewer(context);
    progressInTheRow(context);
    replacePromptInline(context);
    visibleReasons(context);
    availabilityReasons(context);
    noRawCodes(context);
    resultsInPlainWords(context);
    formatSettings(context);
    rangeFields(context);
    addOutputWithFormat(context);
    editsAreOneCommand(context);
    destinationFields(context);
    platformNotSupported(context);
    logAndVerification(context);
    narrowAndWide(context);
    emptyAndAdd(context);
    dragFromAssets(context);
    dropManyFromAssets(context);
    addJobsIsOneUndo(context);
    jobsLink(context);
    addingRevealsAndSelects(context);
    editsAreUndoable(context);
    menusAndAssets(context);
    return context.ok ? 0 : 1;
}
