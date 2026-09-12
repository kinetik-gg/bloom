#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QApplication>
#include <QComboBox>
#include <QImage>
#include <QLineEdit>
#include <QMenuBar>
#include <QPixmap>
#include <QRect>
#include <QSplitter>
#include <QString>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
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

// task C1, item C4 (owner: "panels inside window is overlapping with actual window border at the
// edges; window needs inner padding"): the workspace's single area sits inset from WorkspaceHost's
// own rect by exactly Spacing::Gutter on every side -- the same visible Background gap the
// splitter handle already gives panels between each other, now also given to the window's own
// edge.
void testWorkspaceHostInsetsItsSingleAreaByTheGutterFromItsOwnRect(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    WorkspaceHost host(registry);
    host.resize(400, 300);
    // Shown (like testTheStyledQComboBoxRendersFieldAndBorder's own precedent just below):
    // an unshown top-level widget's layout is not guaranteed to activate from resize() and
    // processEvents() alone, so the area would still report its default sizeHint-based geometry
    // rather than the real, gutter-inset one this test means to measure.
    host.show();
    QCoreApplication::processEvents();

    auto* area = host.activeArea();
    expectations.expect(area != nullptr, "the workspace has an active area to measure");
    if (area == nullptr) {
        host.hide();
        return;
    }
    const QRect areaGeometry = area->geometry();
    const int gutter = kit::px(kit::Spacing::Gutter);
    expectations.expect(areaGeometry.left() == gutter && areaGeometry.top() == gutter,
                        "the area is inset from the host's near edges by exactly the gutter");
    const int rightGap = host.rect().width() - areaGeometry.right() - 1;
    const int bottomGap = host.rect().height() - areaGeometry.bottom() - 1;
    expectations.expect(rightGap == gutter && bottomGap == gutter,
                        "...and from its far edges too, uniformly on all four sides");
    host.hide();
}

// task C1, item C3 (owner: "menus properly padded, not reaching the top edge"): the generated
// theme carries the exact Spacing::S (8px) vertical bar padding and Spacing::MenuItemX (10px)
// horizontal item padding the owner asked for.
void testMenuBarCarriesItsDocumentedPadding(Expectations& expectations) {
    const QString sheet = kit::kinetikStyleSheet();

    // "\n" before "QMenuBar {": the FIRST bare occurrence of "QMenuBar {" is a substring of the
    // combined "QMainWindow, QMenuBar {" selector above it, which carries no padding at all --
    // the standalone rule this test means to inspect always starts its own line.
    const qsizetype barRule = sheet.indexOf(QStringLiteral("\nQMenuBar {"));
    expectations.expect(barRule >= 0, "the theme styles the menu bar container");
    if (barRule >= 0) {
        const qsizetype barRuleEnd = sheet.indexOf(QStringLiteral("}"), barRule);
        const QString block = sheet.mid(barRule, barRuleEnd - barRule);
        expectations.expect(
            block.contains(QStringLiteral("padding: %1px").arg(kit::px(kit::Spacing::S))),
            "the bar carries Spacing::S (8px) vertical padding around its items, off the top "
            "client-area edge");
    }

    const qsizetype itemRule = sheet.indexOf(QStringLiteral("QMenuBar::item {"));
    expectations.expect(itemRule >= 0, "the theme styles the menu bar's items");
    if (itemRule >= 0) {
        const qsizetype itemRuleEnd = sheet.indexOf(QStringLiteral("}"), itemRule);
        const QString block = sheet.mid(itemRule, itemRuleEnd - itemRule);
        expectations.expect(
            block.contains(QStringLiteral("%1px").arg(kit::px(kit::Spacing::MenuItemX))),
            "each item carries Spacing::MenuItemX (10px) horizontal padding");
    }
}

// The same padding, rendered: a real QMenuBar's own item sits with an equal gap above and below
// it in the bar's row -- vertically centered -- rather than flush against the top client-area
// edge the owner complained about.
void testMenuBarItemsRenderVerticallyCenteredWithRoomAboveThem(Expectations& expectations) {
    QMenuBar menuBar;
    menuBar.setStyleSheet(kit::kinetikStyleSheet());
    menuBar.addMenu(QStringLiteral("&File"));
    menuBar.resize(menuBar.sizeHint());
    QCoreApplication::processEvents();

    const auto actions = menuBar.actions();
    expectations.expect(!actions.isEmpty(), "the probe menu bar has a File menu action");
    if (actions.isEmpty()) {
        return;
    }
    const QRect itemRect = menuBar.actionGeometry(actions.constFirst());
    expectations.expect(!itemRect.isNull(), "the item has real, laid-out geometry");
    if (itemRect.isNull()) {
        return;
    }
    const int topGap = itemRect.top();
    const int bottomGap = menuBar.height() - itemRect.bottom() - 1;
    expectations.expect(topGap > 0, "the item never reaches the bar's own top edge");
    expectations.expect(std::abs(topGap - bottomGap) <= 1,
                        "the item sits vertically centered in the bar's row (symmetric top/bottom "
                        "gap)");
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
    testWorkspaceHostInsetsItsSingleAreaByTheGutterFromItsOwnRect(expectations);
    testMenuBarCarriesItsDocumentedPadding(expectations);
    testMenuBarItemsRenderVerticallyCenteredWithRoomAboveThem(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
