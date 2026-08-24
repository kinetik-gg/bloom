#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/main_window.hpp>

#include <QApplication>
#include <QComboBox>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    bloom::ui::EditorRegistry editorRegistry;
    if (!bloom::ui::registerFoundationEditors(editorRegistry)) {
        return 1;
    }

    if (editorRegistry.editors().size() != 5) {
        return 2;
    }

    const bool duplicateRegistered =
        editorRegistry.registerEditor(editorRegistry.editors().front());
    if (duplicateRegistered) {
        return 3;
    }

    bloom::ui::MainWindow window(editorRegistry);

    if (window.windowTitle() != "Bloom") {
        return 4;
    }

    if (window.centralWidget() == nullptr) {
        return 5;
    }

    const auto editorPickers = window.findChildren<QComboBox*>();
    if (editorPickers.size() != 5) {
        return 6;
    }

    for (const auto* editorPicker : editorPickers) {
        if (editorPicker->count() != 5) {
            return 7;
        }
    }

    return 0;
}
