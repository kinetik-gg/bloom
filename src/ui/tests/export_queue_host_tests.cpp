// ProjectHost owns the export queue service and replaces it with the session (ADR 0033, section
// 11): exportQueue() is null while the old service shuts down, then a new service exists and
// exportQueueReplaced() fires once per File > New. A running job asks first, is stopped, and the
// adapter then shows an empty queue. Runs the real service over the deterministic fake runner.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/host/testing/fake_export_runner.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/export_queue_edits.hpp>
#include <bloom/ui/export_queue_service_port.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/ui_feedback.hpp>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <cstdio>

namespace {
using namespace bloom;

bool gOk = true;
void expect(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        gOk = false;
    }
}

class FakePlayer final : public ui::SoundPlayer {
  public:
    void play(const ui::FeedbackSound sound, float) noexcept override { played.push_back(sound); }
    void shutdown() noexcept override {}
    std::vector<ui::FeedbackSound> played;
};
class FakeNotifier final : public ui::Notifier {
  public:
    void post(const ui::SystemNotification& n) override { posted.push_back(n); }
    void setActivationHandler(std::function<void()>) override {}
    std::vector<ui::SystemNotification> posted;
};

template <typename Predicate> bool waitFor(Predicate&& done, const int milliseconds = 20000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < milliseconds)
        QTest::qWait(5);
    return done();
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("BloomExportQueueHostTests"));
    QApplication::setApplicationName(QStringLiteral("ExportQueueHost"));
    QStandardPaths::setTestModeEnabled(true);
    ui::kit::installKinetikTheme(application);

    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds{5});
    bridge.start();
    ui::ProjectHost host(scheduler);
    runtime::NodeDefinitionRegistry definitions;
    (void)runtime::registerBuiltInNodeDefinitions(definitions);
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    runtime::GpuProcessFrameEvaluatorOptions gpuOptions;
    gpuOptions.enabled = false;
    auto gpu = host::GpuExportProvider::create(std::move(gpuOptions));
    gpu->prepare(scheduler);
    auto backend = std::make_shared<host::testing::FakeExportRunBackend>();
    host::testing::FakeExportScript script;
    script.framesPerPoll = 1;
    script.waitingForViewerAfterFrames = 2;
    script.waitingForViewerPolls = 1000000;
    host::testing::FakeExportScript quick;
    quick.framesPerPoll = 1000;
    backend->push(quick);
    backend->push(script);
    host::ExportEncoderStatus encoders;
    auto presets = host::ExportPresetCatalogue::create(
        {.userDataDirectory = std::nullopt, .initialEncoders = encoders});
    QTemporaryDir scratch;
    host.configureExportQueue({.compiler = &compiler,
                               .ledger = std::make_shared<output::ExportResourceLedgerV1>(),
                               .gpu = gpu,
                               .retirement = host::SequenceExportRetirementOwnerV1::create(),
                               .presets = presets,
                               .limits = {},
                               .displayProcessor = nullptr,
                               .scratchDirectory = std::filesystem::path(scratch.path().toStdString()),
                               .backend = backend});
    QWidget owner;
    owner.show();
    host.setDialogOwner(&owner);
    // The seeded composition leaves the project dirty; this test is about the export prompt.
    host.setUnsavedChangeDecisionProvider([] { return ui::UnsavedChangeDecision::Discard; });

    ui::CompositionSession session(*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId());
    commands::Transaction seed("Seed", session.snapshot().revision());
    seed.emplace<commands::AddComposition>("Main", document::CompositionFormat{},
                                           core::RationalTime::fromInteger(10));
    const auto seeded = session.executeTransaction(std::move(seed));
    const auto compositionId =
        seeded.outputId<document::CompositionId>(commands::kAddCompositionOutput);
    expect(compositionId.has_value() && session.setComposition(*compositionId),
           "a composition is seeded");

    auto presetChoice = [&](const std::string& id) -> std::optional<ui::ExportPresetChoice> {
        for (const auto& preset : presets->snapshot()->presets)
            if (preset.id == id)
                return ui::ExportPresetChoice{preset.id, preset.settings};
        return std::nullopt;
    };
    ui::ExportQueueEdits edits(
        session,
        [&] {
            // A PNG sequence is startable without an optional encoder.
            host::ExportDefaultPreset preset = presets->defaultPreset();
            if (const auto png = presetChoice("builtin:png-sequence")) {
                preset.presetId = png->presetId;
                preset.settings = png->settings;
                preset.destination = host::defaultExportDestination(png->settings.format);
            }
            return preset;
        },
        presetChoice);
    ui::ServiceExportQueuePort port([&host] { return host.exportQueue(); }, edits);
    auto player = std::make_unique<FakePlayer>();
    auto notifier = std::make_unique<FakeNotifier>();
    auto* playerRef = player.get();
    auto* notifierRef = notifier.get();
    ui::UiFeedback feedback(std::move(player), std::move(notifier), [] { return false; });
    ui::ExportQueueAdapter adapter(port, &feedback);
    adapter.attach(bridge);
    QObject::connect(&host, &ui::ProjectHost::exportQueueReplaced, &adapter,
                     &ui::ExportQueueAdapter::rebind);
    int replaced = 0;
    QObject::connect(&host, &ui::ProjectHost::exportQueueReplaced, [&] { ++replaced; });

    auto* first = host.exportQueue();
    expect(first != nullptr, "the host owns a service once configured");

    QTemporaryDir out;
    // A job that renders to the end: the finish sound and system notification fire once.
    {
        const auto firstAdd = adapter.addCompositionToQueue(session.compositionId());
        expect(firstAdd.accepted && firstAdd.addedJob.has_value(), "the first job is added");
        const auto snap = adapter.snapshot();
        if (!firstAdd.addedJob || !snap || snap->jobs.empty())
            return 1;
        const auto quickJob = *firstAdd.addedJob;
        document::ExportDestination quickDestination;
        quickDestination.portability = "external";
        quickDestination.path = QUrl::fromLocalFile(out.filePath("a.png")).toString().toStdString();
        quickDestination.overwrite = document::ExportOverwritePolicy::Replace;
        (void)adapter.editOutput(quickJob, snap->jobs[0].outputs[0].id,
                                 ui::outputEdit([&](auto& e) { e.destination = quickDestination; }));
        (void)adapter.editJob(quickJob, ui::jobEdit([&](auto& e) {
                                  e.range = document::ExportRange{document::ExportRangeKind::Custom, 0, 9};
                              }));
        expect(waitFor([&] {
                   adapter.drain();
                   return adapter.snapshot()->jobs[0].commands[host::ExportCommand::Start].available;
               }),
               "the first job becomes startable");
        expect(adapter.start(quickJob).accepted, "the first job starts");
        expect(waitFor([&] {
                   adapter.drain();
                   return playerRef->played.size() == 1;
               }),
               "the render finish sound plays");
        expect(notifierRef->posted.size() == 1 && !notifierRef->posted[0].failure,
               "the render finish notification posts once");
        (void)adapter.removeFinished();
    }

    // One job with an absolute destination, rendered by the fake runner.
    const auto added = adapter.addCompositionToQueue(session.compositionId());
    expect(added.accepted, "Add to Export Queue adds a job through the real commands");
    auto snapshot = adapter.snapshot();
    expect(snapshot && snapshot->jobs.size() == 1, "the service shows the added job");
    if (!snapshot || snapshot->jobs.empty())
        return 1;
    const auto job = snapshot->jobs[0].id;
    const auto output = snapshot->jobs[0].outputs[0].id;
    document::ExportDestination destination;
    destination.portability = "external";
    destination.path = QUrl::fromLocalFile(out.filePath("shot.png")).toString().toStdString();
    destination.overwrite = document::ExportOverwritePolicy::Replace;
    const auto edit = adapter.editOutput(job, output, ui::outputEdit([&](auto& e) { e.destination = destination; }));
    expect(edit.accepted, "the destination edit is accepted");
    document::ExportRange range{document::ExportRangeKind::Custom, 0, 239};
    (void)adapter.editJob(job, ui::jobEdit([&](auto& e) { e.range = range; }));
    expect(waitFor([&] {
               adapter.drain();
               return adapter.snapshot()->jobs[0].commands[host::ExportCommand::Start].available;
           }),
           "the job becomes startable once the destination is sampled");
    expect(adapter.start(job).accepted, "Start is accepted");
    expect(waitFor([&] {
               adapter.drain();
               return adapter.snapshot()->jobs[0].state == host::ExportJobState::Rendering;
           }),
           "the fake runner renders the job");

    // File > New while it renders: the prompt, then the service is stopped and replaced.
    bool sawNull = false;
    bool prompted = false;
    auto* poll = new QTimer(&owner);
    QObject::connect(poll, &QTimer::timeout, [&] {
        if (host.exportQueue() == nullptr)
            sawNull = true;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() != QStringLiteral("runningExportsConfirm"))
                continue;
            if (prompted)
                continue;
            prompted = true;
            for (auto* button : widget->findChildren<ui::kit::KButton*>())
                if (button->text() == QStringLiteral("Stop and Continue")) {
                    // Answer once, from outside this timer's own event processing.
                    QTimer::singleShot(0, button, [button] { button->click(); });
                    break;
                }
        }
    });
    poll->start(0);
    host.newProject();
    const bool didReplace = waitFor([&] { return replaced == 1; });
    expect(didReplace, "exportQueueReplaced fires once per File > New");
    poll->stop();
    expect(prompted, "New during a render asks before stopping it");
    expect(sawNull, "exportQueue() is null while the old service shuts down");
    expect(host.exportQueue() != nullptr && host.exportQueue() != first,
           "a new service exists after the replacement");
    QTest::qWait(50);
    expect(replaced == 1, "the replacement notification fires exactly once");
    snapshot = adapter.snapshot();
    expect(snapshot && snapshot->jobs.empty(), "the adapter shows an empty queue after New");

    // Shutdown path used on quit.
    host.beginExportShutdown(host::ExportStopCause::ApplicationQuit);
    expect(waitFor([&] { return host.pumpExportShutdown(); }), "the service goes quiescent on quit");
    bridge.beginShutdown();
    waitFor([&] { return scheduler.isQuiescent(); }, 5000);
    return gOk ? 0 : 1;
}
