// bloom.ui.mcp-live-ui-binding (ADR 0034, "MCP is a UI for agents"): the real McpLiveUiBinding over
// the real window fixture. Every member of mcp::McpLiveUiPort is called through port(), and the
// visible state and the typed refusals are asserted: project lifecycle (UnsavedChanges, NoPath,
// DestinationExists, PathInvalid, JobsRunning, Busy, Failed, stopJobs), reveal, selection in the
// Timeline, the transport, the Viewer's footer, colour settings, and that no session seam dirties
// the project. Also the enum-label table: every enum parameter's Properties choices equal the
// document's one table. Offscreen.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#include "composition_preview_stage_shared.hpp"
#include "properties_registry_row.hpp"
#include "readonly_archive_fixture.hpp"
#include "window_fixture.hpp"

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter_enums.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/project_color_choice.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/host/testing/fake_export_runner.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/ui/export_queue_edits.hpp>
#include <bloom/ui/export_queue_service_port.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/mcp_live_ui_binding.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QFile>
#include <QUrl>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

namespace {
using namespace bloom;
namespace fs = std::filesystem;
using mcp::McpUiRefusal;

int gFailures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++gFailures;
    }
}

template <typename Predicate> bool waitFor(Predicate&& done, const int milliseconds = 20000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < milliseconds)
        QTest::qWait(5);
    return done();
}

struct World final {
    std::function<std::optional<ui::ExportPresetChoice>(const std::string&)> presetChoice;
    std::unique_ptr<ui::ServiceExportQueuePort> port;
    std::unique_ptr<ui::ExportQueueEdits> editsOwner;
    std::unique_ptr<ui::ExportQueueAdapter> adapter;
    std::shared_ptr<host::ExportPresetCatalogue> presets;
    std::shared_ptr<host::testing::FakeExportRunBackend> backend;
    std::shared_ptr<host::GpuExportProvider> gpu;
    QTemporaryDir scratch;
};

mcp::McpProjectResult runProject(ui::McpLiveUiBinding& binding, mcp::McpProjectRequest request) {
    std::optional<mcp::McpProjectResult> result;
    int calls = 0;
    binding.projectLifecycle(std::move(request), [&](mcp::McpProjectResult value) {
        ++calls;
        result = std::move(value);
    });
    waitFor([&] { return result.has_value(); });
    QTest::qWait(20);
    expect(calls == 1, "done is called exactly once");
    return result.value_or(mcp::McpProjectResult{.refusal = McpUiRefusal::Failed,
                                                 .message = "no answer"});
}

mcp::McpProjectRequest request(const mcp::McpProjectRequestKind kind, fs::path path = {},
                               const bool discard = false, const bool stopJobs = false,
                               const bool replace = false) {
    mcp::McpProjectRequest value;
    value.kind = kind;
    value.path = std::move(path);
    value.discardChanges = discard;
    value.stopJobs = stopJobs;
    value.replace = replace;
    return value;
}

mcp::McpTransportRequest transportRequest(const mcp::McpTransportAction action,
                                          std::optional<core::RationalTime> time = {}) {
    return {.action = action, .time = time};
}

// ---- enum labels -------------------------------------------------------------------------------

void testEnumTableParity() {
    std::set<std::string> keys;
    for (const auto& definition : document::builtInNodeDefinitions().definitions())
        for (const auto& parameter : definition.parameters)
            keys.insert(parameter.schemaKey);
    int enumerations = 0;
    for (const auto& key : keys) {
        const auto table = document::parameterEnumOptions(key);
        const auto items = ui::propertiesSelectorItems(key);
        if (table.empty()) {
            expect(items.isEmpty(), "a parameter outside the table offers no Properties choices");
            continue;
        }
        ++enumerations;
        bool same = static_cast<std::size_t>(items.size()) == table.size();
        for (std::size_t index = 0; same && index < table.size(); ++index)
            same = items[static_cast<qsizetype>(index)].first ==
                       QString::fromStdString(table[index].label) &&
                   items[static_cast<qsizetype>(index)].second == table[index].value;
        if (!same)
            std::fprintf(stderr, "  enum differs: %s\n", key.c_str());
        expect(same, "Properties choices equal the document table (labels, values, order)");
    }
    expect(enumerations >= 20, "the built-in nodes offer their enumerations through the table");
    expect(!document::parameterEnumOptions(document::kBlendModeParameterSchemaKey).empty() &&
               !document::parameterEnumOptions("bloom.shape.kind").empty() &&
               !document::parameterEnumOptions(document::kScalarOperationParameterSchemaKey).empty(),
           "blend modes, shape kinds and scalar operations are in the table");
}

// ---- compile diagnostics carry their subject as data -------------------------------------------

void testCompileSubjectIsData() {
    runtime::SnapshotCompileResult result;
    result.status = runtime::SnapshotCompileStatus::Failed;
    result.diagnostics.push_back(
        {.code = runtime::CompileDiagnosticCode::MissingInput,
         .severity = runtime::DiagnosticSeverity::Error,
         .subject = {.compositionId = document::CompositionId::fromRaw(4),
                     .nodeId = document::NodeId::fromRaw(12),
                     .parameterId = document::ParameterId::fromRaw(30),
                     .layerId = document::LayerId::fromRaw(5)},
         .summary = "Layer Output could not be lowered",
         .detail = "The input is not connected."});
    const auto diagnostics = ui::detail::compileTaskDiagnostics(result);
    expect(diagnostics.size() == 1 && diagnostics[0].detail == "The input is not connected.",
           "the detail stays plain: no ids in the sentence");
    expect(diagnostics[0].subject && diagnostics[0].subject->node == 12 &&
               diagnostics[0].subject->parameter == 30 && diagnostics[0].subject->layer == 5,
           "the subject carries node, parameter and layer ids as data");
    const auto summary = runtime::compileFailureSummary(result, document::CompositionId::fromRaw(4));
    expect(summary && summary->nodeId && summary->nodeId->value() == 12 &&
               summary->parameterId && summary->parameterId->value() == 30,
           "the engine's summary names the same subject");
}

// ---- the window --------------------------------------------------------------------------------

void testBinding() {
    World world;
    QTemporaryDir out;
    QTemporaryDir projects;
    expect(out.isValid() && projects.isValid() && world.scratch.isValid(), "temp folders");
    ui::test::WindowFixture fixture([&](ui::test::WindowFixture& f) {
        runtime::GpuProcessFrameEvaluatorOptions gpuOptions;
        gpuOptions.enabled = false;
        world.gpu = host::GpuExportProvider::create(std::move(gpuOptions));
        world.gpu->prepare(f.scheduler);
        world.backend = std::make_shared<host::testing::FakeExportRunBackend>();
        host::testing::FakeExportScript script;
        script.framesPerPoll = 1;
        script.waitingForViewerAfterFrames = 2;
        script.waitingForViewerPolls = 1000000;
        world.backend->push(script);
        host::ExportEncoderStatus encoders;
        encoders.exportPlatform = true;
        world.presets = host::ExportPresetCatalogue::create(
            {.userDataDirectory = std::nullopt, .initialEncoders = encoders});
        f.projectHost.configureExportQueue(
            {.compiler = &f.compiler,
             .ledger = std::make_shared<output::ExportResourceLedgerV1>(),
             .gpu = world.gpu,
             .retirement = host::SequenceExportRetirementOwnerV1::create(),
             .presets = world.presets,
             .limits = {},
             .displayProcessor = nullptr,
             .scratchDirectory = std::filesystem::path(world.scratch.path().toStdString()),
             .backend = world.backend});
        // Captured by pointer to `world`, which outlives the edits object (locals would dangle).
        auto* w = &world;
        w->presetChoice = [w](const std::string& id) -> std::optional<ui::ExportPresetChoice> {
            for (const auto& preset : w->presets->snapshot()->presets)
                if (preset.id == id)
                    return ui::ExportPresetChoice{preset.id, preset.settings};
            return std::nullopt;
        };
        world.editsOwner = std::make_unique<ui::ExportQueueEdits>(
            f.session,
            [w] {
                host::ExportDefaultPreset preset = w->presets->defaultPreset();
                if (const auto png = w->presetChoice("builtin:png-sequence")) {
                    preset.presetId = png->presetId;
                    preset.settings = png->settings;
                    preset.destination = host::defaultExportDestination(png->settings.format);
                }
                return preset;
            },
            w->presetChoice);
        world.port = std::make_unique<ui::ServiceExportQueuePort>(
            [&f] { return f.projectHost.exportQueue(); }, *world.editsOwner);
        world.adapter = std::make_unique<ui::ExportQueueAdapter>(*world.port);
        world.adapter->attach(f.bridge);
        QObject::connect(&f.projectHost, &ui::ProjectHost::exportQueueReplaced,
                         world.adapter.get(), &ui::ExportQueueAdapter::rebind);
        f.exportQueue = world.adapter.get();
        // The composition root's rebinding, so New and Open leave the session on the live document.
        QObject::connect(&f.projectHost, &ui::ProjectHost::sessionReplaced, &f.session, [&f] {
            auto [document, stack] = f.projectHost.liveDocumentAndStack();
            if (document != nullptr && stack != nullptr)
                f.session.rebind(*document, *stack, f.projectHost.lowestCompositionId());
        });
        QObject::connect(&f.projectHost, &ui::ProjectHost::colorSettingsChanged, &f.session, [&f] {
            if (const auto* settings = f.projectHost.colorSettings(); settings != nullptr)
                f.session.setColorSettings(*settings);
        });
        if (!f.session.addSolidLayer("Background", {0.035, 0.045, 0.075, 1.0}) ||
            !f.session.addSolidLayer("Title", {0.8, 0.2, 0.2, 1.0}))
            throw std::runtime_error("layers");
    });
    auto& session = fixture.session;
    auto& host = fixture.projectHost;
    ui::McpLiveUiBinding binding(host, session, fixture.preview->playbackController());
    binding.setWindow(fixture.window.get());
    const auto port = binding.port();
    expect(port.revealComposition && port.projectLifecycle && port.setSelection &&
               port.activateComposition && port.transport && port.setViewer &&
               port.setColorSettings,
           "all seven members are bound");

    // ---- refusals over an untitled, edited project
    expect(host.isDirty(), "the seeded project has unsaved changes");
    auto result = runProject(binding, request(mcp::McpProjectRequestKind::New));
    expect(result.refusal == McpUiRefusal::UnsavedChanges &&
               result.message.find("Untitled") != std::string::npos,
           "New with unsaved changes -> UnsavedChanges, naming the project");
    result = runProject(binding, request(mcp::McpProjectRequestKind::Close));
    expect(result.refusal == McpUiRefusal::UnsavedChanges, "Close with unsaved changes");
    result = runProject(binding, request(mcp::McpProjectRequestKind::Save));
    expect(result.refusal == McpUiRefusal::NoPath, "Save on an untitled project -> NoPath");
    result = runProject(binding, request(mcp::McpProjectRequestKind::SaveAs, "relative.bloom"));
    expect(result.refusal == McpUiRefusal::PathInvalid, "SaveAs with a relative path");
    result = runProject(binding,
                        request(mcp::McpProjectRequestKind::SaveAs,
                                fs::path(projects.path().toStdString()) / "x.txt"));
    expect(result.refusal == McpUiRefusal::PathInvalid, "SaveAs that is not a .bloom file");
    result = runProject(binding,
                        request(mcp::McpProjectRequestKind::SaveAs,
                                fs::path(projects.path().toStdString()) / "nope" / "x.bloom"));
    expect(result.refusal == McpUiRefusal::PathInvalid, "SaveAs into a missing folder");
    result = runProject(binding,
                        request(mcp::McpProjectRequestKind::Open,
                                fs::path(projects.path().toStdString()) / "missing.bloom"));
    expect(result.refusal == McpUiRefusal::PathInvalid ||
               result.refusal == McpUiRefusal::UnsavedChanges,
           "Open of a missing file is refused");
    result = runProject(binding,
                        request(mcp::McpProjectRequestKind::Open,
                                fs::path(projects.path().toStdString()) / "missing.bloom", true));
    expect(result.refusal == McpUiRefusal::PathInvalid, "Open of a missing file -> PathInvalid");
    expect(!host.isBusy() && host.isDirty(), "refusals leave the project untouched");

    // ---- Save As, Save, and the destination rule
    const auto shot = fs::path(projects.path().toStdString()) / "SC020.bloom";
    result = runProject(binding, request(mcp::McpProjectRequestKind::SaveAs, shot));
    expect(result.ok() && result.projectName == "SC020" && result.path == shot &&
               fs::exists(shot),
           "SaveAs writes the file and names the project");
    expect(!host.isDirty(), "the project is clean after Save As");
    result = runProject(binding, request(mcp::McpProjectRequestKind::SaveAs, shot));
    expect(result.refusal == McpUiRefusal::DestinationExists,
           "SaveAs onto an existing file -> DestinationExists");
    result = runProject(binding,
                        request(mcp::McpProjectRequestKind::SaveAs, shot, false, false, true));
    expect(result.ok(), "SaveAs with replace overwrites");
    result = runProject(binding, request(mcp::McpProjectRequestKind::Save));
    expect(result.ok() && result.path == shot, "Save writes the project's own file");

    // ---- session seams: they act as the artist does, and never dirty the project
    const auto revision = session.snapshot().revision();
    const auto compositionId = session.compositionId();
    const auto* composition = session.composition();
    std::vector<document::LayerId> layers;
    for (const auto& layer : composition->graph().layerOutputs())
        layers.push_back(layer.layerId);
    expect(layers.size() == 2, "two layers");
    auto* timeline = fixture.window->findChild<ui::TimelineEditor*>();
    expect(timeline != nullptr && timeline->layerStackForTest() != nullptr, "the Timeline exists");
    session.clearSelection();
    QApplication::processEvents();
    const auto before = timeline->layerStackForTest()->grab().toImage();
    auto selection = port.setSelection(
        {.composition = compositionId, .layers = {layers.front()}, .nodes = {},
         .mode = mcp::McpSelectionMode::Set});
    QApplication::processEvents();
    expect(selection.ok(), "selection.set accepts a layer");
    const auto boundary = session.boundaryNodeForLayer(layers.front());
    expect(boundary && session.selectedNodes().contains(*boundary) &&
               session.selectedNodes().size() == 1,
           "the layer is selected in the session");
    expect(timeline->layerStackForTest()->grab().toImage() != before,
           "the selection appears in the Timeline");
    selection = port.setSelection({.composition = compositionId, .layers = {layers.back()},
                                   .nodes = {}, .mode = mcp::McpSelectionMode::Add});
    expect(selection.ok() && session.selectedNodes().size() == 2, "Add extends the selection");
    selection = port.setSelection({.composition = compositionId, .layers = {layers.front()},
                                   .nodes = {}, .mode = mcp::McpSelectionMode::Remove});
    expect(selection.ok() && session.selectedNodes().size() == 1, "Remove drops a layer");
    selection = port.setSelection({.composition = compositionId, .layers = {}, .nodes = {},
                                   .mode = mcp::McpSelectionMode::Clear});
    expect(selection.ok() && session.selectedNodes().empty(), "Clear selects nothing");
    selection = port.setSelection({.composition = compositionId,
                                   .layers = {document::LayerId::fromRaw(987654)}, .nodes = {},
                                   .mode = mcp::McpSelectionMode::Set});
    expect(selection.refusal == McpUiRefusal::UnknownTarget, "an unknown layer -> UnknownTarget");
    selection = port.setSelection({.composition = document::CompositionId::fromRaw(987654),
                                   .layers = {}, .nodes = {}, .mode = mcp::McpSelectionMode::Clear});
    expect(selection.refusal == McpUiRefusal::UnknownTarget,
           "an unknown composition -> UnknownTarget");

    // transport
    const auto seconds = [](const std::int64_t whole) { return core::RationalTime::fromInteger(whole); };
    auto moved = port.transport(transportRequest(mcp::McpTransportAction::Seek, seconds(3)));
    expect(moved.ok() && session.currentTime() == seconds(3), "Seek moves the playhead");
    moved = port.transport(transportRequest(mcp::McpTransportAction::StepForward));
    expect(moved.ok() && session.currentTime() > seconds(3), "StepForward moves one frame on");
    moved = port.transport(transportRequest(mcp::McpTransportAction::StepBackward));
    expect(moved.ok() && session.currentTime() == seconds(3), "StepBackward returns");
    moved = port.transport(transportRequest(mcp::McpTransportAction::GoToEnd));
    expect(moved.ok() && session.currentTime() > seconds(9), "GoToEnd shows the last frame");
    moved = port.transport(transportRequest(mcp::McpTransportAction::GoToStart));
    expect(moved.ok() && session.currentTime() == seconds(0), "GoToStart returns to zero");
    moved = port.transport(transportRequest(mcp::McpTransportAction::Seek, seconds(99)));
    expect(moved.refusal == McpUiRefusal::InvalidArgument, "Seek outside the composition");
    moved = port.transport(transportRequest(mcp::McpTransportAction::Seek));
    expect(moved.refusal == McpUiRefusal::InvalidArgument, "Seek without a time");
    moved = port.transport(transportRequest(mcp::McpTransportAction::Play));
    expect(moved.ok() && fixture.preview->playbackController().state() ==
                             ui::PlaybackState::Playing,
           "Play starts the transport the Viewer footer uses");
    moved = port.transport(transportRequest(mcp::McpTransportAction::Stop));
    expect(moved.ok() && fixture.preview->playbackController().state() ==
                             ui::PlaybackState::Stopped,
           "Stop pauses it");

    // reveal and activate
    expect(port.revealComposition(compositionId, seconds(2)) &&
               session.currentTime() == seconds(2),
           "editor.reveal shows the composition and seeks");
    expect(!port.revealComposition(document::CompositionId::fromRaw(987654), std::nullopt),
           "editor.reveal of an unknown composition is false");
    expect(!port.revealComposition(compositionId, seconds(99)), "editor.reveal past the end");
    expect(port.activateComposition(compositionId).ok(), "composition.activate");
    expect(port.activateComposition(document::CompositionId::fromRaw(987654)).refusal ==
               McpUiRefusal::UnknownTarget,
           "composition.activate of an unknown id -> UnknownTarget");

    // viewer
    auto* viewer = fixture.window->displayedViewer();
    expect(viewer != nullptr, "a Viewer is displayed");
    if (viewer != nullptr) {
        auto* channel = fixture.window->findChild<ui::kit::KDropdown*>("viewerChannelDropdown");
        expect(channel != nullptr, "the footer has a channel dropdown");
        auto viewerResult = port.setViewer({.channel = mcp::McpViewerChannel::Alpha});
        expect(viewerResult.ok() && viewer->channelForTest() == ui::ViewerChannel::Alpha &&
                   channel != nullptr && channel->currentIndex() == 5,
               "the Viewer channel changes the footer dropdown");
        viewerResult = port.setViewer({.channel = mcp::McpViewerChannel::Rgba});
        expect(viewerResult.ok() && channel->currentIndex() == 0, "and back to RGBA");
        auto* exposure = fixture.window->findChild<ui::kit::KValueField*>("viewerExposure");
        viewerResult = port.setViewer({.exposure = 1.5});
        expect(viewerResult.ok() && exposure != nullptr && exposure->value() == 1.5,
               "exposure moves the footer field");
        viewerResult = port.setViewer({.exposure = 99.0});
        expect(viewerResult.refusal == McpUiRefusal::InvalidArgument, "exposure out of range");
        (void)port.setViewer({.exposure = 0.0});
        viewerResult = port.setViewer({.zoom = mcp::McpViewerZoom::ActualSize});
        expect(viewerResult.ok() && !viewer->viewTransformForTest().fitToWindow &&
                   viewer->viewTransformForTest().zoom == 1.0,
               "zoom actual size");
        viewerResult = port.setViewer({.zoom = mcp::McpViewerZoom::Factor, .zoomFactor = 2.0});
        expect(viewerResult.ok() && viewer->viewTransformForTest().zoom == 2.0, "zoom factor");
        viewerResult = port.setViewer({.zoom = mcp::McpViewerZoom::Factor, .zoomFactor = 400.0});
        expect(viewerResult.refusal == McpUiRefusal::InvalidArgument, "zoom out of range");
        viewerResult = port.setViewer({.zoom = mcp::McpViewerZoom::Fit});
        expect(viewerResult.ok() && viewer->viewTransformForTest().fitToWindow, "zoom fit");
        viewerResult = port.setViewer({.display = std::string("No Such Display")});
        expect(viewerResult.refusal == McpUiRefusal::InvalidArgument &&
                   viewerResult.message.find("not offered") != std::string::npos,
               "an unknown display is refused with the choices");
        viewerResult = port.setViewer({.display = std::string(), .view = std::string()});
        expect(viewerResult.ok(), "empty display and view return to the project default");
    }

    expect(!host.isDirty() && session.snapshot().revision() == revision,
           "no session seam dirtied the project or entered its history");

    // ---- colour settings: project state, dirty like the dialog
    {
        const auto* current = host.colorSettings();
        const auto aces = current != nullptr
                              ? host::resolveProjectColorSettings(
                                    *current, host::ProjectColorConfig::AcesCg, std::nullopt)
                              : host::ProjectColorResolution{};
        expect(aces.settings.has_value(), "ACES settings resolve");
        if (aces.settings) {
            auto changed = port.setColorSettings(*aces.settings);
            expect(changed.ok() && host.isDirty() &&
                       host.colorSettings()->processColorSpaceId == aces.settings->processColorSpaceId,
                   "color settings apply and dirty the project");
            expect(session.colorSettings().processColorSpaceId == aces.settings->processColorSpaceId,
                   "the session follows at once");
            changed = port.setColorSettings(*aces.settings);
            expect(changed.ok(), "the same settings again is not an error");
        }
        result = runProject(binding,
                            request(mcp::McpProjectRequestKind::SaveAs, shot, false, false, true));
        expect(result.ok() && !host.isDirty(), "saved again");
    }

    // ---- Busy: a second lifecycle request while one runs
    {
        std::optional<mcp::McpProjectResult> first;
        binding.projectLifecycle(request(mcp::McpProjectRequestKind::Open, shot),
                                 [&](mcp::McpProjectResult value) { first = std::move(value); });
        const auto second = runProject(binding, request(mcp::McpProjectRequestKind::Save));
        expect(second.refusal == McpUiRefusal::Busy, "a request during another -> Busy");
        const auto colour = port.setColorSettings(*host.colorSettings());
        expect(colour.refusal == McpUiRefusal::Busy || first.has_value(),
               "color settings are Busy while a project opens");
        expect(waitFor([&] { return first.has_value(); }) && first->ok(), "the open finishes");
    }

    // ---- JobsRunning and stopJobs
    {
        auto* adapter = world.adapter.get();
        const auto added = adapter->addCompositionToQueue(session.compositionId());
        expect(added.accepted && added.addedJob.has_value(), "an export job is added");
        const auto snapshot = adapter->snapshot();
        if (added.accepted && snapshot && !snapshot->jobs.empty()) {
            const auto job = snapshot->jobs[0].id;
            const auto output = snapshot->jobs[0].outputs[0].id;
            document::ExportDestination destination;
            destination.portability = "external";
            destination.path =
                QUrl::fromLocalFile(out.filePath("shot.png")).toString().toStdString();
            destination.overwrite = document::ExportOverwritePolicy::Replace;
            (void)adapter->editOutput(job, output,
                                      ui::outputEdit([&](auto& e) { e.destination = destination; }));
            (void)adapter->editJob(job, ui::jobEdit([&](auto& e) {
                                       e.range = document::ExportRange{
                                           document::ExportRangeKind::Custom, 0, 239};
                                   }));
            expect(waitFor([&] {
                       adapter->drain();
                       return adapter->snapshot()->jobs[0]
                           .commands[host::ExportCommand::Start]
                           .available;
                   }),
                   "the job becomes startable");
            expect(adapter->start(job).accepted, "the job starts");
            expect(waitFor([&] {
                       adapter->drain();
                       return host.exportQueue() != nullptr &&
                              host.exportQueue()->activeJobCount() > 0;
                   }),
                   "an export is running");
            result = runProject(binding, request(mcp::McpProjectRequestKind::Open, shot, true));
            expect(result.refusal == McpUiRefusal::JobsRunning,
                   "Open while an export runs -> JobsRunning");
            result = runProject(binding, request(mcp::McpProjectRequestKind::New, {}, true));
            expect(result.refusal == McpUiRefusal::JobsRunning, "New while an export runs");
            result = runProject(binding, request(mcp::McpProjectRequestKind::Close, {}, true));
            expect(result.refusal == McpUiRefusal::JobsRunning, "Close while an export runs");
            expect(host.exportQueue()->activeJobCount() > 0, "the refusals left the export running");
            result = runProject(binding,
                                request(mcp::McpProjectRequestKind::Open, shot, true, true));
            expect(result.ok() && result.projectName == "SC020" &&
                       (host.exportQueue() == nullptr || host.exportQueue()->activeJobCount() == 0),
                   "stopJobs stops the export through the queue, then Open goes on");
        }
    }

    // ---- Failed: a file that is not a project
    {
        const auto bad = fs::path(projects.path().toStdString()) / "bad.bloom";
        std::ofstream(bad) << "this is not a bloom project";
        result = runProject(binding, request(mcp::McpProjectRequestKind::Open, bad, true));
        expect(result.refusal == McpUiRefusal::Failed && !result.message.empty(),
               "a file that will not open -> Failed, with a message");
        // The window tells the artist about a failed open in a dialog; until it is dismissed,
        // lifecycle calls answer Busy rather than stack on top of it.
        QTest::qWait(50);
        if (auto* dialog = QApplication::activeModalWidget()) {
            const auto busy = runProject(binding, request(mcp::McpProjectRequestKind::Save));
            expect(busy.refusal == McpUiRefusal::Busy, "a dialog on screen -> Busy");
            dialog->close();
            QTest::qWait(50);
        }
    }

    // ---- New and Close with discardChanges
    {
        (void)session.addSolidLayer("Edit", {0.1, 0.1, 0.1, 1.0});
        expect(host.isDirty(), "an edit dirties the project");
        result = runProject(binding, request(mcp::McpProjectRequestKind::New, {}, true));
        if (!result.ok())
            std::fprintf(stderr, "  New: %d %s\n", static_cast<int>(result.refusal), result.message.c_str());
        expect(result.ok() && result.projectName == "Untitled" && result.path.empty() &&
                   !host.isDirty(),
               "New with discardChanges opens an untitled project");
        result = runProject(binding, request(mcp::McpProjectRequestKind::Open, shot));
        expect(result.ok() && result.projectName == "SC020", "Open on a clean project");
        result = runProject(binding, request(mcp::McpProjectRequestKind::Close));
        expect(result.ok() && result.path.empty(), "Close on a clean project");
        // No composition now: the seams answer typed refusals, not crashes.
        expect(port.transport(transportRequest(mcp::McpTransportAction::Play)).refusal ==
                   McpUiRefusal::InvalidArgument,
               "transport with no composition");
        expect(!port.revealComposition(compositionId, std::nullopt),
               "reveal of a composition the new project lacks");
    }

    // ---- an agent's failed call never opens a dialog, and the next call is not Busy
    {
        const auto bad = fs::path(projects.path().toStdString()) / "corrupt.bloom";
        std::ofstream(bad) << "not a project";
        QTest::qWait(30);
        for (const auto& path : {bad, fs::path(projects.path().toStdString()) / "gone.bloom"}) {
            result = runProject(binding, request(mcp::McpProjectRequestKind::Open, path, true));
            QTest::qWait(60);
            expect(result.refusal == McpUiRefusal::Failed || result.refusal == McpUiRefusal::PathInvalid,
                   "an agent Open of a corrupt or missing file returns the refusal");
            expect(!result.message.empty(), "with a plain message");
            expect(QApplication::activeModalWidget() == nullptr, "and no dialog appears");
            expect(!host.agentOperation() && !host.isBusy(), "the host is free again");
            const auto next = runProject(binding, request(mcp::McpProjectRequestKind::New, {}, true));
            expect(next.ok(), "the next lifecycle call is not Busy");
        }
    }

    // ---- ReadOnly: a project the application preserved as it was
    {
        const auto preserved = fs::path(projects.path().toStdString()) / "preserved.bloom";
        {
            const auto bytes =
                ui::test::readonly_fixture::buildManifestSidePreservedReadOnlyArchiveOrAbort();
            std::ofstream stream(preserved, std::ios::binary);
            stream.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        }
        result = runProject(binding, request(mcp::McpProjectRequestKind::Open, preserved, true));
        expect(result.ok() && !host.canSave(), "a preserved read-only project opens");
        result = runProject(binding, request(mcp::McpProjectRequestKind::Save));
        expect(result.refusal == McpUiRefusal::ReadOnly, "Save on a read-only project -> ReadOnly");
        result = runProject(binding,
                            request(mcp::McpProjectRequestKind::SaveAs,
                                    fs::path(projects.path().toStdString()) / "copy.bloom"));
        expect(result.refusal == McpUiRefusal::ReadOnly, "SaveAs on a read-only project -> ReadOnly");
        const auto* settings = host.colorSettings();
        if (settings != nullptr) {
            const auto colour = port.setColorSettings(*settings);
            expect(colour.refusal == McpUiRefusal::ReadOnly || colour.ok(),
                   "color settings on a read-only project answer without changing it");
        }
        result = runProject(binding, request(mcp::McpProjectRequestKind::New));
        expect(result.ok(), "New leaves the read-only project");
    }

    // ---- no window: typed refusals
    {
        ui::McpLiveUiBinding headless(host, session, fixture.preview->playbackController());
        expect(!headless.revealComposition(compositionId, std::nullopt), "no window: reveal false");
        expect(headless.setViewer({.channel = mcp::McpViewerChannel::Red}).refusal ==
                   McpUiRefusal::UnknownTarget,
               "no window: viewer.set -> UnknownTarget");
    }
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("BloomMcpLiveUiTests"));
    QApplication::setApplicationName(QStringLiteral("McpLiveUi"));
    ui::kit::installKinetikTheme(application);
    testEnumTableParity();
    testCompileSubjectIsData();
    try {
        testBinding();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: exception: %s\n", error.what());
        ++gFailures;
    }
    if (gFailures != 0)
        std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
