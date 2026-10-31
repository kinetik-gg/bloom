// The Viewer's Display / View: it starts on the config's untone-mapped view (never the ACES CG
// config's own "ACES 1.0 - SDR Video" tone map) and remembers the artist's choice per project,
// across composition switches, Viewer re-creation and project reopen
// (docs/architecture/color-management.md, "Viewer display/view and look selection").

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/host/bloom_neutral_profile.hpp>
#include <bloom/host/project_color_choice.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QTemporaryDir>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>

namespace {

using namespace std::chrono_literals;
using namespace bloom;

int failures = 0;

void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

const QStringList kUntoneMapped{QStringLiteral("sRGB - Display"), QStringLiteral("Un-tone-mapped")};
const QStringList kSdrVideo{QStringLiteral("sRGB - Display"),
                            QStringLiteral("ACES 1.0 - SDR Video")};

document::NewProject makeProject() {
    const auto format = document::CompositionFormat::create(4, 3);
    if (!format)
        std::abort();
    return document::makeNewProject("Display View", "Main", core::RationalTime::fromInteger(10),
                                    *format);
}

document::ColorSettings acesSettings(const document::ColorSettings& neutral) {
    auto resolved =
        host::resolveProjectColorSettings(neutral, host::ProjectColorConfig::AcesCg, std::nullopt);
    if (!resolved.settings)
        std::abort();
    return *resolved.settings;
}

struct Pipeline final {
    runtime::NodeDefinitionRegistry definitions;
    runtime::SnapshotCompiler compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    ui::PreviewPreparationFunction function;

    Pipeline() : compiler(definitions) {
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            std::abort();
        definitions.freeze();
        function = ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                      qualifiedProcessorProvider);
    }
};

// One project open in a session with a preview controller; Viewers come and go over it.
struct Fixture final {
    document::Document document;
    commands::CommandStack commands;
    ui::CompositionSession session;
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge;
    Pipeline pipeline;
    ui::CompositionPreviewController controller;

    explicit Fixture(document::NewProject project)
        : document(std::move(project.project)), commands(document),
          session(document, commands, project.initialCompositionId),
          scheduler(runtime::TaskSchedulerConfig{.cpuWorkerCount = 1,
                                                 .blockingIoWorkerCount = 1,
                                                 .cpuQueueCapacity = 16,
                                                 .blockingIoQueueCapacity = 4,
                                                 .terminalHistoryCapacity = 32,
                                                 .diagnosticsPerTask = 8,
                                                 .groupRegistryCapacity = 8}),
          bridge(scheduler, nullptr, 1ms),
          controller(session, scheduler, bridge, pipeline.function) {}

    ~Fixture() {
        controller.beginShutdown();
        bridge.beginShutdown();
        QElapsedTimer timer;
        timer.start();
        while (!scheduler.isQuiescent() && timer.elapsed() < 4'000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
};

ui::kit::KDropdown* displayView(ui::ViewerEditor& viewer) {
    return viewer.findChild<ui::kit::KDropdown*>(QStringLiteral("viewerDisplayView"));
}

QStringList shown(ui::ViewerEditor& viewer) {
    auto* dropdown = displayView(viewer);
    return dropdown == nullptr ? QStringList{}
                               : dropdown->itemData(dropdown->currentIndex()).toStringList();
}

bool choose(ui::ViewerEditor& viewer, const QStringList& pair) {
    auto* dropdown = displayView(viewer);
    if (dropdown == nullptr)
        return false;
    for (int index = 0; index < dropdown->count(); ++index)
        if (dropdown->itemData(index).toStringList() == pair) {
            dropdown->setCurrentIndex(index);
            return true;
        }
    return false;
}

bool controllerShows(const Fixture& fixture, const QStringList& pair) {
    return fixture.controller.settings().displayName == pair[0].toStdString() &&
           fixture.controller.settings().viewName == pair[1].toStdString();
}

void testNeutralStartsOnSrgb() {
    Fixture fixture(makeProject());
    fixture.session.setColorSettings(
        document::makeBloomNeutralColorSettingsV1(host::kBloomNeutralV1ConfigDigest));
    ui::ViewerEditor viewer(fixture.session, fixture.controller);
    expect(shown(viewer) == QStringList({QStringLiteral("srgb_rec709_display"),
                                         QStringLiteral("srgb_rec709_display")}),
           "a Bloom Neutral project shows its single sRGB display and view");
}

void testAcesStartsUntoneMappedAndRemembersInSession() {
    Fixture fixture(makeProject());
    fixture.session.viewerDisplayViewMemory().beginProject(std::nullopt);
    fixture.session.setColorSettings(acesSettings(fixture.session.colorSettings()));
    auto viewer = std::make_unique<ui::ViewerEditor>(fixture.session, fixture.controller);
    expect(shown(*viewer) == kUntoneMapped && controllerShows(fixture, kUntoneMapped),
           "an ACES CG project starts on sRGB - Display / Un-tone-mapped, not the ACES tone map");

    expect(choose(*viewer, kSdrVideo) && controllerShows(fixture, kSdrVideo),
           "the artist can still choose the ACES 1.0 SDR Video view");

    // Another composition keeps the choice.
    commands::Transaction add("Add composition", fixture.document.snapshot().revision());
    add.emplace<commands::AddComposition>("Second", *document::CompositionFormat::create(4, 3),
                                          core::RationalTime::fromInteger(5));
    expect(fixture.session.executeTransaction(std::move(add)).succeeded(),
           "a second composition is added");
    const auto snapshot = fixture.document.snapshot();
    const auto compositions = snapshot.project().compositions();
    expect(compositions.size() == 2 && fixture.session.setComposition(compositions.back().id()),
           "the Viewer switches to the second composition");
    expect(shown(*viewer) == kSdrVideo && controllerShows(fixture, kSdrVideo),
           "a composition switch keeps the chosen display and view");

    // A Viewer opened again on the same (unsaved) project starts on the remembered choice.
    viewer.reset();
    viewer = std::make_unique<ui::ViewerEditor>(fixture.session, fixture.controller);
    expect(shown(*viewer) == kSdrVideo,
           "a re-created Viewer starts on the project's remembered choice");
}

void testSavedProjectRemembersAcrossReopen(const std::filesystem::path& directory) {
    const auto saved = directory / "remembered.bloom";
    const auto older = directory / "older.bloom";
    {
        Fixture fixture(makeProject());
        fixture.session.viewerDisplayViewMemory().beginProject(saved);
        fixture.session.setColorSettings(acesSettings(fixture.session.colorSettings()));
        ui::ViewerEditor viewer(fixture.session, fixture.controller);
        expect(choose(viewer, kSdrVideo), "the artist chooses SDR Video in a saved project");

        // Open replaces the project in this same session: the other project has no remembered
        // choice, so it starts on the untone-mapped default although its color settings are equal.
        auto other = makeProject();
        document::Document otherDocument(std::move(other.project));
        commands::CommandStack otherCommands(otherDocument);
        fixture.session.viewerDisplayViewMemory().beginProject(older);
        fixture.session.rebind(otherDocument, otherCommands, other.initialCompositionId);
        expect(shown(viewer) == kUntoneMapped && controllerShows(fixture, kUntoneMapped),
               "opening a project with no remembered choice shows Un-tone-mapped");
        fixture.session.viewerDisplayViewMemory().beginProject(saved);
        const auto snapshot = fixture.document.snapshot();
        fixture.session.rebind(fixture.document, fixture.commands,
                               snapshot.project().compositions().front().id());
        expect(shown(viewer) == kSdrVideo && controllerShows(fixture, kSdrVideo),
               "reopening the saved project restores its chosen view");
    }
    // A new application run: nothing in memory, the choice comes back from the saved settings.
    {
        Fixture fixture(makeProject());
        fixture.session.viewerDisplayViewMemory().beginProject(saved);
        fixture.session.setColorSettings(acesSettings(fixture.session.colorSettings()));
        ui::ViewerEditor viewer(fixture.session, fixture.controller);
        expect(shown(viewer) == kSdrVideo,
               "the saved project's view survives closing and reopening Bloom");
    }
    {
        Fixture fixture(makeProject());
        fixture.session.viewerDisplayViewMemory().beginProject(older);
        fixture.session.setColorSettings(acesSettings(fixture.session.colorSettings()));
        ui::ViewerEditor viewer(fixture.session, fixture.controller);
        expect(shown(viewer) == kUntoneMapped,
               "an older project with no remembered view opens un-tone-mapped");
    }
}

void testSaveAsCarriesTheChoice(const std::filesystem::path& directory) {
    const auto target = directory / "saved-as.bloom";
    {
        Fixture fixture(makeProject());
        fixture.session.viewerDisplayViewMemory().beginProject(std::nullopt);
        fixture.session.setColorSettings(acesSettings(fixture.session.colorSettings()));
        ui::ViewerEditor viewer(fixture.session, fixture.controller);
        expect(choose(viewer, kSdrVideo), "the artist chooses SDR Video before the first save");
        fixture.session.viewerDisplayViewMemory().moveProject(target);
    }
    Fixture fixture(makeProject());
    fixture.session.viewerDisplayViewMemory().beginProject(target);
    fixture.session.setColorSettings(acesSettings(fixture.session.colorSettings()));
    ui::ViewerEditor viewer(fixture.session, fixture.controller);
    expect(shown(viewer) == kSdrVideo, "Save As keeps the unsaved project's chosen view");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    QTemporaryDir settingsDirectory;
    QTemporaryDir projectDirectory;
    QCoreApplication::setOrganizationName("BloomTests");
    QCoreApplication::setApplicationName("ViewerDisplayView");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    const std::filesystem::path projects(projectDirectory.path().toStdString());
    testNeutralStartsOnSrgb();
    testAcesStartsUntoneMappedAndRemembersInSession();
    testSavedProjectRemembersAcrossReopen(projects);
    testSaveAsCarriesTheChoice(projects);
    if (failures == 0)
        std::cout << "viewer display/view: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
