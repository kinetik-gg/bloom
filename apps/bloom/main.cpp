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
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/qualified_display_processor_bootstrap.hpp>
#include <bloom/ui/task_monitor_model.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QSettings>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);

    QCoreApplication::setApplicationName("Bloom");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCoreApplication::setOrganizationName("Kinetik");

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
    bloom::ui::CompositionPreviewController previewController(
        compositionSession, taskScheduler, taskUiBridge,
        bloom::ui::makeCompositionPreviewPipeline(snapshotCompiler, cpuEvaluator,
                                                  referenceDisplayPreparer,
                                                  qualifiedDisplayProcessorProvider));
    bloom::ui::ApplicationShutdownCoordinator shutdownCoordinator(previewController, taskUiBridge);
    application.installEventFilter(&shutdownCoordinator);
    bloom::ui::TaskMonitorModel taskMonitor(taskUiBridge);

    // "File -> Export Frame..." (task F3, issue #103): binds to the SAME application-wide
    // PublicationCoordinator/StagedArtifactCoordinator ProjectHost already owns (docs/architecture/
    // frame-output.md, "Capability Boundary": saves and exports share one coordinator pair for
    // correct same-target ordering/supersession), and to the SAME SnapshotCompiler/TaskScheduler/
    // TaskUiBridge the preview pipeline above already uses.
    bloom::ui::FrameExportController frameExportController(
        compositionSession, taskScheduler, taskUiBridge, snapshotCompiler,
        projectHost.publicationCoordinator(), projectHost.artifactCoordinator());

    bloom::ui::EditorRegistry editorRegistry;
    const bool editorsRegistered = bloom::ui::registerFoundationEditors(
                                       editorRegistry, compositionSession, previewController) &&
                                   bloom::ui::registerJobsEditor(editorRegistry, taskMonitor);
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
    bloom::ui::MainWindow window(editorRegistry, compositionSession, projectHost,
                                 frameExportController);
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

    return application.exec();
}
