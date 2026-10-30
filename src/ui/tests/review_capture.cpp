// Release review captures that no other capture program or test writes: the main window under the
// context bar of each tool group, a dropdown opened near the screen bottom (so it flips above its
// field), the searchable font dropdown, value fields editing and refusing text, and the Timeline
// with selected rows. Offscreen, at the device pixel ratio the process runs with.
//
// Usage: bloom_review_capture <directory> <1|1.5>
// Writes <directory>/<name>-dpr1.png or -dpr15.png; the exit status is the number of captures that
// could not be written.
#include "window_fixture.hpp"
#include <QGuiApplication>
#include <QLineEdit>
#include <QScreen>
#include <QVBoxLayout>
#include <QWidget>
#include <bloom/ui/context_bar.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/tool_controller.hpp>
#include <iostream>

namespace {
using namespace bloom::ui;
QString gDirectory;
QString gSuffix;
int gFailures = 0;

void settle(const int milliseconds = 60) {
    QApplication::processEvents();
    QTest::qWait(milliseconds);
    QApplication::processEvents();
}

void save(const QImage& image, const QString& name) {
    const auto path =
        gDirectory + QLatin1Char('/') + name + QLatin1Char('-') + gSuffix + QStringLiteral(".png");
    if (image.isNull() || !image.save(path)) {
        std::cerr << "Could not write " << path.toStdString() << '\n';
        ++gFailures;
    }
}

QWidget* popupOf(kit::KDropdown& dropdown) {
    return dropdown.findChild<QWidget*>(QStringLiteral("kDropdownPopup"));
}

void captureTools(test::WindowFixture& fixture) {
    auto& tools = fixture.session.tools();
    struct Group {
        const char* name;
        ViewerTool tool;
    };
    for (const auto& group : {Group{"select", ViewerTool::Select}, Group{"hand", ViewerTool::Hand},
                              Group{"shape", ViewerTool::Rectangle}, Group{"pen", ViewerTool::Pen},
                              Group{"text", ViewerTool::Text}}) {
        tools.setTool(group.tool);
        settle();
        save(fixture.window->grab().toImage(),
             QStringLiteral("review-window-tool-") + QLatin1String(group.name));
        if (auto* bar = fixture.window->contextBar())
            save(bar->grab().toImage(),
                 QStringLiteral("review-contextbar-") + QLatin1String(group.name));
    }
}

void captureFontDropdown(test::WindowFixture& fixture) {
    fixture.session.tools().setTool(ViewerTool::Text);
    settle();
    auto* font = fixture.window->contextBar()->findChild<kit::KDropdown*>(
        QStringLiteral("contextBarTextFont"));
    if (font == nullptr) {
        ++gFailures;
        return;
    }
    font->showPopup();
    settle(150);
    if (auto* popup = popupOf(*font))
        save(popup->grab().toImage(), QStringLiteral("review-font-dropdown"));
    else
        ++gFailures;
    font->hidePopup();
    fixture.session.tools().setTool(ViewerTool::Select);
    settle();
}

// A dropdown whose field sits at the bottom of the screen: its popup opens above the field.
void captureFlippedDropdown() {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addStretch(1);
    auto* dropdown = new kit::KDropdown(&host);
    for (const char* item : {"Normal", "Add", "Multiply", "Screen", "Overlay", "Darken", "Lighten",
                             "Difference", "Exclusion", "Linear Dodge"})
        dropdown->addItem(QString::fromLatin1(item));
    layout->addWidget(dropdown);
    host.resize(360, 220);
    const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
    host.move(screen.left() + 40, screen.bottom() - host.height() - 8);
    host.show();
    settle();
    dropdown->showPopup();
    settle(150);
    if (auto* popup = popupOf(*dropdown)) {
        const bool above =
            popup->mapToGlobal(QPoint(0, 0)).y() < dropdown->mapToGlobal(QPoint(0, 0)).y();
        std::cout << "flipped dropdown opens above its field: " << (above ? "yes" : "NO") << '\n';
        if (!above)
            ++gFailures;
        save(popup->grab().toImage(), QStringLiteral("review-dropdown-flipped-popup"));
    } else {
        ++gFailures;
    }
    save(host.grab().toImage(), QStringLiteral("review-dropdown-flipped-host"));
    dropdown->hidePopup();
}

void captureValueFields() {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* rest = new kit::KValueField(&host);
    auto* editing = new kit::KValueField(&host);
    auto* invalid = new kit::KValueField(&host);
    for (auto* field : {rest, editing, invalid}) {
        field->setOutlinedAtRest(true);
        field->setRange(0.0, 100.0);
        field->setValue(42.0);
        field->setUnit(QStringLiteral("%"));
        layout->addWidget(field);
    }
    host.resize(260, 140);
    host.show();
    settle();
    save(host.grab().toImage(), QStringLiteral("review-value-field-rest"));
    editing->setFocus();
    QTest::keyClick(editing, Qt::Key_Return);
    settle();
    if (editing->lineEdit() != nullptr) {
        editing->lineEdit()->selectAll();
        QTest::keyClicks(editing->lineEdit(), QStringLiteral("61"));
    }
    invalid->setFocus();
    QTest::keyClick(invalid, Qt::Key_Return);
    settle();
    if (invalid->lineEdit() != nullptr) {
        invalid->lineEdit()->selectAll();
        QTest::keyClicks(invalid->lineEdit(), QStringLiteral("250"));
        QTest::keyClick(invalid->lineEdit(), Qt::Key_Return);
    }
    settle();
    save(host.grab().toImage(), QStringLiteral("review-value-field-editing-invalid"));
    std::cout << "invalid field refusal: " << invalid->refusal().toStdString() << '\n';
    if (invalid->refusal().isEmpty())
        ++gFailures;
}

void captureTimeline(test::WindowFixture& fixture) {
    for (auto* area : fixture.window->findChildren<EditorArea*>())
        if (area->editorId() == "bloom.timeline") {
            settle();
            save(area->grab().toImage(), QStringLiteral("review-timeline-selection"));
            return;
        }
    ++gFailures;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: bloom_review_capture <directory> <1|1.5>\n";
        return 2;
    }
    qputenv("QT_SCALE_FACTOR", argv[2]);
    QApplication app(argc, argv);
    app.setOrganizationName("BloomReviewCapture");
    app.setApplicationName("ReviewCapture");
    kit::installKinetikTheme(app);
    gDirectory = QString::fromLocal8Bit(argv[1]);
    gSuffix = QString::fromLatin1(argv[2]).remove(QLatin1Char('.'));
    gSuffix = (gSuffix == QStringLiteral("1") ? QStringLiteral("dpr1") : QStringLiteral("dpr15"));
    try {
        test::WindowFixture fixture;
        fixture.clearInteraction();
        settle(200);
        captureTimeline(fixture);
        captureTools(fixture);
        captureFontDropdown(fixture);
        captureFlippedDropdown();
        captureValueFields();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return gFailures;
}
