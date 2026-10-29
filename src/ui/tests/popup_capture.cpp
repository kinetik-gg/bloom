// Opens one kit dropdown popup or one kit menu in a real window and holds it open, so a compositor
// screenshot (or, offscreen, a grab) shows exactly what the artist sees under the last row: the
// popup chin check. Usage: bloom_popup_capture <dropdown|menu> [seconds] [grab.png]
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/theme.hpp>

#include <QApplication>
#include <QHBoxLayout>
#include <QMenu>
#include <QPixmap>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    bloom::ui::kit::installKinetikTheme(application);
    const QString mode = argc > 1 ? QString::fromLatin1(argv[1]) : QStringLiteral("dropdown");
    const int seconds = argc > 2 ? QString::fromLatin1(argv[2]).toInt() : 6;
    const QString grabPath = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QString{};

    QWidget window;
    window.setWindowTitle(QStringLiteral("Popup capture"));
    auto* layout = new QVBoxLayout(&window);
    auto* row = new QHBoxLayout;
    layout->addLayout(row);
    auto* dropdown = new bloom::ui::kit::KDropdown(&window);
    for (const char* name : {"Normal", "Add", "Multiply", "Screen", "Overlay", "Darken", "Lighten",
                             "Difference", "Exclusion", "Linear Dodge"})
        dropdown->addItem(QString::fromLatin1(name));
    row->addWidget(dropdown);
    row->addStretch(1);
    layout->addStretch(1);
    window.resize(480, 420);
    window.show();

    QMenu* menu = bloom::ui::kit::makeMenu(&window);
    for (const char* name : {"Fit", "100%", "Zoom In", "Zoom Out", "Guides"})
        menu->addAction(QString::fromLatin1(name));

    QTimer::singleShot(600, &window, [&] {
        if (mode == QStringLiteral("menu"))
            menu->popup(window.mapToGlobal(QPoint(40, 60)));
        else
            dropdown->showPopup();
    });
    // A headless compositor has no seat, so an xdg_popup's grab is refused and the popup closes at
    // once. --as-window re-shows the same translucent widget as a frameless tool window instead,
    // which the compositor still blends per pixel: what lies under the last row is unchanged.
    const bool asWindow = qEnvironmentVariableIsSet("BLOOM_CAPTURE_AS_WINDOW");
    if (asWindow) {
        QTimer::singleShot(900, &window, [&] {
            QWidget* popup = mode == QStringLiteral("menu") ? static_cast<QWidget*>(menu)
                                                             : dropdown->findChild<QWidget*>(
                                                                   QStringLiteral("kDropdownPopup"));
            if (popup == nullptr)
                return;
            const QSize size = popup->size();
            popup->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
            popup->resize(size);
            popup->show();
        });
    }
    if (!grabPath.isEmpty()) {
        QTimer::singleShot(1200, &window, [&] {
            QWidget* popup = mode == QStringLiteral("menu") ? static_cast<QWidget*>(menu)
                                                             : dropdown->findChild<QWidget*>(
                                                                   QStringLiteral("kDropdownPopup"));
            if (popup != nullptr)
                popup->grab().save(grabPath);
        });
    }
    QTimer::singleShot(seconds * 1000, &application, &QApplication::quit);
    return QApplication::exec();
}
