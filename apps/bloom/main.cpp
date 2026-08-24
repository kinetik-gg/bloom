#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/main_window.hpp>

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);

    QCoreApplication::setApplicationName("Bloom");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCoreApplication::setOrganizationName("Kinetik");

    bloom::ui::EditorRegistry editorRegistry;
    if (!bloom::ui::registerFoundationEditors(editorRegistry)) {
        return 1;
    }

    bloom::ui::MainWindow window(editorRegistry);
    window.show();

    return application.exec();
}
