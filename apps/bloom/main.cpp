#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/application_shutdown_coordinator.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/frame_export_controller.hpp>
#include <bloom/ui/jobs_editor.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/qualified_display_processor_bootstrap.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/task_monitor_model.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QSettings>

#include <memory>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);

    QCoreApplication::setApplicationName("Bloom");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCoreApplication::setOrganizationName("Kinetik");

    // The Kinetik visual language is installed once, application-wide, before any widget exists
    // (task U1, issue #117). It replaces MainWindow::applyFoundationTheme(): the palette, the
    // application stylesheet, and the interface font belong to the application, not to one window,
    // and installing them first means no surface is ever constructed under the default Qt theme.
    bloom::ui::kit::installKinetikTheme(application);
    // Mnemonic underlines only while Alt is held (task U2, issue #118, decision 2): a small
    // QProxyStyle installed application-wide, right after the theme's own Fusion style, so every
    // menu -- title-bar-embedded or classic -- shares the same behavior. QApplication::setStyle()
    // takes ownership of the style object (Qt's own documented contract); the static analyzer
    // cannot see through that ownership transfer and reports a path-sensitive "leak" anchored
    // wherever later in main() its analysis happens to conclude the object is unreachable, not at
    // this allocation -- hence the function-scoped suppression markers below rather than a
    // single-line one.
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks) -- QApplication::setStyle() owns it.
    auto* mnemonicStyle = new bloom::ui::kit::AltUnderlineProxyStyle();
    QApplication::setStyle(mnemonicStyle);

    // ProjectHost (task U1, issue #72) replaces the hand-rolled document/command-stack pair: it
    // owns the application's single live bloom::host::ProjectSession and constructs an initial
    // createNew() project itself. taskScheduler is shared with CompositionPreviewController below
    // (both need a TaskExecutor::BlockingIo-capable scheduler; TaskSchedulerConfig::defaults()
    // already provisions one BlockingIo worker, so a single scheduler instance serves both).
    bloom::runtime::TaskScheduler taskScheduler;
    bloom::ui::ProjectHost projectHost(taskScheduler);

    auto [initialDocument, initialCommandStack] = projectHost.liveDocumentAndStack();
    if (initialDocument == nullptr || initialCommandStack == nullptr) {
        return 1;
    }
    bloom::ui::CompositionSession compositionSession(*initialDocument, *initialCommandStack,
                                                     projectHost.lowestCompositionId());

    // Projection rebinding (decision 2): every time ProjectHost replaces the live session content
    // (New or a successful Open install), rebind CompositionSession to whatever document/command-
    // stack pair is now live. A preserved-read-only install has no document/command-stack at all
    // (liveDocumentAndStack() returns null), so this lambda intentionally skips rebinding and
    // leaves CompositionSession bound to whatever it projected before. That skip is now
    // intentional and safe rather than a known limitation (task R1, issue #74): MainWindow hides
    // the entire editor workspace behind a presentation-level read-only placeholder page whenever
    // ProjectHost's content kind is PreservedReadOnly (see MainWindow::updateContentSurface()), so
    // the stale CompositionSession this lambda leaves bound is never shown to the artist. Returning
    // to decoded content (a New or an editable Open) switches the workspace back into view and this
    // lambda rebinds normally.
    QObject::connect(&projectHost, &bloom::ui::ProjectHost::sessionReplaced, &compositionSession,
                     [&projectHost, &compositionSession] {
                         auto [document, commandStack] = projectHost.liveDocumentAndStack();
                         if (document == nullptr || commandStack == nullptr) {
                             return;
                         }
                         compositionSession.rebind(*document, *commandStack,
                                                   projectHost.lowestCompositionId());
                     });

    bloom::runtime::NodeDefinitionRegistry nodeDefinitions;
    if (!bloom::runtime::registerBuiltInNodeDefinitions(nodeDefinitions)) {
        return 1;
    }
    nodeDefinitions.freeze();
    bloom::runtime::SnapshotCompiler snapshotCompiler(nodeDefinitions);
    bloom::runtime::CpuCompositionEvaluator cpuEvaluator;
    bloom::runtime::CpuReferenceDisplayPreparer referenceDisplayPreparer;
    // Issue #97 (task C3): resolved and built once, on the shared TaskScheduler's blocking-I/O
    // lane, at this same session/pipeline-construction point (design decision 3). Declared before
    // taskUiBridge/qualifiedDisplayProcessorBootstrap/previewController so it outlives every
    // reference into it those objects hold (locals destruct in reverse declaration order).
    bloom::runtime::QualifiedDisplayProcessorProvider qualifiedDisplayProcessorProvider;
    bloom::ui::TaskUiBridge taskUiBridge(taskScheduler);
    bloom::ui::QualifiedDisplayProcessorBootstrap qualifiedDisplayProcessorBootstrap(
        taskScheduler, taskUiBridge, qualifiedDisplayProcessorProvider);
    // The RAM preview cache and the preview pipeline are built HERE, once, because two surfaces
    // share each of them: the preview controller and the RAM preview controller both render through
    // the one pipeline (and so through the one compiled-plan cache inside it), and both put frames
    // into the one frame cache. The cache's budget is the artist's setting (task PERF1, item 2);
    // `settings` is declared below for window state, so this read opens its own short-lived
    // QSettings over the same organization/application keys rather than moving that declaration up
    // here.
    const auto ramPreviewByteBudget = [] {
        const QSettings playbackSettings;
        return bloom::ui::ramPreviewByteBudgetFromSettings(playbackSettings);
    }();
    auto previewFrameCache = std::make_shared<bloom::ui::PreviewFrameCache>(ramPreviewByteBudget);
    const auto previewPipeline = bloom::ui::makeCompositionPreviewPipeline(
        snapshotCompiler, cpuEvaluator, referenceDisplayPreparer,
        qualifiedDisplayProcessorProvider);
    bloom::ui::CompositionPreviewController previewController(
        compositionSession, taskScheduler, taskUiBridge, previewPipeline, {}, previewFrameCache);
    bloom::ui::RamPreviewController ramPreviewController(
        compositionSession, previewController, taskScheduler, taskUiBridge, previewPipeline);
    bloom::ui::ApplicationShutdownCoordinator shutdownCoordinator(previewController, taskUiBridge);
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownStarted,
                     &ramPreviewController, &bloom::ui::RamPreviewController::beginShutdown);
    application.installEventFilter(&shutdownCoordinator);
    // Kept live even though no editor shows it (task F1, item F6 removed Jobs from the registry
    // below): this is the model a JobsEditor takes, and it is the bridge's own consumer. Dropping
    // it would change what happens to task-bridge state, which item F6 does not ask for.
    bloom::ui::TaskMonitorModel taskMonitor(taskUiBridge);

    // "File -> Export Frame..." (task F3, issue #103): binds to the SAME application-wide
    // PublicationCoordinator/StagedArtifactCoordinator ProjectHost already owns (docs/architecture/
    // frame-output.md, "Capability Boundary": saves and exports share one coordinator pair for
    // correct same-target ordering/supersession), and to the SAME SnapshotCompiler/TaskScheduler/
    // TaskUiBridge the preview pipeline above already uses.
    bloom::ui::FrameExportController frameExportController(
        compositionSession, taskScheduler, taskUiBridge, snapshotCompiler,
        projectHost.publicationCoordinator(), projectHost.artifactCoordinator(), {},
        &qualifiedDisplayProcessorProvider);

    bloom::ui::EditorRegistry editorRegistry;
    // Jobs is deliberately NOT registered (task F1, item F6). An editor in this registry is an
    // editor the panel switcher offers and a workspace can place, and Jobs is wanted in neither
    // for now. The JobsEditor class and registerJobsEditor() both survive untouched -- the single
    // `&& bloom::ui::registerJobsEditor(editorRegistry, taskMonitor)` this line used to carry is
    // all it takes to offer the panel again -- so Jobs is reachable programmatically and simply
    // not on offer in the interface.
    const bool editorsRegistered = bloom::ui::registerFoundationEditors(
        editorRegistry, compositionSession, previewController, &ramPreviewController);
    if (!editorsRegistered) {
        QEventLoop shutdownLoop;
        QObject::connect(&shutdownCoordinator,
                         &bloom::ui::ApplicationShutdownCoordinator::shutdownQuiescent,
                         &shutdownLoop, &QEventLoop::quit);
        shutdownCoordinator.beginShutdown();
        if (!taskScheduler.isQuiescent()) {
            shutdownLoop.exec();
        }
        return 1;
    }

    application.setQuitOnLastWindowClosed(false);
    QSettings settings;
    // Native (server-side) window chrome only (task C1): MainWindow no longer takes a chrome mode
    // at all -- there is nothing left for main() to read from settings before constructing it.
    bloom::ui::MainWindow window(editorRegistry, compositionSession, projectHost,
                                 frameExportController, &ramPreviewController);
    (void)window.restoreApplicationState(settings);
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownStarted, &window,
                     [&window, &settings] { window.saveApplicationState(settings); });
    QObject::connect(&window, &bloom::ui::MainWindow::shutdownRequested, &shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::beginShutdown);
    QObject::connect(&shutdownCoordinator,
                     &bloom::ui::ApplicationShutdownCoordinator::shutdownQuiescent, &application,
                     &QApplication::quit);
    window.show();

    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
    return application.exec();
}
