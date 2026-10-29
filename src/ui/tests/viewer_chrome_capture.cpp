// Real-window captures of the maximized Viewer panel at a small and a large size, with and without
// a composition. Writes PNGs to BLOOM_GRAMMAR_ARTIFACT_DIR (or the directory given as the first
// argument) and checks the hosted chrome: no header or footer control lies outside its row.
#include "window_fixture.hpp"
#include <QDir>
#include <QImage>
#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <iostream>

namespace {
using namespace bloom::ui;

EditorArea* viewerArea(MainWindow& window) {
    for (auto* area : window.findChildren<EditorArea*>())
        if (area->editorId() == "bloom.viewer")
            return area;
    return nullptr;
}

// Resizes the window until the maximized Viewer area is `target` logical pixels.
void sizeViewerArea(MainWindow& window, EditorArea& area, const QSize target) {
    for (int pass = 0; pass < 3; ++pass) {
        QApplication::processEvents();
        const QSize delta = target - area.size();
        if (delta.isNull())
            return;
        window.resize(window.size() + delta);
        QTest::qWait(30);
    }
}

int checkRows(EditorArea& area, const char* label) {
    int failures = 0;
    for (const char* rowName : {"editorFooter", "editorHeader"}) {
        auto* row = area.findChild<QWidget*>(QString::fromLatin1(rowName));
        if (row == nullptr)
            continue;
        const QRect bounds = row->rect();
        for (auto* control : row->findChildren<QWidget*>()) {
            if (!control->isVisible() || control->isWindow() || !control->property("chromeControl").toBool())
                continue;
            const QRect placed(control->mapTo(row, QPoint()), control->size());
            if (!bounds.contains(placed)) {
                ++failures;
                std::cerr << label << ": " << control->objectName().toStdString()
                          << " lies outside " << rowName << '\n';
            }
        }
    }
    return failures;
}

int run(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomGrammarTests");
    app.setApplicationName("ViewerChromeCapture");
    kit::installKinetikTheme(app);
    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                       : QStringLiteral(BLOOM_GRAMMAR_ARTIFACT_DIR);
    const QString stage = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("current");
    QDir().mkpath(directory);
    test::WindowFixture fixture;
    const auto ratio = fixture.window->devicePixelRatioF();
    const QString dpr = ratio == 1.0 ? QStringLiteral("dpr1") : QStringLiteral("dpr15");
    auto* area = viewerArea(*fixture.window);
    if (area == nullptr) {
        std::cerr << "no Viewer area\n";
        return 1;
    }
    Q_EMIT area->maximizeRequested(area);
    QTest::qWait(50);
    int failures = 0;
    const auto capture = [&](const char* state) {
        for (const auto& [name, size] :
             {std::pair{"small", QSize(800, 600)}, std::pair{"large", QSize(1900, 1000)}}) {
            sizeViewerArea(*fixture.window, *area, size);
            fixture.clearInteraction();
            QTest::qWait(50);
            const QString label =
                QStringLiteral("viewer-%1-%2-%3-%4").arg(stage, QString::fromLatin1(state),
                                                        QString::fromLatin1(name), dpr);
            failures += checkRows(*area, label.toStdString().c_str());
            const auto image = area->grab().toImage();
            if (!image.save(directory + QLatin1Char('/') + label + QStringLiteral(".png")))
                ++failures;
            std::cout << label.toStdString() << ' ' << area->width() << 'x' << area->height()
                      << '\n';
        }
    };
    capture("composition");
    if (!deleteComposition(fixture.session, fixture.session.compositionId())) {
        std::cerr << "could not delete the fixture composition\n";
        return 1;
    }
    QTest::qWait(200);
    capture("empty");
    return failures == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
