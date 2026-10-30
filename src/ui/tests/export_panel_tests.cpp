// The Export panel (ADR 0033) as After Effects' Render Queue, from hand-built snapshots: the
// adapter's single drain and its sounds, the render list's columns and order, the Render check box
// and Start All, the Render Settings and Output Module links and their dialogs (OK is one edit,
// Cancel nothing), + and −, the ▾ quick picks, the replace question, drag to reorder, drops from
// Assets, the Current Render strip, Details…, plain words everywhere, and the narrow layout.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#include "asset_drop.hpp"
#include "export_job_details.hpp"
#include "export_job_lines.hpp"
#include "export_output_module_dialog.hpp"
#include "export_panel_text.hpp"
#include "export_render_list.hpp"
#include "export_render_settings_dialog.hpp"
#include "export_test_support.hpp"
#include "window_fixture.hpp"

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/export_editor.hpp>
#include <bloom/ui/export_queue_edits.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/progress_bar.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_monitor_model.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/ui_feedback.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
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

[[nodiscard]] document::ExportJobId J(const std::uint64_t id) { return document::ExportJobId::fromRaw(id); }
[[nodiscard]] document::ExportOutputId O(const std::uint64_t id) { return document::ExportOutputId::fromRaw(id); }

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

    explicit Rig(const bool withEditor = true, const QSize size = QSize(900, 760)) {
        auto fakePlayer = std::make_unique<FakePlayer>();
        auto fakeNotifier = std::make_unique<FakeNotifier>();
        player = fakePlayer.get();
        notifier = fakeNotifier.get();
        feedback = std::make_unique<UiFeedback>(std::move(fakePlayer), std::move(fakeNotifier),
                                                [this] { return active; });
        adapter = std::make_unique<ExportQueueAdapter>(port, feedback.get());
        if (withEditor)
            open(size);
    }
    void open(const QSize size = QSize(900, 760)) {
        (void)registerExportEditor(registry, session, adapter.get());
        area = std::make_unique<EditorArea>(registry, "bloom.export", QString{});
        area->resize(size);
        area->show();
        QApplication::processEvents();
        editor = area->findChild<ExportEditor*>();
    }
    [[nodiscard]] document::CompositionId composition() const { return session.compositionId(); }
    template <typename T> T* find(const char* name) const {
        return area->findChild<T*>(QString::fromLatin1(name));
    }
    [[nodiscard]] ep::RenderList* list() const { return editor->renderList(); }
    [[nodiscard]] QTreeWidgetItem* item(const std::uint64_t job) const { return editor->jobItem(J(job)); }
    [[nodiscard]] QString cell(const std::uint64_t job, const int column) const {
        auto* row = item(job);
        return row != nullptr ? row->text(column) : QString{};
    }
    // Opens a job, so its lines show.
    void openJob(const std::uint64_t job) const {
        editor->setJobExpanded(J(job), true);
        QApplication::processEvents();
    }
    // The texts the artist can read right now, in the panel and in any dialog over it.
    [[nodiscard]] QStringList visibleTexts() const {
        QStringList texts;
        for (auto* label : area->findChildren<QLabel*>())
            if (label->isVisibleTo(label->window()) && label->window()->isVisible() && !label->text().isEmpty())
                texts.push_back(label->text());
        for (auto* button : area->findChildren<kit::KButton*>())
            if (button->isVisibleTo(button->window()) && button->window()->isVisible())
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

host::ExportJobView jobIn(Rig& rig, const std::uint64_t id, const host::ExportJobState state,
                          const std::uint64_t output = 0) {
    auto job = makeJob(id, "Shot " + std::to_string(id), rig.composition(),
                       {makeOutput(output != 0 ? output : id * 10 + 1)});
    job.state = state;
    return job;
}

// The dialog of type T the panel just opened over its window, or nullptr.
template <typename T> T* openDialog(const Rig& rig) {
    QApplication::processEvents();
    for (auto* dialog : rig.area->findChildren<T*>())
        if (dialog->isVisible())
            return dialog;
    return nullptr;
}

kit::KDialog* openPrompt(const Rig& rig, const char* name) {
    QApplication::processEvents();
    for (auto* dialog : rig.area->findChildren<kit::KDialog*>(QString::fromLatin1(name)))
        if (dialog->isVisible())
            return dialog;
    return nullptr;
}

QStringList actionTexts(const QMenu& menu, const bool sections = false) {
    QStringList texts;
    for (auto* action : menu.actions())
        if (action->isSeparator() ? sections && !action->text().isEmpty() : true)
            if (!action->text().isEmpty())
                texts.push_back(action->text());
    return texts;
}

QAction* actionNamed(const QMenu& menu, const QString& text) {
    for (auto* action : menu.actions())
        if (action->text() == text || action->objectName() == text)
            return action;
    return nullptr;
}

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
    // The Output Module dialog's OK: a new format, its settings, the overwrite rule and the import
    // flag are one edit, so one undo puts every one of them back.
    const auto before = jobs()[0].outputs[0];
    auto settings = before.settings;
    settings.format = document::ExportFormat::Png;
    settings.useCachedFrames = false;
    auto destination = before.destination;
    destination.overwrite = document::ExportOverwritePolicy::Replace;
    context.expect(formats.editOutput(output, outputEdit([&](auto& edit) {
                       edit.presetId = std::string("builtin:png-sequence");
                       edit.settings = settings;
                       edit.destination = destination;
                       edit.importIntoAssets = true;
                   })).accepted,
                   "the Output Module dialog's changes are accepted as one edit");
    const auto after = jobs()[0].outputs[0];
    context.expect(after.presetId == "builtin:png-sequence" && after.settings.format == document::ExportFormat::Png &&
                       !after.settings.useCachedFrames &&
                       after.destination.overwrite == document::ExportOverwritePolicy::Replace &&
                       after.importIntoAssets,
                   "every change of the dialog is in the project");
    (void)rig.session.undo();
    context.expect(jobs()[0].outputs[0] == before, "one undo reverts the whole Output Module edit");
    // Render Settings' OK is one edit of the range.
    context.expect(edits.editJob(job, jobEdit([](auto& edit) {
                       edit.range = document::ExportRange{document::ExportRangeKind::Custom, 10, 20};
                   })).accepted &&
                       jobs()[0].render.range.kind == document::ExportRangeKind::Custom,
                   "the Render Settings range is one edit");
    (void)rig.session.undo();
    context.expect(jobs()[0].render.range.kind == document::ExportRangeKind::WorkArea, "and one undo");
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
        context.expect(editor->jobOrder().size() == 1 && editor->selectedJob().has_value(),
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
        context.expect(editor->jobOrder().size() == 3 && editor->selectedJob() == snap->jobs[0].id,
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
        auto* list = editor->renderList();
        context.expect(list->topLevelItemCount() == 3, "three jobs are listed");
        const auto rect = [&](const int row) { return list->visualItemRect(list->topLevelItem(row)); };
        const auto toEditor = [&](const int listY) { return list->viewport()->mapTo(editor, QPoint(40, listY)); };
        std::unique_ptr<QMimeData> mime(payload({comps[1], comps[2]}));
        // Just below the first job's centre line: after job one.
        const auto between = toEditor(rect(0).bottom());
        context.expect(editor->dropIndexAt(between) == 1, "the pointer between jobs one and two means index 1");
        QDragEnterEvent enter(between, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor, &enter);
        context.expect(editor->dropIndicatorShown() && std::abs(editor->dropIndicatorY() - rect(1).top()) <= kit::px(kit::Spacing::XXS),
                       "a line shows at the top of job two");
        const auto afterAll = toEditor(rect(2).bottom() + 40);
        context.expect(editor->dropIndexAt(afterAll) == 3, "below the last job means the end");
        const auto beforeAll = toEditor(rect(0).top());
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


// ---------------------------------------------------------------------------------------------
// The render list

void columnsAndOrder(TestContext& context) {
    Rig rig;
    context.expect(rig.editor != nullptr, "the Export editor is created by the registry");
    rig.threeJobs();
    auto* list = rig.list();
    QStringList titles;
    for (int column = 0; column < ep::ActionsColumn; ++column)
        titles.push_back(list->headerItem()->text(column));
    context.expect(titles == QStringList{"Render", "#", "Composition", "Status", "Started", "Render Time"},
                   "the columns are Render, #, Composition, Status, Started and Render Time, in that order");
    context.expect(!list->header()->isHidden(), "the list has real column headers");
    context.expect(rig.editor->jobOrder() == std::vector{J(1), J(2), J(3)}, "the rows are in queue order");
    context.expect(rig.cell(1, ep::NumberColumn) == "1" && rig.cell(3, ep::NumberColumn) == "3",
                   "# is the queue order");
    context.expect(rig.cell(1, ep::CompositionColumn) == "Shot 010", "Composition names the job");
    context.expect(rig.editor->statusCell(J(1))->text() == "Rendering 50%", "a rendering job reads its percent");
    context.expect(rig.editor->statusCell(J(2))->text() == "Queued", "a waiting job reads Queued");
    context.expect(rig.editor->statusCell(J(3))->text() == "Failed", "a failed job reads Failed");
    context.expect(rig.editor->statusCell(J(1))->barShown() && !rig.editor->statusCell(J(2))->barShown() &&
                       !rig.editor->statusCell(J(3))->barShown(),
                   "only the rendering job has a bar, in its Status cell");
    context.expect(rig.area->findChildren<kit::KColorChip*>().isEmpty(), "there is no label color");
    bool onlyChecks = true;
    for (auto* toggle : rig.area->findChildren<kit::KSwitch*>())
        onlyChecks = onlyChecks && qobject_cast<kit::KCheckBox*>(toggle) != nullptr;
    context.expect(onlyChecks, "there are no switches, only the Render check boxes");
    // The check box is centred in its cell, after the row's caret.
    auto* check = rig.editor->renderCheck(J(1));
    const auto box = check->box()->geometry().center().x();
    context.expect(std::abs(box - check->width() / 2) <= 1, "the Render check box is centered in its column");
    // Every row's text starts under its column title.
    const auto header = list->header();
    for (const int column : {ep::NumberColumn, ep::CompositionColumn})
        context.expect(list->visualItemRect(rig.item(1)).isValid() &&
                           list->visualRect(list->model()->index(list->indexOfTopLevelItem(rig.item(1)), column))
                                   .left() ==
                               header->sectionViewportPosition(column),
                       "a cell lines up with its column title");
    // A job opens to Render Settings and one Output Module line per output, folded by default.
    context.expect(!rig.item(1)->isExpanded(), "a job starts folded");
    rig.openJob(1);
    auto* settings = rig.editor->renderSettingsLine(J(1));
    auto* module = rig.editor->outputLine(J(1), O(11));
    context.expect(settings != nullptr && settings->isVisibleTo(rig.area.get()) &&
                       settings->link()->text().contains("Work area"),
                   "an open job shows its Render Settings line with the range as a link");
    context.expect(module != nullptr && module->moduleLink()->text() == "H.264 MP4" &&
                       module->destinationLink()->text() == "renders/out.mp4",
                   "the Output Module line names the format and Output To where it saves");
    context.expect(settings->link()->variant() == kit::KButton::Variant::Link &&
                       module->moduleLink()->variant() == kit::KButton::Variant::Link &&
                       module->destinationLink()->variant() == kit::KButton::Variant::Link,
                   "the values are kit links");
    // The labels share one right-aligned column, so the values start at one x.
    const auto valueX = [&](QWidget* widget) { return widget->mapTo(list->viewport(), QPoint(0, 0)).x(); };
    context.expect(valueX(settings->picks()) == valueX(module->presetsButton()),
                   "Render Settings and Output Module values start at the same x");
    if (valueX(settings->picks()) != list->valueLeft())
        std::fprintf(stderr, "values at %d, list says %d, line at %d\n", valueX(settings->picks()), list->valueLeft(),
                     valueX(settings));
    context.expect(valueX(settings->picks()) == list->valueLeft(), "where the list says values start");
    // A line's disclosure shows what it comes to.
    rig.item(1)->child(0)->setExpanded(true);
    QApplication::processEvents();
    auto* summary = rig.editor->summaryLine(rig.item(1)->child(0));
    context.expect(summary != nullptr && summary->isVisibleTo(rig.area.get()) &&
                       summary->text().contains("240 frames"),
                   "Render Settings opens to its frames");
}

void renderCheckAndStartAll(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        snapshot.jobs = {jobIn(rig, 1, host::ExportJobState::Ready), jobIn(rig, 2, host::ExportJobState::Ready)};
    });
    auto* startAll = rig.find<kit::KButton>("exportStartAllButton");
    context.expect(startAll->isEnabled() && startAll->variant() == kit::KButton::Variant::Primary,
                   "Start All is the Primary while a job is ready");
    context.expect(rig.visiblePrimaries() <= 1, "one Primary at most");
    auto* check = rig.editor->renderCheck(J(1))->box();
    context.expect(check->isChecked(), "a job is checked: Start All includes it");
    rig.port.calls.clear();
    QTest::mouseClick(check, Qt::LeftButton);
    context.expect(rig.port.calls == std::vector<std::string>{"editJob 1 enabled=0"},
                   "unchecking Render is one edit of Include in Start All");
    rig.publish([](host::ExportQueueSnapshot& snapshot) {
        for (auto& job : snapshot.jobs) {
            job.record.enabled = false;
        }
    });
    context.expect(!startAll->isEnabled() && startAll->variant() != kit::KButton::Variant::Primary,
                   "with every job unchecked Start All waits and is not the Primary");
    context.expect(!startAll->toolTip().isEmpty(), "and says why");
    rig.publish([](host::ExportQueueSnapshot& snapshot) { snapshot.jobs[1].record.enabled = true; });
    context.expect(startAll->isEnabled() && startAll->variant() == kit::KButton::Variant::Primary,
                   "one checked job makes Start All the Primary again");
    rig.port.calls.clear();
    QTest::mouseClick(startAll, Qt::LeftButton);
    context.expect(rig.port.calls == std::vector<std::string>{"startAll"}, "Start All starts the queue");
    // Stop All shows only while something renders.
    context.expect(rig.find<kit::KButton>("exportStopAllButton")->property("chromeSuppressed").toBool(),
                   "Stop All is hidden while nothing renders");
}

void renderSettingsDialog(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) { snapshot.jobs = {jobIn(rig, 1, host::ExportJobState::Ready)}; });
    rig.openJob(1);
    rig.port.calls.clear();
    QTest::mouseClick(rig.editor->renderSettingsLine(J(1))->link(), Qt::LeftButton);
    auto* dialog = openDialog<ep::RenderSettingsDialog>(rig);
    context.expect(dialog != nullptr, "the Render Settings link opens the Render Settings dialog");
    if (dialog == nullptr)
        return;
    context.expect(dialog->windowTitle() == "Render Settings", "it is called Render Settings");
    auto* group = dialog->findChild<kit::KSection*>("exportRenderSettingsComposition");
    context.expect(group != nullptr && group->title() == QStringLiteral("Composition “Main”"),
                   "its group names the composition, as After Effects does");
    context.expect(dialog->startText().startsWith("0:00:00:00") && dialog->durationText().contains("240 frames"),
                   "it reads where the range starts and how long it is");
    context.expect(!dialog->startField()->isVisibleTo(dialog), "a work area has no frame fields");
    // Cancel changes nothing.
    dialog->rangeKind()->setCurrentIndex(dialog->rangeKind()->findData(static_cast<int>(document::ExportRangeKind::Custom)));
    dialog->button(kit::KDialog::Role::Reject)->click();
    QApplication::processEvents();
    context.expect(rig.port.calls.empty(), "Cancel changes nothing");
    // A custom range, with its timecode, is one edit on OK.
    rig.editor->openRenderSettings(J(1));
    dialog = openDialog<ep::RenderSettingsDialog>(rig);
    if (dialog == nullptr)
        return;
    dialog->rangeKind()->setCurrentIndex(dialog->rangeKind()->findData(static_cast<int>(document::ExportRangeKind::Custom)));
    context.expect(dialog->startField()->isVisibleTo(dialog) && dialog->endField()->isVisibleTo(dialog),
                   "Custom shows start and end frame fields");
    dialog->startField()->setIntegerValue(24);
    dialog->endField()->setIntegerValue(47);
    context.expect(dialog->startText() == "0:00:01:00" && dialog->durationText().contains("24 frames"),
                   "the fields read as timecode and a duration");
    dialog->endField()->setIntegerValue(10);
    context.expect(!dialog->button(kit::KDialog::Role::Accept)->isEnabled(),
                   "an end before the start is refused under its field");
    dialog->endField()->setIntegerValue(47);
    dialog->button(kit::KDialog::Role::Accept)->click();
    QApplication::processEvents();
    context.expect(rig.port.calls == std::vector<std::string>{"editJob 1 range=3:24-47"},
                   "OK writes the range as one edit");
    // One frame hides the end.
    rig.editor->openRenderSettings(J(1));
    dialog = openDialog<ep::RenderSettingsDialog>(rig);
    if (dialog == nullptr)
        return;
    dialog->rangeKind()->setCurrentIndex(dialog->rangeKind()->findData(static_cast<int>(document::ExportRangeKind::Frame)));
    context.expect(!dialog->endShown() && dialog->durationText().contains("1 frame"), "One frame has no end");
    dialog->button(kit::KDialog::Role::Reject)->click();
    QApplication::processEvents();
}

void outputModuleDialog(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto job = makeJob(1, "Shot 010", rig.composition(),
                           {makeOutput(11), makeOutput(12, "builtin:prores-422hq-mov", document::ExportFormat::ProResMov,
                                                       "Exports/Shot 010.mov")});
        snapshot.jobs = {job};
    });
    rig.openJob(1);
    rig.port.calls.clear();
    QTest::mouseClick(rig.editor->outputLine(J(1), O(12))->moduleLink(), Qt::LeftButton);
    auto* dialog = openDialog<ep::OutputModuleDialog>(rig);
    context.expect(dialog != nullptr, "the Output Module link opens the Output Module dialog");
    if (dialog == nullptr)
        return;
    context.expect(dialog->mainSection()->title() == QStringLiteral("“ProRes 422 HQ”"),
                   "its first group names the preset");
    context.expect(!rig.visibleTexts().join('\n').contains("olour"),
                   "visible copy is American English (Color, not Color)");
    // Grouped formats, video first.
    QStringList items;
    for (int index = 0; index < dialog->format()->count(); ++index)
        items.push_back(dialog->format()->itemText(index));
    context.expect(items.size() >= 2 && items.front() == "Video" && !dialog->format()->isItemEnabled(0) &&
                       items.indexOf("Image sequences") > items.indexOf("ProRes 422 HQ"),
                   "formats are grouped, video first, then image sequences");
    // ProRes: a profile, display and view, audio; no compression, colour space or numbering.
    context.expect(dialog->shows(dialog->profile()) && !dialog->shows(dialog->compression()) &&
                       dialog->shows(dialog->display()) && dialog->shows(dialog->view()) &&
                       !dialog->shows(dialog->colourSpace()) && dialog->shows(dialog->includeAudio()) &&
                       dialog->shows(dialog->bakeLook()) && !dialog->shows(dialog->startNumber()),
                   "ProRes shows only what applies to it");
    context.expect(dialog->shows(dialog->cachedFrames()) && dialog->shows(dialog->importIntoAssets()) &&
                       dialog->shows(dialog->overwrite()),
                   "cached frames, post-render and If a file is there are always there");
    // Choosing OpenEXR takes its settings and shows what applies to it.
    dialog->format()->chooseIndex(dialog->format()->findData(QStringLiteral("builtin:exr-sequence")));
    QApplication::processEvents();
    context.expect(dialog->mainSection()->title() == QStringLiteral("“OpenEXR sequence”"),
                   "the group follows the chosen preset");
    context.expect(dialog->shows(dialog->compression()) && dialog->shows(dialog->colourSpace()) &&
                       !dialog->shows(dialog->display()) && !dialog->shows(dialog->includeAudio()) &&
                       dialog->shows(dialog->startNumber()) && dialog->shows(dialog->padding()),
                   "OpenEXR shows compression, color space and numbering");
    dialog->importIntoAssets()->setChecked(true);
    dialog->overwrite()->setCurrentIndex(
        dialog->overwrite()->findData(static_cast<int>(document::ExportOverwritePolicy::Replace)));
    dialog->button(kit::KDialog::Role::Accept)->click();
    QApplication::processEvents();
    context.expect(rig.port.calls ==
                       std::vector<std::string>{"editOutput 1 12 preset=builtin:exr-sequence settings "
                                                "destination=Exports/Shot 010.mov policy=2 import=1"},
                   "OK writes every change as one edit");
    // Cancel changes nothing.
    rig.port.calls.clear();
    dialog = rig.editor->openOutputModule(J(1), O(11));
    QApplication::processEvents();
    if (dialog == nullptr)
        return;
    dialog->cachedFrames()->setChecked(false);
    dialog->button(kit::KDialog::Role::Reject)->click();
    QApplication::processEvents();
    context.expect(rig.port.calls.empty(), "Cancel changes nothing");
    // OK with nothing changed writes nothing.
    dialog = rig.editor->openOutputModule(J(1), O(11));
    QApplication::processEvents();
    if (dialog != nullptr)
        dialog->button(kit::KDialog::Role::Accept)->click();
    QApplication::processEvents();
    context.expect(rig.port.calls.empty(), "OK without a change writes nothing");
}

void everyFormatIsOffered(TestContext& context) {
    Rig rig;
    auto catalogue = makeCatalogue();
    auto& hevc = catalogue->presets.emplace_back(makePreset("builtin:hevc-mp4", "HEVC MP4",
                                                            host::ExportPresetCategory::Video,
                                                            document::ExportFormat::HevcMp4));
    hevc.platformAvailability.available = false;
    hevc.platformAvailability.reason = "HEVC needs a VA-API render node with HEVC encode";
    auto output = makeOutput(11, "builtin:hevc-mp4", document::ExportFormat::HevcMp4, "Exports/a.mp4");
    output.availability.available = false;
    output.availability.reason = "HEVC needs a VA-API render node with HEVC encode";
    output.availability.remedy = host::ExportRemedy::InstallOpenH264;
    ep::OutputModuleDialog dialog(output, catalogue, {}, *rig.editor);
    const int index = dialog.format()->findData(QStringLiteral("builtin:hevc-mp4"));
    context.expect(index >= 0 && dialog.format()->itemText(index) == "HEVC MP4" && dialog.format()->isItemEnabled(index),
                   "a format the engine reports unavailable is listed like any other");
    QStringList texts;
    for (auto* label : dialog.findChildren<QLabel*>())
        texts.push_back(label->text());
    for (auto* button : dialog.findChildren<kit::KButton*>())
        texts.push_back(button->text());
    const auto all = texts.join('\n');
    context.expect(!all.contains("not available") && !all.contains("VA-API") && !all.contains("Install") &&
                       !all.contains("Locate"),
                   "the dialog never says unavailable and never offers to install an encoder");
}

void addAndRemoveOutputs(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        snapshot.jobs = {jobIn(rig, 1, host::ExportJobState::Ready)};
    });
    rig.openJob(1);
    auto* line = rig.editor->outputLine(J(1), O(11));
    context.expect(!line->removeButton()->isEnabled() &&
                       line->removeButton()->toolTip() == "A job keeps at least one output.",
                   "− is disabled on the last output, with the reason");
    rig.port.calls.clear();
    QTest::mouseClick(line->addButton(), Qt::LeftButton);
    QApplication::processEvents();
    context.expect(rig.port.calls == std::vector<std::string>{"addOutput 1"}, "+ adds an output");
    auto* added = rig.editor->outputLine(J(1), O(5000));
    context.expect(added != nullptr && added->isVisibleTo(rig.area.get()),
                   "each output gets its own Output Module line");
    auto* first = rig.editor->outputLine(J(1), O(11));
    context.expect(first != nullptr && first->removeButton()->isEnabled(), "with two outputs − works");
    rig.port.calls.clear();
    QTest::mouseClick(first->removeButton(), Qt::LeftButton);
    context.expect(rig.port.calls == std::vector<std::string>{"removeOutput 1 11"}, "− removes this output");
    // The + − and Output To line up across outputs.
    auto* second = rig.editor->outputLine(J(1), O(5000));
    const auto x = [&](QWidget* widget) { return widget->mapTo(rig.list()->viewport(), QPoint(0, 0)).x(); };
    context.expect(x(first->addButton()) == x(second->addButton()) &&
                       x(first->destinationLink()) == x(second->destinationLink()),
                   "+, − and Output To line up across outputs");
}

void quickPickMenus(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) { snapshot.jobs = {jobIn(rig, 1, host::ExportJobState::Ready)}; });
    rig.openJob(1);
    std::unique_ptr<QMenu> range(rig.editor->renderSettingsLine(J(1))->buildPicks());
    context.expect(actionTexts(*range) == QStringList{"Work area", "Whole composition", "Current frame"},
                   "the range ▾ offers Work area, Whole composition and Current frame");
    context.expect(actionNamed(*range, "Current frame")->isEnabled(), "Current frame works in the open composition");
    rig.port.calls.clear();
    actionNamed(*range, "Whole composition")->trigger();
    context.expect(rig.port.calls == std::vector<std::string>{"editJob 1 range=2:0-0"},
                   "a quick pick is one edit of the range");
    rig.port.calls.clear();
    actionNamed(*range, "Current frame")->trigger();
    context.expect(rig.port.calls == std::vector<std::string>{"editJob 1 range=4:0-0"},
                   "Current frame renders the frame under the playhead");
    auto* line = rig.editor->outputLine(J(1), O(11));
    std::unique_ptr<QMenu> presets(line->buildPresets());
    const auto all = actionTexts(*presets, true);
    context.expect(all.indexOf("Video") == 0 && all.indexOf("Image sequences") > all.indexOf("ProRes 422 HQ") &&
                       all.indexOf("OpenEXR sequence") > all.indexOf("Image sequences"),
                   "the format ▾ lists video first, then image sequences");
    rig.port.calls.clear();
    actionNamed(*presets, "ProRes 422 HQ")->trigger();
    context.expect(rig.port.calls == std::vector<std::string>{"applyPreset 1 11 builtin:prores-422hq-mov"},
                   "picking a format applies its preset");
    std::unique_ptr<QMenu> destinations(line->buildDestinations());
    context.expect(actionTexts(*destinations).front() == "Default name", "Output To ▾ offers Default name first");
    rig.port.calls.clear();
    actionNamed(*destinations, "Default name")->trigger();
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0].starts_with("editOutput 1 11 destination=Exports/"),
                   "Default name puts the default destination back");
    ep::rememberExportFolder(QStringLiteral("/tmp/renders"));
    rig.publish([](host::ExportQueueSnapshot&) {});
    std::unique_ptr<QMenu> recent(rig.editor->outputLine(J(1), O(11))->buildDestinations());
    context.expect(actionNamed(*recent, QDir::toNativeSeparators("/tmp/renders")) != nullptr,
                   "Output To ▾ offers the recent folders");
    rig.port.calls.clear();
    actionNamed(*recent, QDir::toNativeSeparators("/tmp/renders"))->trigger();
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0].find("chosen=/tmp/renders/") != std::string::npos,
                   "a recent folder keeps the default name in that folder");
}

void replaceConfirm(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto job = jobIn(rig, 1, host::ExportJobState::Ready);
        job.outputs[0].preflight.push_back({host::ExportPreflightCode::DestinationExists,
                                            host::ExportPreflightTarget::Destination,
                                            host::ExportPreflightSeverity::NeedsConfirmation,
                                            "out.mp4 already exists."});
        refuse(job.commands, host::ExportCommand::Start, host::ExportUnavailableCode::NeedsReplaceConfirmation,
               "Replace existing files?");
        snapshot.jobs = {job};
    });
    context.expect(rig.editor->statusCell(J(1))->text() == "Waiting for your OK",
                   "a file that is already there reads Waiting for your OK");
    auto* startAll = rig.find<kit::KButton>("exportStartAllButton");
    context.expect(startAll->isEnabled() && startAll->variant() == kit::KButton::Variant::Primary,
                   "Start All stays the Primary: it asks before it replaces");
    rig.port.calls.clear();
    QTest::mouseClick(startAll, Qt::LeftButton);
    auto* prompt = openPrompt(rig, "exportReplaceConfirm");
    context.expect(prompt != nullptr, "Start All asks once before replacing");
    if (prompt == nullptr)
        return;
    auto* heading = prompt->findChild<QLabel*>("exportReplaceConfirmHeading");
    context.expect(heading != nullptr && heading->text() == "Replace out.mp4?", "it names the file");
    context.expect(prompt->button(kit::KDialog::Role::Destructive) != nullptr &&
                       prompt->button(kit::KDialog::Role::Destructive)->text() == "Replace" &&
                       prompt->button(kit::KDialog::Role::Alternate)->text() == "Choose another name" &&
                       prompt->button(kit::KDialog::Role::Reject)->text() == "Cancel",
                   "Replace, Choose another name and Cancel");
    prompt->button(kit::KDialog::Role::Reject)->click();
    QApplication::processEvents();
    context.expect(rig.port.calls.empty(), "Cancel starts nothing and replaces nothing");
    QTest::mouseClick(startAll, Qt::LeftButton);
    prompt = openPrompt(rig, "exportReplaceConfirm");
    if (prompt == nullptr)
        return;
    prompt->button(kit::KDialog::Role::Destructive)->click();
    QApplication::processEvents();
    context.expect(rig.port.calls == std::vector<std::string>{"confirmReplace 1 11", "startAll"},
                   "Replace says yes for the file, then starts");
    // The row's own Start asks the same question.
    rig.port.calls.clear();
    const auto menu = rig.editor->buildJobMenu(J(1));
    auto* start = actionNamed(*menu, "exportStartAction");
    context.expect(start != nullptr && start->isEnabled(), "the row's Start is offered");
    start->trigger();
    prompt = openPrompt(rig, "exportReplaceConfirm");
    context.expect(prompt != nullptr, "and asks before replacing");
    if (prompt != nullptr) {
        prompt->button(kit::KDialog::Role::Destructive)->click();
        QApplication::processEvents();
    }
    context.expect(rig.port.calls == std::vector<std::string>{"confirmReplace 1 11", "start 1"},
                   "then starts that job");
}

void rowMenu(TestContext& context) {
    Rig rig;
    rig.threeJobs();
    const auto menu = rig.editor->buildJobMenu(J(2));
    const auto texts = actionTexts(*menu);
    for (const auto* item : {"Details…", "Duplicate", "Remove", "Move up", "Move down", "Copy as bloom-cli command",
                             "Copy as Python"})
        context.expect(texts.contains(QString::fromUtf8(item)), (std::string("the ⋯ menu offers ") + item).c_str());
    context.expect(texts.indexOf("Duplicate") < texts.indexOf("Remove") &&
                       texts.indexOf("Remove") < texts.indexOf("Move up") &&
                       texts.indexOf("Move down") < texts.indexOf("Copy as bloom-cli command"),
                   "in the After Effects order");
    rig.port.calls.clear();
    actionNamed(*menu, "Move up")->trigger();
    actionNamed(*menu, "Duplicate")->trigger();
    context.expect(rig.port.calls == std::vector<std::string>{"moveJob 2 0", "duplicateJob 2"},
                   "Move up and Duplicate are one command each");
    const auto first = rig.editor->buildJobMenu(J(1));
    context.expect(!actionNamed(*first, "Move up")->isEnabled() && !actionNamed(*first, "Move up")->toolTip().isEmpty(),
                   "the first job cannot move up, and says so");
    actionNamed(*first, "Copy as Python")->trigger();
    context.expect(QGuiApplication::clipboard()->text() == "bloom-cli export-script", "Copy as Python copies the script");
    // The ⋯ button opens it.
    context.expect(rig.area->findChildren<kit::KIconButton*>("exportJobMenuButton").size() == 3,
                   "every row has its ⋯ button");
}

void dragToReorder(TestContext& context) {
    Rig rig;
    rig.threeJobs();
    auto* list = rig.list();
    context.expect(rig.item(1)->flags().testFlag(Qt::ItemIsDragEnabled), "a job row can be picked up");
    const auto toEditor = [&](const int y) { return list->viewport()->mapTo(rig.editor, QPoint(40, y)); };
    const auto drop = [&](const std::uint64_t job, const QPoint where) {
        QMimeData mime;
        mime.setData(ep::kExportJobsMimeType, QByteArray::number(static_cast<qulonglong>(job)));
        QDragEnterEvent enter(where, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(rig.editor, &enter);
        QDragMoveEvent move(where, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(rig.editor, &move);
        const bool line = rig.editor->dropIndicatorShown();
        QDropEvent dropped(where, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(rig.editor, &dropped);
        return enter.isAccepted() && line && dropped.isAccepted();
    };
    rig.port.calls.clear();
    context.expect(drop(3, toEditor(list->visualItemRect(rig.item(1)).top() + 1)),
                   "a job row dragged in the list shows the line and is taken");
    context.expect(rig.port.calls == std::vector<std::string>{"moveJob 3 0"}, "dropping it above the first moves it first");
    rig.port.calls.clear();
    (void)drop(1, toEditor(list->visualItemRect(rig.item(3)).bottom() + 20));
    context.expect(rig.port.calls == std::vector<std::string>{"moveJob 1 2"}, "dropping it at the end moves it last");
    rig.port.calls.clear();
    (void)drop(2, toEditor(list->visualItemRect(rig.item(2)).center().y()));
    context.expect(rig.port.calls.empty(), "dropping it where it is moves nothing");
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
    context.expect(rig.editor->jobOrder().size() == 4, "the dropped composition is a new row");
    QMimeData other;
    other.setText("not a composition");
    QDragEnterEvent refused(where, Qt::CopyAction, &other, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(rig.editor, &refused);
    context.expect(!refused.isAccepted(), "a drag that is not a composition is refused");
}

void currentRenderStrip(TestContext& context) {
    QSettings().remove(QStringLiteral("export/current-render/expanded"));
    Rig rig;
    rig.threeJobs();
    context.expect(rig.editor->currentRenderText() ==
                       QStringLiteral("Rendering Shot 010 · frame 121 of 240 · 0:07 elapsed · about 42 s left"),
                   "the strip says what renders, the frame, the time and what is left");
    context.expect(!rig.editor->currentRenderExpanded() && !rig.shown("exportCurrentRenderBar"),
                   "the strip starts folded to its one line");
    QTest::mouseClick(rig.find<QWidget>("exportCurrentRenderToggle"), Qt::LeftButton);
    auto* bar = rig.find<kit::KProgressBar>("exportCurrentRenderBar");
    context.expect(rig.editor->currentRenderExpanded() && bar->isVisibleTo(rig.area.get()) &&
                       bar->completed() == 120 && bar->total() == 240,
                   "opened, it adds a progress bar");
    rig.publish([](host::ExportQueueSnapshot& snapshot) {
        snapshot.activeJob.reset();
        for (auto& job : snapshot.jobs)
            if (job.state == host::ExportJobState::Rendering)
                job.state = host::ExportJobState::Done;
    });
    context.expect(rig.editor->currentRenderText() == "Idle", "nothing rendering reads Idle");
    rig.editor->setCurrentRenderExpanded(false);
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
    auto* cell = rig.editor->statusCell(J(1));
    context.expect(cell->text() == "Paused while you work in the Viewer", "waiting for the Viewer says so in plain words");
    context.expect(cell->barShown() && cell->bar()->busy(), "waiting for the Viewer shows a busy bar");
    context.expect(!rig.editor->currentRenderText().contains("left") &&
                       rig.editor->currentRenderText().contains("paused while you work in the Viewer"),
                   "the strip says so too, with no time left");
    const auto status = ep::jobStatus(rig.port.snapshot()->jobs[0]);
    context.expect(ep::toneColor(status.tone) == kit::Color::Muted, "waiting for the Viewer is neutral ink");
}

void statusWording(TestContext& context) {
    using State = host::ExportJobState;
    Rig rig(false);
    const auto statusOf = [&](const State state, const host::ExportDeviceRoute route = host::ExportDeviceRoute::None) {
        auto job = jobIn(rig, 1, state);
        job.progress.route = route;
        job.progress.fraction = 0.25;
        if (state == State::Failed)
            job.failure = host::ExportFailure{.reason = "boom", .summary = "It failed."};
        return ep::jobStatus(job);
    };
    context.expect(statusOf(State::Ready).text == "Queued", "a ready job is Queued");
    context.expect(statusOf(State::Waiting).text == "Queued", "a job waiting its turn is Queued");
    context.expect(statusOf(State::Rendering).text == "Rendering 25%", "Rendering 25%");
    context.expect(statusOf(State::Rendering, host::ExportDeviceRoute::WaitingForViewer).text ==
                       "Paused while you work in the Viewer",
                   "waiting for the Viewer is a calm plain sentence");
    context.expect(statusOf(State::Done).text == "Done", "Done");
    context.expect(statusOf(State::Failed).text == "Failed", "Failed");
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
    context.expect(refused(host::ExportUnavailableCode::NeedsReplaceConfirmation, false) == "Waiting for your OK",
                   "a replace question reads Waiting for your OK");
    context.expect(refused(host::ExportUnavailableCode::PreflightBlocked, true) == "Needs output",
                   "a missing destination reads Needs output");
    for (int value = 1; value <= 12; ++value) {
        auto job = jobIn(rig, 1, static_cast<State>(value));
        if (job.state == State::Failed)
            job.failure = host::ExportFailure{.reason = "boom"};
        const auto status = ep::jobStatus(job);
        context.expect(!status.text.isEmpty(), "every job state has status text");
        const auto ink = ep::toneColor(status.tone);
        context.expect(ink != kit::Color::Warn && ink != kit::Color::Ok, "no status is amber or green");
        context.expect((ink == kit::Color::Error) == (job.state == State::Failed), "only Failed is red");
    }
}

void detailsDialog(TestContext& context) {
    Rig rig;
    rig.threeJobs();
    rig.port.logs[3] = {{.sequence = 1, .sinceStart = std::chrono::milliseconds(1000), .code = "frame.ok",
                         .message = "Frame 1 written"},
                        {.sequence = 2, .sinceStart = std::chrono::milliseconds(9000), .code = "verify.frames",
                         .message = "Frame count differs"}};
    rig.publish([](host::ExportQueueSnapshot& snapshot) { snapshot.jobs[2].lastLogSequence = 2; });
    const auto ready = rig.editor->buildJobMenu(J(2));
    context.expect(!actionNamed(*ready, "Details…")->isEnabled(), "a job that has not run has no details yet");
    const auto menu = rig.editor->buildJobMenu(J(3));
    auto* details = actionNamed(*menu, "Details…");
    context.expect(details != nullptr && details->isEnabled(), "a failed job offers Details…");
    details->trigger();
    auto* dialog = openDialog<ep::JobDetailsDialog>(rig);
    context.expect(dialog != nullptr, "Details… opens a dialog");
    if (dialog == nullptr)
        return;
    context.expect(dialog->summaryText().startsWith("Couldn't save Shot 030."), "it says what happened in a sentence");
    context.expect(dialog->lineTexts().join('\n').contains("The file has the wrong number of frames (expected 240, found 239)"),
                   "the check of the file is a plain line with what was expected and found");
    context.expect(dialog->logText().contains("Frame count differs"), "the log is there");
    auto* copy = dialog->button(kit::KDialog::Role::Apply);
    context.expect(copy != nullptr && copy->text() == "Copy details", "Copy details copies it for a bug report");
    copy->click();
    context.expect(QGuiApplication::clipboard()->text().contains("Frame count differs"), "Copy details copies the log");
    dialog->button(kit::KDialog::Role::Accept)->click();
    QApplication::processEvents();
}

void noRawCodes(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto job = makeJob(1, "Shot 010", rig.composition(), {makeOutput(11)});
        job.state = host::ExportJobState::Failed;
        job.run = host::ExportRunLink{.run = 1};
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
        job.outputs[0].record.destination.path = "Exports/{name}.<ext>";
        snapshot.jobs = {job};
    });
    rig.openJob(1);
    for (int child = 0; child < rig.item(1)->childCount(); ++child)
        rig.item(1)->child(child)->setExpanded(true);
    auto* dialog = rig.editor->openDetails(J(1));
    QApplication::processEvents();
    auto visible = rig.visibleTexts();
    for (auto* widget : rig.area->findChildren<QWidget*>())
        if (widget->isVisibleTo(widget->window()) && widget->window()->isVisible())
            visible.push_back(widget->toolTip());
    const auto all = visible.join('\n');
    context.expect(all.contains("OpenEXR"), "the failure line is a sentence with a remedy");
    for (const auto* raw : {"stage=", "code=", "facet", "output.analysis", "export.", "0x", "digest", "Digest",
                            "CPU reference", "preflight", "publication", "verification:", "{name}", "<ext>",
                            "builtin:"})
        context.expect(!all.contains(QString::fromLatin1(raw)), (std::string("no raw word in view: ") + raw).c_str());
    context.expect(dialog != nullptr && dialog->detailsText().contains("facet=3"),
                   "the raw detail goes only into Copy details");
    if (dialog != nullptr)
        dialog->close();
}

void emptyState(TestContext& context) {
    Rig rig;
    rig.publish([](host::ExportQueueSnapshot&) {});
    const auto texts = rig.visibleTexts();
    context.expect(texts.contains("Nothing to render yet.") && texts.contains("Or drag compositions here from Assets."),
                   "the empty list says so and how to add one");
    context.expect(rig.shown("exportAddActiveButton") && rig.visiblePrimaries() == 1,
                   "Add composition is the one Primary of the empty panel");
    context.expect(!rig.shown("exportRenderList"), "there is no list to read yet");
    QMimeData mime;
    mime.setData(kCompositionMimeType, QByteArray::number(static_cast<qulonglong>(rig.composition().value())));
    QDragEnterEvent enter(QPoint(60, 90), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(rig.editor, &enter);
    QDropEvent drop(QPoint(60, 90), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(rig.editor, &drop);
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0].starts_with("addJobs"),
                   "a drop on the empty list adds a job");
}

void narrowAndWide(TestContext& context) {
    Rig rig(true, QSize(2000, 760));
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        fillThreeJobs(snapshot, rig.composition());
        snapshot.jobs[1].outputs.push_back(makeOutput(22, "builtin:exr-sequence", document::ExportFormat::OpenExr,
                                                      "Exports/Shot 020.####.exr"));
        snapshot.jobs[1].record.outputs.push_back(snapshot.jobs[1].outputs.back().record);
    });
    rig.openJob(2);
    auto* header = rig.list()->header();
    context.expect(!header->isSectionHidden(ep::StartedColumn) && !header->isSectionHidden(ep::RenderTimeColumn),
                   "a wide panel shows every column");
    context.expect(header->sectionSize(ep::CompositionColumn) < 600 && header->sectionSize(ep::StatusColumn) < 600,
                   "columns keep the width of their content on a wide panel");
    context.expect(!rig.editor->outputLine(J(2), O(21))->folded(), "a wide panel keeps an output on one line");
    rig.area->resize(420, 760);
    QApplication::processEvents();
    context.expect(header->isSectionHidden(ep::StartedColumn) && header->isSectionHidden(ep::RenderTimeColumn),
                   "a narrow panel hides Started and Render Time first");
    context.expect(!header->isSectionHidden(ep::StatusColumn) && !header->isSectionHidden(ep::CompositionColumn),
                   "Composition and Status stay");
    int used = 0;
    for (int column = 0; column + 1 < ep::ListColumnCount; ++column)
        used += header->isSectionHidden(column) ? 0 : header->sectionSize(column);
    context.expect(used <= rig.list()->viewport()->width(), "nothing runs off a narrow panel");
    auto* line = rig.editor->outputLine(J(2), O(21));
    context.expect(line->folded() && rig.list()->visualItemRect(rig.item(2)->child(1)).height() ==
                                         kit::px(kit::Size::AssetRow) + kit::px(kit::Size::AssetRow),
                   "Output To folds onto its own row on a narrow panel");
    context.expect(line->destinationLink()->mapTo(line, QPoint(0, 0)).x() ==
                       line->moduleLink()->mapTo(line, QPoint(0, 0)).x(),
                   "a folded Output To lines up with the module above it");
    rig.area->resize(2000, 760);
    QApplication::processEvents();
    context.expect(!rig.editor->outputLine(J(2), O(21))->folded(), "widening unfolds it again");
}

void panelsNeverPump(TestContext& context) {
    Rig rig;
    rig.threeJobs();
    const int pumps = rig.port.pumps;
    rig.editor->selectJob(J(2));
    (void)rig.adapter->start(J(2));
    context.expect(rig.port.pumps == pumps, "panels and commands never pump; only drain() does");
}

void startedAndRenderTime(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto job = jobIn(rig, 1, host::ExportJobState::Rendering);
        job.run = host::ExportRunLink{.run = 1};
        job.progress.elapsed = std::chrono::milliseconds(65000);
        snapshot.jobs = {job, jobIn(rig, 2, host::ExportJobState::Ready)};
        snapshot.activeJob = job.id;
    });
    context.expect(!rig.cell(1, ep::StartedColumn).isEmpty() && rig.cell(1, ep::RenderTimeColumn) == "1:05",
                   "a job that ran shows when it started and how long it took");
    context.expect(rig.cell(2, ep::StartedColumn).isEmpty() && rig.cell(2, ep::RenderTimeColumn).isEmpty(),
                   "a job that has not run shows neither");
    context.expect(rig.adapter->startedAt(J(1)).has_value(), "the adapter keeps the start for every panel");
}


} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("BloomExportPanelTests"));
    QApplication::setApplicationName(QStringLiteral("ExportPanel"));
    QStandardPaths::setTestModeEnabled(true);
    kit::installKinetikTheme(application);
    QSettings().clear();
    TestContext context;
    adapterTests(context);
    panelsNeverPump(context);
    emptyState(context);
    columnsAndOrder(context);
    renderCheckAndStartAll(context);
    renderSettingsDialog(context);
    outputModuleDialog(context);
    everyFormatIsOffered(context);
    addAndRemoveOutputs(context);
    quickPickMenus(context);
    replaceConfirm(context);
    rowMenu(context);
    dragToReorder(context);
    dragFromAssets(context);
    dropManyFromAssets(context);
    currentRenderStrip(context);
    waitingForViewer(context);
    statusWording(context);
    startedAndRenderTime(context);
    detailsDialog(context);
    noRawCodes(context);
    narrowAndWide(context);
    addJobsIsOneUndo(context);
    jobsLink(context);
    addingRevealsAndSelects(context);
    editsAreUndoable(context);
    menusAndAssets(context);
    return context.ok ? 0 : 1;
}
