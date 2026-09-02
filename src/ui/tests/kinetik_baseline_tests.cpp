#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QApplication>
#include <QComboBox>
#include <QImage>
#include <QLineEdit>
#include <QPixmap>
#include <QSplitter>
#include <QString>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using namespace bloom::ui;

EditorRegistry makeRegistry() {
    EditorRegistry registry;
    (void)registry.registerEditor(
        {"bloom.probe", "Probe", [](QWidget* parent) -> QWidget* { return new QWidget(parent); }});
    return registry;
}

void testPanelsAreSeparatedByARealGutter(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    WorkspaceHost host(registry);
    host.resetToSingleArea("bloom.probe");
    auto* area = host.activeArea();
    expectations.expect(area != nullptr, "the workspace has an area to split");
    if (area == nullptr) {
        return;
    }
    (void)host.splitArea(*area, Qt::Horizontal, "bloom.probe", 0.5);

    const auto splitters = host.findChildren<QSplitter*>(QStringLiteral("workspaceSplitter"));
    expectations.expect(!splitters.isEmpty(), "splitting produces a workspace splitter");
    for (const auto* splitter : splitters) {
        // The gutter is a visible Background gap between panels, not a hairline seam: the handle
        // width is what makes that gap real and grabbable.
        expectations.expect(splitter->handleWidth() == kit::px(kit::Spacing::Gutter),
                            "every workspace splitter's handle is the gutter token wide");
    }
    expectations.expect(kit::px(kit::Spacing::Gutter) == 6, "and the gutter token is 6");
}

void testTheSplitterHandleIsPaintedInBackground(Expectations& expectations) {
    // The gap has to read as the window behind the panels rather than as a light divider, so the
    // handle takes Background -- asserted through the generated sheet rather than a screenshot.
    const QString sheet = kit::kinetikStyleSheet();
    const qsizetype rule = sheet.indexOf(QStringLiteral("QSplitter::handle"));
    expectations.expect(rule >= 0, "the theme styles the splitter handle");
    if (rule < 0) {
        return;
    }
    const QString block = sheet.mid(rule, 80);
    expectations.expect(
        block.contains(kit::hex(kit::Color::Background)),
        "the handle is painted in Background, so the gutter reads as the gap it is");
}

void testTheNodeGraphSitsOnTheKinetikBackground(Expectations& expectations) {
    NodeGraphicsScene scene;
    expectations.expect(scene.backgroundBrush().color() == kit::color(kit::Color::Background),
                        "the node graph's canvas is the Kinetik background, not its own grey");
    expectations.expect(scene.objectName() == QStringLiteral("nodeGraphicsScene"),
                        "and the scene keeps its objectName");
}

// task U8, issue #131, fix 6: a window-scope backtick toggles fullscreen for the ACTIVE panel,
// mirroring composition_editors.cpp's own Space-key transport shortcut test (playback_controller_
// tests.cpp's testPlaybackToggleButtonAndSpaceShortcut): a plain QLineEdit as a sibling inside one
// shown, activated top-level window, so Qt::WindowShortcut's real per-window dispatch decides
// whether backtick reaches the action, rather than a synthetic focus check.
void testBacktickTogglesFullscreenForTheActivePanelAndDefersToTextEntry(
    Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();

    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* host = new WorkspaceHost(registry, &window);
    host->resetToSingleArea("bloom.probe");
    auto* firstArea = host->activeArea();
    expectations.expect(firstArea != nullptr, "the workspace starts with an active area");
    if (firstArea != nullptr) {
        (void)host->splitArea(*firstArea, Qt::Horizontal, "bloom.probe", 0.5);
    }
    auto* probeLineEdit = new QLineEdit(&window);
    probeLineEdit->setObjectName(QStringLiteral("workspaceBacktickProbeLineEdit"));
    layout->addWidget(host);
    layout->addWidget(probeLineEdit);
    window.show();
    window.activateWindow();
    QCoreApplication::processEvents();

    expectations.expect(!host->isAreaMaximized(), "the workspace starts un-maximized");

    host->setFocus();
    QCoreApplication::processEvents();
    QTest::keyClick(host, Qt::Key_QuoteLeft);
    QCoreApplication::processEvents();
    expectations.expect(host->isAreaMaximized(),
                        "the backtick application shortcut toggles fullscreen for the active "
                        "panel when no text-entry widget has focus");

    QTest::keyClick(host, Qt::Key_QuoteLeft);
    QCoreApplication::processEvents();
    expectations.expect(!host->isAreaMaximized(), "backtick toggles again to restore");

    host->toggleMaximizeActiveArea();
    QCoreApplication::processEvents();
    expectations.expect(host->isAreaMaximized(), "maximized again via the existing routing");

    probeLineEdit->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(QApplication::focusWidget() == probeLineEdit,
                        "the probe line edit genuinely holds keyboard focus");
    QTest::keyClick(probeLineEdit, Qt::Key_QuoteLeft);
    QCoreApplication::processEvents();
    expectations.expect(host->isAreaMaximized(),
                        "backtick is NOT stolen from a focused text-entry widget: fullscreen "
                        "state is unchanged by this keystroke");
    expectations.expect(probeLineEdit->text() == QStringLiteral("`"),
                        "the focused line edit consumed backtick as ordinary text input, "
                        "confirming the keystroke really reached it rather than vanishing");

    host->toggleMaximizeActiveArea();
}

// task U8, issue #131, fix 3: the app-wide QComboBox styling (the panel switcher's own widget
// type) matches the design sheet's dropdown chrome. Checked visually via an offscreen grab, on top
// of the QSS text assertions in kit_theme_tests.cpp; every plain kit::KDropdown consumer (viewer
// zoom, timeline blending/parent, picker model selector) is covered separately in
// kit_dropdown_tests.cpp, since they all share KDropdown's one paintEvent.
void testTheStyledQComboBoxRendersFieldAndBorder(Expectations& expectations) {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* combo = new QComboBox(&host);
    combo->addItem(QStringLiteral("Viewer"));
    combo->addItem(QStringLiteral("Timeline"));
    layout->addWidget(combo);
    host.resize(200, 60);
    host.show();
    QCoreApplication::processEvents();

    const QPixmap rendered = combo->grab();
    expectations.expect(!rendered.isNull(), "the styled QComboBox renders offscreen");
    if (rendered.isNull()) {
        return;
    }

    const QImage image = rendered.toImage();
    bool sawField = false;
    bool sawBorder = false;
    const QColor field = kit::color(kit::Color::Field);
    const QColor border = kit::color(kit::Color::Border);
    const auto near = [](const QColor& left, const QColor& right) {
        return std::abs(left.red() - right.red()) <= 4 &&
               std::abs(left.green() - right.green()) <= 4 &&
               std::abs(left.blue() - right.blue()) <= 4;
    };
    for (int y = 0; y < image.height() && (!sawField || !sawBorder); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (near(pixel, field)) {
                sawField = true;
            }
            if (near(pixel, border)) {
                sawBorder = true;
            }
        }
    }
    expectations.expect(sawField, "the closed field really paints the Field surface");
    expectations.expect(sawBorder, "the closed field really paints the Border hairline");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    Expectations expectations;
    testPanelsAreSeparatedByARealGutter(expectations);
    testTheSplitterHandleIsPaintedInBackground(expectations);
    testTheNodeGraphSitsOnTheKinetikBackground(expectations);
    testBacktickTogglesFullscreenForTheActivePanelAndDefersToTextEntry(expectations);
    testTheStyledQComboBoxRendersFieldAndBorder(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
