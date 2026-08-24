#include <bloom/commands/command_stack.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/application_shutdown_coordinator.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/jobs_editor.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/task_monitor_model.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QSettings>

#include <utility>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);

    QCoreApplication::setApplicationName("Bloom");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCoreApplication::setOrganizationName("Kinetik");

    auto newProject = bloom::document::makeNewProject("Untitled", "Composition 1",
                                                      bloom::core::RationalTime::fromInteger(10));
    const auto initialCompositionId = newProject.initialCompositionId;
    bloom::document::Document document(std::move(newProject.project));
    bloom::commands::CommandStack commandStack(document);
    bloom::ui::CompositionSession compositionSession(document, commandStack, initialCompositionId);

    bloom::runtime::NodeDefinitionRegistry nodeDefinitions;
    if (!bloom::runtime::registerBuiltInNodeDefinitions(nodeDefinitions)) {
        return 1;
    }
    nodeDefinitions.freeze();
    bloom::runtime::SnapshotCompiler snapshotCompiler(nodeDefinitions);
    bloom::runtime::CpuCompositionEvaluator cpuEvaluator;
    bloom::runtime::TaskScheduler taskScheduler;
    bloom::ui::TaskUiBridge taskUiBridge(taskScheduler);
    bloom::ui::CompositionPreviewController previewController(
        compositionSession, taskScheduler, taskUiBridge,
        bloom::ui::makeCompositionPreviewPipeline(snapshotCompiler, cpuEvaluator));
    bloom::ui::ApplicationShutdownCoordinator shutdownCoordinator(previewController, taskUiBridge);
    application.installEventFilter(&shutdownCoordinator);
    bloom::ui::TaskMonitorModel taskMonitor(taskUiBridge);

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
    bloom::ui::MainWindow window(editorRegistry, compositionSession);
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
