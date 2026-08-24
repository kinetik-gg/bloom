#include <bloom/ui/main_window.hpp>

#include <QApplication>
#include <QComboBox>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    bloom::ui::MainWindow window;

    if (window.windowTitle() != "Bloom") {
        return 1;
    }

    if (window.centralWidget() == nullptr) {
        return 2;
    }

    const auto editorPickers = window.findChildren<QComboBox*>();
    if (editorPickers.size() != 5) {
        return 3;
    }

    for (const auto* editorPicker : editorPickers) {
        if (editorPicker->count() != 5) {
            return 4;
        }
    }

    return 0;
}
