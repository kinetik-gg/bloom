#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/panel_switcher.hpp>
#include <bloom/ui/kit/surfaces.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QIcon>
#include <QImage>
#include <QMenu>
#include <QPixmap>
#include <QPoint>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSize>
#include <QSizePolicy>
#include <QString>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
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

// FORMAL AMENDMENT 1 (task C1): a minimal test double for an editor that offers EditorArea a
// footer -- exercises the generic EditorFooterProvider seam without needing ViewerEditor's real
// CompositionSession/CompositionPreviewController wiring (that pin belongs in
// viewer_editor_tests.cpp, which already has that fixture machinery).
class FakeFooterProvidingEditor final : public QWidget, public EditorChromeProvider {
  public:
    explicit FakeFooterProvidingEditor(QWidget* parent) : QWidget(parent) {
        chrome_.footer.objectName = "fakeDeclaredFooter";
        chrome_.footer.addWidget(new kit::KLabel("Footer", this));
        auto* footer = EditorArea::buildChromeRow(chrome_.footer, this, true);
        footer->setProperty("fakeFooterMarker", true);
    }
    EditorChromeSpec& editorChrome() override { return chrome_; }

  private:
    EditorChromeSpec chrome_;
};

EditorRegistry makeRegistry() {
    EditorRegistry registry;
    (void)registry.registerEditor(
        {"bloom.probe", "Probe", [](QWidget* parent) -> QWidget* { return new QWidget(parent); }});
    return registry;
}

// task U8, issue #131, formal amendment 1, A5: the real editor ids the panel-identity icon
// mapping keys on, with trivial factories -- not registerFoundationEditors(), which needs a real
// CompositionSession/CompositionPreviewController this test has no reason to construct.
EditorRegistry makeRealIdRegistry() {
    EditorRegistry registry;
    const auto addTrivial = [&registry](const char* id, const char* displayName) {
        (void)registry.registerEditor(
            {id, displayName, [](QWidget* parent) -> QWidget* { return new QWidget(parent); }});
    };
    addTrivial("bloom.viewer", "Viewer");
    addTrivial("bloom.nodes", "Nodes");
    addTrivial("bloom.timeline", "Timeline");
    addTrivial("bloom.assets", "Assets");
    addTrivial("bloom.properties", "Properties");
    return registry;
}

EditorRegistry makeFooterProvidingRegistry() {
    EditorRegistry registry;
    (void)registry.registerEditor(
        {"bloom.footerProbe", "Footer Probe",
         [](QWidget* parent) -> QWidget* { return new FakeFooterProvidingEditor(parent); }});
    return registry;
}

// Task NODES-1: the header's counterpart to FakeFooterProvidingEditor above -- a minimal test
// double proving the generic EditorHeaderMenuProvider seam without needing the real node editor's
// menus/actions (that pin belongs in node_interaction_tests.cpp, which already exercises them).
class FakeHeaderMenuProvidingEditor final : public QWidget, public EditorChromeProvider {
  public:
    explicit FakeHeaderMenuProvidingEditor(QWidget* parent) : QWidget(parent) {
        chrome_.header.addWidget(new kit::KLabel("Header", this));
        auto* header = EditorArea::buildChromeRow(chrome_.header, this);
        header->setProperty("fakeHeaderMenuMarker", true);
    }
    EditorChromeSpec& editorChrome() override { return chrome_; }

  private:
    EditorChromeSpec chrome_;
};

EditorRegistry makeHeaderMenuProvidingRegistry() {
    EditorRegistry registry;
    (void)registry.registerEditor(
        {"bloom.headerMenuProbe", "Header Menu Probe",
         [](QWidget* parent) -> QWidget* { return new FakeHeaderMenuProvidingEditor(parent); }});
    return registry;
}

// task U8, issue #131 (fixes 4/5/6): panelContextMenuButton and closeAreaButton are gone. The
// only surviving header QToolButton is maximizeAreaButton, and it now toggles/restores
// fullscreen -- Fullscreen/Exit Fullscreen wording, not Maximize/Restore.

void testTheOldHeaderButtonsAreReallyGone(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    EditorArea area(registry, "bloom.probe", QString{});

    for (const char* objectName : {"splitLeftRightButton", "splitTopBottomButton",
                                   "panelContextMenuButton", "closeAreaButton"}) {
        expectations.expect(area.findChild<QToolButton*>(QString::fromLatin1(objectName)) ==
                                nullptr,
                            std::string{objectName} + " is really gone, not merely relabeled");
    }
}

void testTheMaximizeButtonIsTheOnlyRemainingHeaderButtonAndIsAFullscreenToggle(
    Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    EditorArea area(registry, "bloom.probe", QString{});

    auto* button = area.findChild<QToolButton*>(QStringLiteral("maximizeAreaButton"));
    expectations.expect(button != nullptr, "the maximize control exists");
    if (button == nullptr) {
        return;
    }
    expectations.expect(!button->icon().isNull(), "maximizeAreaButton draws a Kinetik icon");
    expectations.expect(button->text().isEmpty(), "maximizeAreaButton carries no typed glyph");
    expectations.expect(button->toolTip() == QStringLiteral("Fullscreen"),
                        "resting wording is Fullscreen, not Maximize area");
    expectations.expect(button->accessibleName() == QStringLiteral("Fullscreen"),
                        "the accessible name matches: an icon never replaces one");
    // task U8, formal amendment 2, A9: IconMedium (16px), not the dense-chrome IconSmall box.
    // Task VIEW-1 gave that box a name -- it is kit::IconRole::Chrome's own box now.
    expectations.expect(button->iconSize().width() == kit::px(kit::iconSize(kit::IconRole::Chrome)),
                        "maximizeAreaButton uses the corrected 16px glyph box");

    const qint64 restingIcon = button->icon().cacheKey();
    area.setMaximizedAppearance(true);
    expectations.expect(button->icon().cacheKey() != restingIcon,
                        "toggling to fullscreen swaps the glyph, not only the tooltip");
    expectations.expect(button->toolTip() == QStringLiteral("Exit Fullscreen"),
                        "fullscreen wording is Exit Fullscreen");
    expectations.expect(button->accessibleName() == QStringLiteral("Exit Fullscreen"),
                        "the accessible name follows the tooltip");
    expectations.expect(button->objectName() == QStringLiteral("maximizeAreaButton"),
                        "the objectName is a contract and never changes with appearance");

    area.setMaximizedAppearance(false);
    expectations.expect(button->toolTip() == QStringLiteral("Fullscreen"),
                        "restoring puts the resting wording back");
}

void testTheHeaderQToolButtonsAreSquare(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    EditorArea area(registry, "bloom.probe", QString{});
    auto* button = area.findChild<QToolButton*>(QStringLiteral("maximizeAreaButton"));
    expectations.expect(button != nullptr, "the maximize control exists");
    if (button == nullptr) {
        return;
    }
    expectations.expect(button->width() == button->height(),
                        "the header's one remaining icon-only QToolButton is square");
    // task U8, formal amendment 2, A9: reverted to a plain, bordered Size::Control (26x26)
    // square -- amendment 1's "Control minus header padding" formula (fix 7) produced an
    // illegible 16px button once Spacing::PanelHeader grew to 10, and A9 explicitly undoes that
    // coupling; Spacing::PanelHeader now governs only the header row's own padding.
    expectations.expect(button->width() == kit::px(kit::Size::Control),
                        "square at exactly Size::Control, per formal amendment 2's A9");
}

void testTheContextMenuOffersAllFourOperationsWithFullscreenWording(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    EditorArea area(registry, "bloom.probe", QString{});
    auto* menu = area.findChild<QMenu*>(QStringLiteral("panelOptionsMenu"));
    expectations.expect(menu != nullptr, "the panel options menu exists on the area itself");
    if (menu == nullptr) {
        return;
    }
    for (const char* objectName : {"panelSplitHorizontalAction", "panelSplitVerticalAction",
                                   "panelMaximizeAction", "panelCloseAction"}) {
        expectations.expect(menu->findChild<QAction*>(QString::fromLatin1(objectName)) != nullptr,
                            std::string{"the menu offers "} + objectName);
    }
    auto* maximizeAction = menu->findChild<QAction*>(QStringLiteral("panelMaximizeAction"));
    expectations.expect(maximizeAction != nullptr &&
                            maximizeAction->text() == QStringLiteral("Fullscreen"),
                        "Maximize reads as Fullscreen per fix 6's wording");

    auto* closeAction = menu->findChild<QAction*>(QStringLiteral("panelCloseAction"));
    expectations.expect(closeAction != nullptr && closeAction->text() == QStringLiteral("Close"),
                        "Close stays in the menu, unchanged");

    area.setMaximizedAppearance(true);
    expectations.expect(maximizeAction->text() == QStringLiteral("Exit Fullscreen"),
                        "the menu action's wording stays in lockstep with the header button");
}

void testARightClickOnTheHeaderOpensTheMenuAtTheCursor(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    EditorArea area(registry, "bloom.probe", QString{});
    auto* header = area.findChild<QWidget*>(QStringLiteral("editorHeader"));
    auto* menu = area.findChild<QMenu*>(QStringLiteral("panelOptionsMenu"));
    expectations.expect(header != nullptr && menu != nullptr,
                        "the header and its options menu both exist");
    if (header == nullptr || menu == nullptr) {
        return;
    }
    expectations.expect(!menu->isVisible(), "the menu starts closed");

    const QPoint localPos(5, 5);
    QContextMenuEvent contextMenuEvent(QContextMenuEvent::Mouse, localPos,
                                       header->mapToGlobal(localPos));
    QCoreApplication::sendEvent(header, &contextMenuEvent);
    QCoreApplication::processEvents();

    expectations.expect(menu->isVisible(), "a right-click on the header opens the panel menu");
    menu->close();
}

void testHeaderControlsAndMenuActionsStillDriveTheirSignals(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    EditorArea area(registry, "bloom.probe", QString{});
    auto* menu = area.findChild<QMenu*>(QStringLiteral("panelOptionsMenu"));
    expectations.expect(menu != nullptr, "the panel options menu exists");
    if (menu == nullptr) {
        return;
    }

    QSignalSpy splitSpy(&area, &EditorArea::splitRequested);
    QSignalSpy closeSpy(&area, &EditorArea::closeRequested);
    QSignalSpy maximizeSpy(&area, &EditorArea::maximizeRequested);

    // QAction::trigger() fires the same signal a real click on a shown popup would, without
    // needing to actually open the (offscreen) popup -- the menu's own visibility is not what is
    // under test here.
    menu->findChild<QAction*>(QStringLiteral("panelSplitHorizontalAction"))->trigger();
    menu->findChild<QAction*>(QStringLiteral("panelSplitVerticalAction"))->trigger();
    area.findChild<QToolButton*>(QStringLiteral("maximizeAreaButton"))->click();
    // The menu's own Maximize/Close actions route to the SAME signals the standalone maximize
    // button does -- Close now has no standalone button at all, only the menu action.
    menu->findChild<QAction*>(QStringLiteral("panelMaximizeAction"))->trigger();
    menu->findChild<QAction*>(QStringLiteral("panelCloseAction"))->trigger();

    expectations.expect(splitSpy.count() == 2, "both menu split actions request a split");
    expectations.expect(closeSpy.count() == 1,
                        "the menu's close action requests a close -- the only remaining path");
    expectations.expect(maximizeSpy.count() == 2,
                        "the maximize button AND the menu's maximize action request a maximize");
}

void testSplitAndCloseEnablementIsUnchanged(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    EditorArea area(registry, "bloom.probe", QString{});
    auto* menu = area.findChild<QMenu*>(QStringLiteral("panelOptionsMenu"));
    expectations.expect(menu != nullptr, "the panel options menu exists");
    if (menu == nullptr) {
        return;
    }

    area.setSplitEnabled(false);
    area.setCloseEnabled(false);
    expectations.expect(
        !menu->findChild<QAction*>(QStringLiteral("panelSplitHorizontalAction"))->isEnabled() &&
            !menu->findChild<QAction*>(QStringLiteral("panelSplitVerticalAction"))->isEnabled(),
        "split enablement still reaches the menu's two split actions");
    expectations.expect(
        !menu->findChild<QAction*>(QStringLiteral("panelCloseAction"))->isEnabled(),
        "close enablement reaches the menu's close action -- the only remaining close path");

    area.setSplitEnabled(true);
    area.setCloseEnabled(true);
    expectations.expect(
        menu->findChild<QAction*>(QStringLiteral("panelSplitHorizontalAction"))->isEnabled() &&
            menu->findChild<QAction*>(QStringLiteral("panelSplitVerticalAction"))->isEnabled(),
        "and re-enabling reaches them too");
    expectations.expect(menu->findChild<QAction*>(QStringLiteral("panelCloseAction"))->isEnabled(),
                        "re-enabling reaches the close action too");
}

// task U8, issue #131, formal amendment 1, A5 (mapping) / formal amendment 2, A7 (native
// KPanelSwitcher item-icon rendering, ported from QComboBox::setItemIcon): pins the
// panel-switcher icon mapping. One assertion per id covers both the closed field's current-item
// icon and the popup row, since KPanelSwitcher's own model backs both.
void testThePanelSwitcherPinsTheIconMapping(Expectations& expectations) {
    const EditorRegistry registry = makeRealIdRegistry();
    EditorArea area(registry, "bloom.viewer", QString{});
    auto* picker = area.findChild<kit::KPanelSwitcher*>(QStringLiteral("editorTypePicker"));
    expectations.expect(picker != nullptr, "the panel switcher exists");
    if (picker == nullptr) {
        return;
    }

    struct Mapping {
        const char* id;
        kit::IconId icon;
    };
    // task U8, formal amendment 2, A7: the crop's own "panel icon 16px" -- IconMedium, not the
    // IconSmall size formal amendment 1 originally used. Task VIEW-1: that is IconRole::Chrome.
    const auto chromeBox = kit::px(kit::iconSize(kit::IconRole::Chrome));
    const auto sampleSize = QSize(chromeBox, chromeBox);
    for (const auto& [id, iconId] :
         {Mapping{"bloom.assets", kit::IconId::Folder}, Mapping{"bloom.viewer", kit::IconId::Stack},
          Mapping{"bloom.timeline", kit::IconId::Clock},
          Mapping{"bloom.properties", kit::IconId::SlidersHorizontal},
          Mapping{"bloom.nodes", kit::IconId::Graph}}) {
        const int index = picker->findData(QString::fromLatin1(id));
        expectations.expect(index >= 0, std::string{id} + " is a panel-switcher entry");
        if (index < 0) {
            continue;
        }
        const QIcon actual = picker->itemIcon(index);
        expectations.expect(!actual.isNull(), std::string{id} + " carries an icon");
        const QIcon expected = kit::icon(iconId, kit::IconRole::Chrome);
        expectations.expect(actual.pixmap(sampleSize).toImage() ==
                                expected.pixmap(sampleSize).toImage(),
                            std::string{id} + " maps to its documented glyph");
    }
}

// task U8, issue #131, formal amendment 2, A7/A8: the switcher hugs its content (never
// stretches), keeps its objectName, ports the unavailable-editor placeholder/tooltip behavior,
// and its label is natural Title case (no uppercase transform).
void testThePanelSwitcherHugsItsContentAndKeepsBehaviorParity(Expectations& expectations) {
    const EditorRegistry registry = makeRealIdRegistry();
    EditorArea area(registry, "bloom.viewer", QString{});
    auto* picker = area.findChild<kit::KPanelSwitcher*>(QStringLiteral("editorTypePicker"));
    expectations.expect(picker != nullptr, "the panel switcher exists and keeps its objectName");
    if (picker == nullptr) {
        return;
    }

    expectations.expect(picker->font().capitalization() != QFont::AllUppercase,
                        "the switcher label is NOT uppercase-transformed (A8)");
    expectations.expect(picker->itemText(picker->currentIndex()) == QStringLiteral("Viewer"),
                        "the registry's own Title-case display name renders verbatim");

    // Content-hugging: the field's sizeHint is well short of a typical header row's own width,
    // proving it does not stretch to fill the layout the way the old QComboBox did.
    expectations.expect(picker->sizeHint().width() < 200,
                        "the switcher hugs its content rather than stretching wide");

    // The unavailable-editor placeholder path, ported: an id the registry does not know about
    // still becomes a selectable, tooltip-carrying entry (setEditorId()'s own contract).
    expectations.expect(area.setEditorId("bloom.nonexistent"),
                        "setEditorId() still succeeds for an id the registry does not know");
    expectations.expect(area.editorId() == "bloom.nonexistent",
                        "the unavailable id becomes the current value");
    expectations.expect(picker->toolTip() ==
                            QStringLiteral("Unavailable editor: bloom.nonexistent"),
                        "the closed field's tooltip carries the ported placeholder message");
}

// task U8, issue #131, formal amendment 2, A10: header proportions per the design crops.
void testHeaderProportionsMatchTheDesignCrops(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    EditorArea area(registry, "bloom.probe", QString{});
    auto* header = area.findChild<QWidget*>(QStringLiteral("editorHeader"));
    auto* picker = area.findChild<kit::KPanelSwitcher*>(QStringLiteral("editorTypePicker"));
    expectations.expect(header != nullptr && picker != nullptr,
                        "the header and switcher both exist");
    if (header == nullptr || picker == nullptr) {
        return;
    }
    expectations.expect(kit::px(kit::Size::EditorHeader) == 32,
                        "the header row's own height token is 32");
    expectations.expect(picker->sizeHint().height() == kit::px(kit::Size::Control),
                        "the switcher field is Control (26) tall");
}

double pixelLuminance(const QColor& color) {
    return 0.2126 * color.red() + 0.7152 * color.green() + 0.0722 * color.blue();
}

void testPanelSwitcherFocusIsKeyboardOnly(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    auto* area = new EditorArea(registry, "bloom.probe", QString{});
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(area);
    host.resize(320, 180);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    auto* picker = area->findChild<kit::KPanelSwitcher*>(QStringLiteral("editorTypePicker"));
    expectations.expect(picker != nullptr, "the editor header owns the panel switcher");
    if (picker == nullptr) {
        delete area;
        return;
    }

    picker->clearFocus();
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(picker, &leave);
    const QImage resting = picker->grab().toImage();
    const auto edgeLuminance = [](const QImage& image) {
        const int y = std::min(2, image.height() - 1);
        return pixelLuminance(image.pixelColor(image.width() / 2, y));
    };
    const double restingEdge = edgeLuminance(resting);
    const double borderEdge = pixelLuminance(kit::color(kit::Color::Border));
    const double accentEdge = pixelLuminance(kit::color(kit::Color::Accent));
    expectations.expect(picker->borderToken() == kit::Color::Border &&
                            std::abs(restingEdge - borderEdge) < std::abs(restingEdge - accentEdge),
                        "a resting panel switcher has no focus outline without keyboard focus");

    picker->setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    const QImage focused = picker->grab().toImage();
    const double focusedEdge = edgeLuminance(focused);
    expectations.expect(picker->borderToken() == kit::Color::Accent &&
                            focusedEdge > restingEdge + 10.0,
                        "the panel switcher shows its focus outline after keyboard traversal");

    picker->clearFocus();
    QCoreApplication::sendEvent(picker, &leave);
    delete area;
}

// FORMAL AMENDMENT 1 (task C1, after the first report; owner: "an empty reserved strip on every
// panel is NOT wanted"). An editor that implements EditorFooterProvider and offers a real footer
// widget gets one hosted under objectName "editorFooter", holding exactly the widget it handed
// back.
void testAFooterProvidingEditorGetsAHostedFooterNamedEditorFooter(Expectations& expectations) {
    const EditorRegistry registry = makeFooterProvidingRegistry();
    EditorArea area(registry, "bloom.footerProbe", QString{});
    auto* footer = area.findChild<QWidget*>(QStringLiteral("editorFooter"));
    expectations.expect(footer != nullptr,
                        "the footer-providing editor's footer is hosted under objectName "
                        "\"editorFooter\"");
    if (footer == nullptr) {
        return;
    }
    expectations.expect(
        footer->findChild<QWidget*>("fakeDeclaredFooter") &&
            footer->findChild<QWidget*>("fakeDeclaredFooter")
                ->property("fakeFooterMarker")
                .toBool(),
        "the footer host retains the exact declared row and its original object name");
}

// FORMAL AMENDMENT 1: a footer-LESS editor (the plain probe stands in for nodes/properties/assets,
// none of which implement EditorFooterProvider) has no "editorFooter" child at all -- not an
// empty reserved strip, per the amendment's correction of this task's first report.
void testAFooterLessEditorHasNoEditorFooterChildAtAll(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    EditorArea area(registry, "bloom.probe", QString{});
    expectations.expect(area.findChild<QWidget*>(QStringLiteral("editorFooter")) == nullptr,
                        "an editor that never implements EditorFooterProvider gets no footer row "
                        "at all");
}

// Task NODES-1: the header's counterpart to the two footer tests above, pinning
// EditorHeaderMenuProvider the same way.
void testAHeaderMenuProvidingEditorGetsItsWidgetHostedInTheHeader(Expectations& expectations) {
    const EditorRegistry registry = makeHeaderMenuProvidingRegistry();
    EditorArea area(registry, "bloom.headerMenuProbe", QString{});
    auto* header = area.findChild<QWidget*>(QStringLiteral("editorHeader"));
    expectations.expect(header != nullptr, "the header exists");
    if (header == nullptr) {
        return;
    }
    QWidget* hosted = nullptr;
    for (auto* candidate : header->findChildren<QWidget*>()) {
        if (candidate->property("fakeHeaderMenuMarker").toBool()) {
            hosted = candidate;
            break;
        }
    }
    expectations.expect(hosted != nullptr,
                        "the header-menu-providing editor's widget is hosted in the header -- the "
                        "exact widget the provider handed back, not a copy or a wrapper");
}

void testAHeaderMenuLessEditorHasNoExtraHeaderChild(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    EditorArea area(registry, "bloom.probe", QString{});
    auto* header = area.findChild<QWidget*>(QStringLiteral("editorHeader"));
    expectations.expect(header != nullptr, "the header exists");
    if (header == nullptr) {
        return;
    }
    for (auto* candidate : header->findChildren<QWidget*>()) {
        expectations.expect(!candidate->property("fakeHeaderMenuMarker").toBool(),
                            "an editor that never implements EditorHeaderMenuProvider adds nothing "
                            "extra to the header");
    }
}

// Switching AWAY from a header-menu-providing editor tears down its header widget along with it --
// the same rebuildEditor() sequence that already retires an old footer.
void testSwitchingEditorsRetiresTheOldHeaderMenuWidget(Expectations& expectations) {
    EditorRegistry registry = makeHeaderMenuProvidingRegistry();
    (void)registry.registerEditor(
        {"bloom.probe", "Probe", [](QWidget* parent) -> QWidget* { return new QWidget(parent); }});
    EditorArea area(registry, "bloom.headerMenuProbe", QString{});
    expectations.expect(area.setEditorId("bloom.probe"),
                        "switching to a header-menu-less editor succeeds");
    auto* header = area.findChild<QWidget*>(QStringLiteral("editorHeader"));
    expectations.expect(header != nullptr, "the header still exists after switching");
    if (header == nullptr) {
        return;
    }
    for (auto* candidate : header->findChildren<QWidget*>()) {
        expectations.expect(!candidate->property("fakeHeaderMenuMarker").toBool(),
                            "the old editor's header-menu widget was torn down, not left behind");
    }
}

// task C1, item C5 (owner: "cut rounded corners because the background is not clipped by the
// panel"): an offscreen grab of a real EditorArea -- header, content, and footer all painting
// their own full-bleed Surface/Background rectangles -- still shows exactly the window Background
// color at all four corners, never a header/footer/content square corner bleeding past the
// Radius::Panel curve.
void testTheFourCornersAreClippedToWindowBackground(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    EditorArea area(registry, "bloom.probe", QString{});
    area.resize(240, 160);
    QCoreApplication::processEvents();

    const QImage image = area.grab().toImage();
    expectations.expect(!image.isNull(), "the panel renders offscreen");
    if (image.isNull()) {
        return;
    }

    const QColor background = kit::color(kit::Color::Background);
    const QColor border = kit::color(kit::Color::Border);
    const qreal dpr = image.devicePixelRatio();
    const int w = image.width();
    const int h = image.height();
    const int last = static_cast<int>(std::lround(dpr)) - 1;
    const std::array<QPoint, 4> corners = {
        QPoint(0, 0),
        QPoint(w - 1 - last, 0),
        QPoint(0, h - 1 - last),
        QPoint(w - 1 - last, h - 1 - last),
    };
    const int r = kit::KPanelFrame::radiusPx() * static_cast<int>(std::lround(dpr));
    if (r == 0) {
        // Square panels: there is no corner curve to clip, so the border reaches the corner and
        // the corner is border ink rather than the window background a rounded panel revealed.
        const int inkFloor =
            (qGray(border.rgb()) + qGray(kit::color(kit::Color::Surface).rgb())) / 2;
        for (const auto& corner : corners) {
            expectations.expect(qGray(image.pixelColor(corner).rgb()) >= inkFloor,
                                "a square panel's corner carries border ink at (" +
                                    std::to_string(corner.x()) + ", " + std::to_string(corner.y()) +
                                    ")");
        }
        return;
    }
    // Rounded panels: Background (#111111) and Surface (#141414, the header/footer's own fill)
    // differ by only 3 per channel, so this checks exact equality rather than a tolerant "near"
    // match -- a loose tolerance would pass even if a header/footer/content corner bled straight
    // through.
    for (const auto& corner : corners) {
        expectations.expect(image.pixelColor(corner) == background,
                            "the panel corner at (" + std::to_string(corner.x()) + ", " +
                                std::to_string(corner.y()) +
                                ") shows the window background, not a header/footer/content "
                                "corner bleeding past the rounded curve");
    }
    // Owner, 2026-09-15: the extreme pixel was never the problem; the header's square corner
    // showed INSIDE the curve and cut the border. Walk the top-left arc: every pixel outside it
    // is window background and the arc itself carries the border ink, on the header's own rows.
    bool outsideIsBackground = true;
    bool arcCarriesBorder = true;
    for (int y = 1; y < r / 2; ++y) {
        // x on the arc for this row (circle of radius r centred at (r, r)).
        const double dy = r - y - 0.5;
        const double dx = std::sqrt(std::max(0.0, static_cast<double>(r) * r - dy * dy));
        const int arcX = static_cast<int>(std::lround(r - dx));
        if (arcX > 2 && image.pixelColor(QPoint(std::max(0, arcX - 2), y)) != background)
            outsideIsBackground = false;
        // The arc is antialiased, so the border ink blends with its neighbours: accept any
        // pixel on the arc that is clearly lighter than Surface (Border is 0x22, Surface 0x14).
        const int inkFloor =
            (qGray(border.rgb()) + qGray(kit::color(kit::Color::Surface).rgb())) / 2;
        bool ink = false;
        for (int x = std::max(0, arcX - 1); x <= arcX + 1 && x < w; ++x)
            if (qGray(image.pixelColor(QPoint(x, y)).rgb()) >= inkFloor)
                ink = true;
        if (!ink)
            arcCarriesBorder = false;
    }
    expectations.expect(outsideIsBackground,
                        "every pixel outside the top-left arc is window background, so no header "
                        "corner shows inside the curve");
    expectations.expect(arcCarriesBorder,
                        "the top-left arc carries the border ink on the header's own rows, so the "
                        "curve is never cut");
}

// task WIDTH-1 (owner: "let it have min width of something like 300px ... instead of kicking
// borders around"): the pin for EditorArea::minimumSizeHint()'s new fixed floor. A hosted editor
// whose own natural minimum width is nowhere near 300px (bloom.probe, a plain QWidget) and one
// that demands 900px (a stand-in for a Properties selection with long parameter names and many
// value cells) must both report the exact same PanelMinWidth -- proof the panel's own reported
// minimum never depends on what is hosted inside it, so a QSplitter can never be asked to move a
// handle over a selection change.
void testPanelMinWidthIsFixedRegardlessOfHostedEditorHints(Expectations& expectations) {
    EditorRegistry registry = makeRegistry();
    (void)registry.registerEditor({"bloom.wide", "Wide", [](QWidget* parent) -> QWidget* {
                                       auto* widget = new QWidget(parent);
                                       widget->setMinimumWidth(900);
                                       return widget;
                                   }});
    EditorArea area(registry, "bloom.probe", QString{});
    const int narrowHostedMinimum = area.minimumSizeHint().width();
    expectations.expect(narrowHostedMinimum == kit::px(kit::Size::PanelMinWidth),
                        "hosting a plain, narrow editor still reports exactly PanelMinWidth");

    expectations.expect(area.setEditorId("bloom.wide"),
                        "switching to a 900px-minimum hosted editor succeeds");
    const int wideHostedMinimum = area.minimumSizeHint().width();
    expectations.expect(wideHostedMinimum == kit::px(kit::Size::PanelMinWidth),
                        "hosting a 900px-minimum editor STILL reports exactly PanelMinWidth -- "
                        "task WIDTH-1's whole point");
    expectations.expect(narrowHostedMinimum == wideHostedMinimum,
                        "switching the hosted editor never moves what the panel itself reports as "
                        "its own minimum size, which is what keeps a QSplitter handle from moving");
}

// task WIDTH-1: Properties is the one hosted editor that is a form rather than a canvas, so
// EditorArea hosts it inside its own QScrollArea (Ignored on the horizontal axis, widgetResizable)
// rather than parenting it directly -- see editor_area.cpp's rebuildEditor(). This exercises that
// wrapping with a stand-in "bloom.properties" editor (a plain 900px-minimum QWidget) rather than
// the real PropertiesEditor, which has its own fixture-heavy CompositionSession dependency covered
// separately in properties_editor_tests.cpp.
void testPropertiesIsHostedInsideAnIgnoredScrollArea(Expectations& expectations) {
    EditorRegistry registry = makeRegistry();
    (void)registry.registerEditor(
        {"bloom.properties", "Properties", [](QWidget* parent) -> QWidget* {
             auto* widget = new QWidget(parent);
             widget->setMinimumWidth(900);
             return widget;
         }});
    EditorArea area(registry, "bloom.properties", QString{});

    auto* scrollArea = area.findChild<QScrollArea*>(QStringLiteral("editorContentScrollArea"));
    expectations.expect(scrollArea != nullptr,
                        "Properties is hosted inside a QScrollArea named editorContentScrollArea "
                        "(new objectName, task WIDTH-1)");
    if (scrollArea == nullptr) {
        return;
    }
    expectations.expect(scrollArea->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored,
                        "the scroll area is Ignored on the horizontal axis so its own minimum "
                        "width never propagates up through the panel's content layout");
    expectations.expect(scrollArea->widgetResizable(),
                        "the scroll area actually resizes the hosted editor down to the room it "
                        "gets rather than always keeping it at its natural size");
    expectations.expect(area.minimumSizeHint().width() == kit::px(kit::Size::PanelMinWidth),
                        "the panel still reports exactly PanelMinWidth with Properties hosted");

    expectations.expect(area.setEditorId("bloom.probe"), "switching away from Properties succeeds");
    expectations.expect(
        area.findChild<QScrollArea*>(QStringLiteral("editorContentScrollArea")) == nullptr,
        "the scroll area host is torn down, not left behind, once Properties is no longer hosted");
}

void testSharedFooterExpansionAndSuppression(Expectations& expectations) {
    QWidget host;
    EditorChromeRowSpec spec;
    auto* fixed = new kit::KIconButton(&host);
    auto* flexible = new QWidget(&host);
    spec.addWidget(fixed);
    spec.addExpandingWidget(flexible);
    auto* footer = EditorArea::buildChromeRow(spec, &host, true);
    host.resize(600, kit::px(kit::Size::FooterRow));
    host.show();
    for (int width : {600, 300}) {
        footer->resize(width, kit::px(kit::Size::FooterRow));
        footer->show();
        QCoreApplication::processEvents();
        const int padding = kit::px(kit::Spacing::ChromePadding);
        expectations.expect(fixed->x() == padding &&
                                flexible->geometry().right() == width - padding - 1,
                            "shared footer fills available space and retains chrome padding");
        expectations.expect(flexible->x() - fixed->geometry().right() - 1 ==
                                kit::px(kit::Spacing::ChromeGap),
                            "expanding footer controls reuse the shared gap");
        flexible->setProperty("chromeSuppressed", true);
        expectations.expect(!flexible->isVisible() && fixed->isVisible(),
                            "suppression hides only the optional footer control immediately");
        flexible->setProperty("chromeSuppressed", false);
        expectations.expect(flexible->isVisible(), "optional footer controls reappear immediately");
    }
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const QApplication application(argc, argv);
    Expectations expectations;
    testSharedFooterExpansionAndSuppression(expectations);
    testTheOldHeaderButtonsAreReallyGone(expectations);
    testTheMaximizeButtonIsTheOnlyRemainingHeaderButtonAndIsAFullscreenToggle(expectations);
    testTheHeaderQToolButtonsAreSquare(expectations);
    testTheContextMenuOffersAllFourOperationsWithFullscreenWording(expectations);
    testARightClickOnTheHeaderOpensTheMenuAtTheCursor(expectations);
    testHeaderControlsAndMenuActionsStillDriveTheirSignals(expectations);
    testSplitAndCloseEnablementIsUnchanged(expectations);
    testThePanelSwitcherPinsTheIconMapping(expectations);
    testThePanelSwitcherHugsItsContentAndKeepsBehaviorParity(expectations);
    testHeaderProportionsMatchTheDesignCrops(expectations);
    testPanelSwitcherFocusIsKeyboardOnly(expectations);
    testAFooterProvidingEditorGetsAHostedFooterNamedEditorFooter(expectations);
    testAFooterLessEditorHasNoEditorFooterChildAtAll(expectations);
    testAHeaderMenuProvidingEditorGetsItsWidgetHostedInTheHeader(expectations);
    testAHeaderMenuLessEditorHasNoExtraHeaderChild(expectations);
    testSwitchingEditorsRetiresTheOldHeaderMenuWidget(expectations);
    testTheFourCornersAreClippedToWindowBackground(expectations);
    testPanelMinWidthIsFixedRegardlessOfHostedEditorHints(expectations);
    testPropertiesIsHostedInsideAnIgnoredScrollArea(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
