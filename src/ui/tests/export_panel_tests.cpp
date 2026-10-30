// The Export panel (ADR 0033) from hand-built snapshots: the adapter's single drain, the sounds on
// success and failure, the rows and their status wording for every state, a calm Waiting for the
// Viewer, typed availability reasons on every control, the replace confirmation, the Linux-only
// note, an absent size estimate, and one command per gesture.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#include "export_editor_internal.hpp"
#include "export_panel_text.hpp"
#include "export_job_tree.hpp"
#include "export_test_support.hpp"
#include "window_fixture.hpp"

#include <bloom/commands/transaction.hpp>
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
#include <QPlainTextEdit>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>

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
    ep::ExportJobTree* tree() const { return area->findChild<ep::ExportJobTree*>(); }
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

void panelsNeverPump(TestContext& context) {
    Rig rig;
    rig.threeJobs();
    const int pumps = rig.port.pumps;
    // Selecting, editing and refreshing a panel never pumps; a second panel changes nothing.
    rig.tree()->setCurrentItem(rig.tree()->topLevelItem(1));
    (void)rig.adapter->start(document::ExportJobId::fromRaw(2));
    context.expect(rig.port.pumps == pumps, "panels and commands never pump; only drain() does");
}

void rowsAndStatus(TestContext& context) {
    Rig rig;
    context.expect(rig.editor != nullptr, "the Export editor is created by the registry");
    auto* tree = rig.tree();
    context.expect(tree != nullptr, "the job tree exists");
    // Empty state first: one plain line and the Add button.
    auto* emptyText = rig.find<QLabel>("exportEmptyText");
    context.expect(emptyText != nullptr && emptyText->isVisibleTo(rig.area.get()),
                   "an empty queue shows the empty-state line");
    context.expect(rig.find<kit::KButton>("exportAddActiveButton") != nullptr,
                   "the empty state offers Add active composition");

    rig.threeJobs();
    context.expect(tree->topLevelItemCount() == 3, "three jobs make three rows");
    context.expect(tree->topLevelItem(0)->childCount() == 1, "an output is a child row");
    const auto* first = tree->topLevelItem(0);
    context.expect(first->text(ep::NameColumn) == "Shot 010", "the Name cell shows the job name");
    context.expect(first->text(ep::CompositionColumn) == "Main", "the Composition cell names it");
    context.expect(first->text(ep::RangeColumn).contains("Work area"), "the Range cell reads");
    context.expect(first->text(ep::StatusColumn) == "Rendering", "a rendering job reads Rendering");
    context.expect(tree->topLevelItem(1)->text(ep::StatusColumn) == "Waiting",
                   "a queued job reads Waiting");
    context.expect(tree->topLevelItem(2)->text(ep::StatusColumn) == "Failed",
                   "a failed job reads Failed");
    context.expect(tree->topLevelItem(2)->toolTip(ep::StatusColumn).contains("verification"),
                   "a failure reads as a plain sentence in its status tooltip");
    const auto* output = first->child(0);
    context.expect(output->text(ep::NameColumn) == "H.264 MP4", "an output row names its preset");
    context.expect(output->data(ep::CompositionColumn, ep::kDestinationRole).toString().contains("out.mp4"),
                   "an output row shows the resolved destination");
    context.expect(output->toolTip(ep::NameColumn).contains("/project/renders/out.mp4"),
                   "the full destination is the output row's tooltip");

    // The strip while it runs.
    auto* strip = rig.find<QWidget>("exportRunStrip");
    context.expect(strip != nullptr && strip->isVisibleTo(rig.area.get()),
                   "the run strip shows while a job renders");
    context.expect(rig.find<QLabel>("exportRunFrames")->text() == "Frame 121 of 240",
                   "the strip shows frame x of y");
    context.expect(rig.find<QLabel>("exportRunTime")->text().contains("Elapsed 0:07") &&
                       rig.find<QLabel>("exportRunTime")->text().contains("about 0:42 left"),
                   "the strip shows elapsed time and the ETA when known");

    // Every job state has wording and the right tone.
    for (int value = 1; value <= 12; ++value) {
        auto job = makeJob(9, "x", rig.composition(), {makeOutput(90)});
        job.state = static_cast<host::ExportJobState>(value);
        if (job.state == host::ExportJobState::Failed)
            job.failure = host::ExportFailure{.reason = "boom"};
        context.expect(!ep::jobStatus(job).text.isEmpty(), "every job state has status text");
    }
    context.expect(ep::jobStatus(tree != nullptr ? makeJob(1, "x", rig.composition(), {makeOutput(2)})
                                                 : host::ExportJobView{})
                           .tone == ep::StatusTone::Neutral,
                   "a ready job is neutral");
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
    context.expect(rig.find<QLabel>("exportRunState")->text() ==
                       "Waiting for the Viewer to finish playback",
                   "waiting for the Viewer says so in plain words");
    context.expect(!rig.find<QLabel>("exportRunTime")->text().contains("left"),
                   "waiting for the Viewer shows no ETA");
    const auto status = ep::jobStatus(rig.port.snapshot()->jobs[0]);
    context.expect(status.tone == ep::StatusTone::Neutral &&
                       ep::toneColor(status.tone) != kit::Color::Error &&
                       ep::toneColor(status.tone) != kit::Color::Warn,
                   "waiting for the Viewer is neutral ink, never an error colour");
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
        refuse(snapshot.commands, host::ExportCommand::StopAll, host::ExportUnavailableCode::NothingToStop,
               "No job is running.");
        refuse(snapshot.commands, host::ExportCommand::RemoveFinished,
               host::ExportUnavailableCode::NothingFinished, "No job has finished.");
    });
    rig.tree()->setCurrentItem(rig.tree()->topLevelItem(0));
    QApplication::processEvents();
    auto* start = rig.find<kit::KButton>("exportJobStartButton");
    context.expect(start != nullptr && !start->isEnabled() &&
                       start->toolTip() == "Choose a destination.",
                   "Start gives its reason as the tooltip");
    auto* retry = rig.find<kit::KButton>("exportJobRetryButton");
    context.expect(retry != nullptr && !retry->isEnabled() && retry->toolTip().contains("finished"),
                   "Retry gives its reason");
    context.expect(rig.find<kit::KButton>("exportStopAllButton")->toolTip() == "No job is running." &&
                       !rig.find<kit::KButton>("exportStopAllButton")->isEnabled(),
                   "Stop All gives its reason");
    context.expect(rig.find<kit::KButton>("exportRemoveFinishedButton")->toolTip() ==
                       "No job has finished.",
                   "Remove finished gives its reason");
    context.expect(rig.find<kit::KButton>("exportStartAllButton")->variant() ==
                       kit::KButton::Variant::Primary,
                   "Start All is the primary action while something can run");
    // A running job locks its edits with the reason on every editing control.
    rig.threeJobs();
    rig.tree()->setCurrentItem(rig.tree()->topLevelItem(0));
    QApplication::processEvents();
    auto* name = rig.find<kit::KLineEdit>("exportJobName");
    context.expect(name != nullptr && !name->isEnabled() && name->toolTip().contains("locked"),
                   "a running job's edit controls are disabled with the reason");
    // The context menu lists the reasons too.
    rig.publish([](host::ExportQueueSnapshot& snapshot) {
        refuse(snapshot.jobs[1].commands, host::ExportCommand::Start,
               host::ExportUnavailableCode::PreflightBlocked, "Choose a destination.");
    });
    QStringList disabled;
    QTimer::singleShot(0, rig.tree(), [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu == nullptr)
            return;
        for (auto* action : menu->actions())
            if (!action->isEnabled() && !action->toolTip().isEmpty())
                disabled.push_back(action->objectName());
        menu->close();
    });
    emit rig.tree()->customContextMenuRequested(
        rig.tree()->visualItemRect(rig.tree()->topLevelItem(1)).center());
    QApplication::processEvents();
    context.expect(!disabled.isEmpty(), "context menu actions that cannot run carry their reason");
}

void destinationStates(TestContext& context) {
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
    rig.tree()->setCurrentItem(rig.tree()->topLevelItem(0));
    QApplication::processEvents();
    auto* replace = rig.find<kit::KButton>("exportReplaceButton");
    context.expect(replace != nullptr && replace->isVisibleTo(rig.area.get()),
                   "an existing destination shows the Replace existing files row");
    context.expect(rig.find<QLabel>("exportReplaceText")->text() == "Replace existing files?",
                   "the row asks Replace existing files?");
    QTest::mouseClick(replace, Qt::LeftButton);
    context.expect(!rig.port.calls.empty() && rig.port.calls.back() == "confirmReplace 1 11",
                   "Replace calls confirmReplace(job, output)");

    // Checking destination is quiet; no estimate shows nothing.
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        auto output = makeOutput(11);
        output.preflight.push_back({.code = host::ExportPreflightCode::DestinationCheckPending,
                                    .target = host::ExportPreflightTarget::Destination,
                                    .severity = host::ExportPreflightSeverity::Blocking,
                                    .reason = "Checking destination…"});
        snapshot.jobs = {makeJob(1, "Shot 010", rig.composition(), {output})};
    });
    auto* reason = rig.find<QLabel>("exportDestinationReason");
    context.expect(reason != nullptr && reason->text() == "Checking destination…",
                   "a pending destination says Checking destination…");
    context.expect(!rig.find<QLabel>("exportReplaceText")->isVisibleTo(rig.area.get()),
                   "the replace row is gone once nothing exists");
    context.expect(!rig.find<QLabel>("exportEstimate")->isVisibleTo(rig.area.get()),
                   "an absent estimatedBytes shows nothing");
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        snapshot.jobs[0].outputs[0].estimatedBytes = 12'000'000;
    });
    context.expect(rig.find<QLabel>("exportEstimate")->isVisibleTo(rig.area.get()) &&
                       rig.find<QLabel>("exportEstimate")->text().contains("About"),
                   "a given estimate is shown");
}

void platformNotSupported(TestContext& context) {
    Rig rig;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) {
        snapshot.jobs = {makeJob(1, "Shot 010", rig.composition(), {makeOutput(11)})};
        for (const auto command : {host::ExportCommand::StartAll})
            refuse(snapshot.commands, command, host::ExportUnavailableCode::PlatformNotSupported,
                   "Export runs on Linux in this version");
        refuse(snapshot.jobs[0].commands, host::ExportCommand::Start,
               host::ExportUnavailableCode::PlatformNotSupported,
               "Export runs on Linux in this version");
    });
    rig.tree()->setCurrentItem(rig.tree()->topLevelItem(0));
    QApplication::processEvents();
    auto* note = rig.find<QLabel>("exportPlatformNote");
    context.expect(note != nullptr && note->isVisibleTo(rig.area.get()) &&
                       note->text() == "Export runs on Linux in this version",
                   "an unsupported platform says so under the header");
    context.expect(!rig.find<kit::KButton>("exportStartAllButton")->isEnabled() &&
                       !rig.find<kit::KButton>("exportJobStartButton")->isEnabled(),
                   "Start and Start All are disabled");
    context.expect(rig.find<kit::KLineEdit>("exportJobName")->isEnabled(),
                   "editing still works where export cannot run");
    context.expect(rig.tree()->topLevelItem(0)->text(ep::StatusColumn) == "Ready",
                   "an unsupported platform does not turn a job into a fault");
}

void editsAreOneCommand(TestContext& context) {
    Rig rig;
    rig.threeJobs();
    rig.tree()->setCurrentItem(rig.tree()->topLevelItem(1));
    QApplication::processEvents();
    rig.port.calls.clear();
    auto* name = rig.find<kit::KLineEdit>("exportJobName");
    name->setText("Hero");
    emit name->editingFinished();
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0] == "editJob 2 name=Hero",
                   "renaming a job is one command");
    rig.port.calls.clear();
    emit rig.tree()->enabledToggled(2, 0, false);
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0] == "editJob 2 enabled=0",
                   "the enabled check on a job row is one command");
    rig.port.calls.clear();
    emit rig.tree()->enabledToggled(2, 21, false);
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0] == "editOutput 2 21 enabled=0",
                   "the enabled check on an output row is one command");
    rig.port.calls.clear();
    emit rig.tree()->moveRequested(3, 0);
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0] == "moveJob 3 0",
                   "a drag reorder is one MoveJob");
    rig.port.calls.clear();
    auto* range = rig.find<kit::KDropdown>("exportRangeKind");
    range->chooseIndex(2);
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0].starts_with("editJob 2 range=3:"),
                   "choosing a custom range is one command");
    rig.port.calls.clear();
    auto* preset = rig.find<kit::KDropdown>("exportPresetPicker");
    preset->chooseIndex(preset->findData(QString("builtin:prores-422hq-mov")));
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0].starts_with("applyPreset 2 21"),
                   "choosing a preset applies it in one command");
    // Video comes first, VFX / sequences second, as disabled group titles.
    context.expect(preset->itemText(0) == "Video" && !preset->isItemEnabled(0),
                   "the preset list starts with the Video group");
    int vfx = -1;
    for (int i = 0; i < preset->count(); ++i)
        if (preset->itemText(i) == "VFX / sequences")
            vfx = i;
    context.expect(vfx > 0, "VFX / sequences is the second group");
    rig.port.calls.clear();
    QTest::mouseClick(rig.find<kit::KButton>("exportAddOutputButton"), Qt::LeftButton);
    context.expect(rig.port.calls == std::vector<std::string>{"addOutput 2"}, "Add output is one command");
    rig.port.calls.clear();
    auto* importSwitch = rig.find<kit::KSwitch>("exportImportIntoAssets");
    importSwitch->setChecked(true);
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0] == "editOutput 2 21 import=1",
                   "Import into Assets is one per-output command");
    // A refusal is shown, not swallowed.
    rig.port.nextEdit = {false, "Edits are locked while the job runs.", std::nullopt};
    name->setText("Again");
    emit name->editingFinished();
    context.expect(rig.find<QLabel>("exportRefusal")->isVisibleTo(rig.area.get()),
                   "a refused edit shows its reason");
    rig.port.nextEdit = {true, {}, std::nullopt};
    // Copy as command.
    rig.port.calls.clear();
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
    rig.tree()->setCurrentItem(rig.tree()->topLevelItem(2));
    QApplication::processEvents();
    auto* log = rig.find<QWidget>("exportLog");
    context.expect(log != nullptr, "the log exists");
    auto* pane = rig.area->findChild<ep::ExportDetailPane*>();
    context.expect(pane != nullptr && pane->logText().contains("Frame 1 written"),
                   "the log shows the run's entries");
    // Appended incrementally, and a trimmed gap is said.
    rig.port.logs[3].push_back({.sequence = 2, .sinceStart = std::chrono::milliseconds(2000),
                                .code = "frame.ok", .message = "Frame 2 written"});
    rig.port.logGap = true;
    rig.publish([&](host::ExportQueueSnapshot& snapshot) { snapshot.jobs[2].lastLogSequence = 2; });
    context.expect(pane->logText().contains("Frame 2 written") &&
                       pane->logText().contains("Earlier entries were trimmed"),
                   "new entries append and a trimmed gap is reported");
    context.expect(pane->logText().count("Frame 1 written") == 1, "entries are not re-read");
    // Structured verification.
    bool failedLine = false;
    for (auto* label : rig.area->findChildren<QLabel*>("exportVerificationLine"))
        failedLine = failedLine || (label->text().contains("Frame count") &&
                                    label->text().contains("expected 240, found 239"));
    context.expect(failedLine, "a failed check reads expected against found");
}

void failuresReadAsSentences(TestContext& context) {
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
            .summary = std::string(
                host::exportFailureSummary(host::ExportFailureCode::AnalysisRefused))};
        snapshot.jobs = {job};
    });
    rig.tree()->setCurrentItem(rig.tree()->topLevelItem(0));
    QApplication::processEvents();
    QStringList visible;
    visible.push_back(rig.tree()->topLevelItem(0)->text(ep::StatusColumn));
    visible.push_back(rig.tree()->topLevelItem(0)->toolTip(ep::StatusColumn));
    for (auto* label : rig.area->findChildren<QLabel*>("exportVerificationLine"))
        visible.push_back(label->text() + label->toolTip());
    const auto all = visible.join('\n');
    context.expect(all.contains("OpenEXR"), "the failure line is a sentence with a remedy");
    context.expect(!all.contains("stage=") && !all.contains("code=") && !all.contains("facet"),
                   "the visible failure line has no stage, code or facet numbers");
    // The raw detail lives only in the log section, behind Details, and can be copied.
    auto* details = rig.find<QPlainTextEdit>("exportFailureDetails");
    context.expect(details != nullptr && details->toPlainText().contains("facet=3") &&
                       !details->isVisibleTo(rig.area.get()),
                   "raw detail is in the Details disclosure, closed by default");
    rig.find<kit::KButton>("exportDetailsToggle")->setChecked(true);
    context.expect(details->isVisibleTo(rig.area.get()), "Details opens the raw text");
    context.expect(rig.find<kit::KButton>("exportCopyDetailsButton") != nullptr,
                   "Copy details is offered");
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

void emptyAndAdd(TestContext& context) {
    Rig rig;
    auto* add = rig.find<kit::KButton>("exportAddActiveButton");
    context.expect(add != nullptr && add->isEnabled(), "Add active composition is enabled");
    QTest::mouseClick(add, Qt::LeftButton);
    context.expect(rig.port.calls.size() == 1 && rig.port.calls[0].starts_with("addJob "),
                   "Add active composition adds a job");
    context.expect(rig.tree()->topLevelItemCount() == 1, "the new job appears in the list");
    context.expect(rig.editor->selectedJob().has_value(), "the new job is selected");
    // A dropped refusal leaves the button with its reason.
    Rig closed;
    closed.publish([](host::ExportQueueSnapshot& snapshot) {
        refuse(snapshot.commands, host::ExportCommand::AddJob, host::ExportUnavailableCode::QueueFull,
               "The queue is full.");
    });
    context.expect(!closed.find<kit::KButton>("exportAddActiveButton")->isEnabled() &&
                       closed.find<kit::KButton>("exportAddActiveButton")->toolTip() ==
                           "The queue is full.",
                   "Add gives the queue's reason when it cannot add");
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
    rig.port.nextEdit = {false, "The queue is full.", std::nullopt};
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
    context.expect(!portRef->calls.empty() && portRef->calls.back().starts_with("addJob "),
                   "the menu item adds a job");
    QApplication::processEvents();
    auto* host = fixture.window->workspaceHost();
    bool shown = false;
    for (auto* area : host->findChildren<EditorArea*>())
        shown = shown || area->editorId() == "bloom.export";
    context.expect(shown, "adding a job from the menu shows the Export panel");
    context.expect(fixture.window->windowExportAction()->isEnabled(), "Window > Export is enabled");
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
    rowsAndStatus(context);
    waitingForViewer(context);
    availabilityReasons(context);
    destinationStates(context);
    platformNotSupported(context);
    editsAreOneCommand(context);
    logAndVerification(context);
    failuresReadAsSentences(context);
    jobsLink(context);
    emptyAndAdd(context);
    addingRevealsAndSelects(context);
    editsAreUndoable(context);
    menusAndAssets(context);
    return context.ok ? 0 : 1;
}
