#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QIcon>
#include <QMenu>
#include <QPoint>
#include <QSignalSpy>
#include <QString>
#include <QToolButton>
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
    expectations.expect(button->iconSize().width() == kit::px(kit::Size::IconSmall),
                        "maximizeAreaButton uses the dense-chrome icon box");

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
    expectations.expect(
        button->width() == kit::px(kit::Size::Control) - 4,
        "square at Size::Control minus the header's own vertical padding, per fix 7");
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

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const QApplication application(argc, argv);
    Expectations expectations;
    testTheOldHeaderButtonsAreReallyGone(expectations);
    testTheMaximizeButtonIsTheOnlyRemainingHeaderButtonAndIsAFullscreenToggle(expectations);
    testTheHeaderQToolButtonsAreSquare(expectations);
    testTheContextMenuOffersAllFourOperationsWithFullscreenWording(expectations);
    testARightClickOnTheHeaderOpensTheMenuAtTheCursor(expectations);
    testHeaderControlsAndMenuActionsStillDriveTheirSignals(expectations);
    testSplitAndCloseEnablementIsUnchanged(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
