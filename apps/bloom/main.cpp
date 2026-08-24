#include <bloom/commands/command_stack.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/main_window.hpp>

#include <QApplication>
#include <QCoreApplication>
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

    bloom::ui::EditorRegistry editorRegistry;
    if (!bloom::ui::registerFoundationEditors(editorRegistry, compositionSession)) {
        return 1;
    }

    QSettings settings;
    bloom::ui::MainWindow window(editorRegistry, compositionSession);
    (void)window.restoreApplicationState(settings);
    QObject::connect(&application, &QApplication::aboutToQuit, &window, [&window, &settings] {
        window.saveApplicationState(settings);
        settings.sync();
    });
    window.show();

    return application.exec();
}
