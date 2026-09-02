#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMenu>
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

// task U2, issue #118, decision 4 -- the ONE sanctioned test-contract change: the H/V split
// QToolButtons (splitLeftRightButton/splitTopBottomButton) are gone, replaced by a single
// "panelContextMenuButton" QToolButton whose "panelOptionsMenu" QMenu offers four actions
// (panelSplitHorizontalAction/panelSplitVerticalAction/panelMaximizeAction/panelCloseAction, all
// new stable objectNames). Maximize and Close keep their original standalone QToolButtons and
// objectNames unchanged.

void testHeaderControlsAreIconsWithTheirNamesIntact(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    EditorArea area(registry, "bloom.probe", QString{});

    expectations.expect(
        area.findChild<QToolButton*>(QStringLiteral("splitLeftRightButton")) == nullptr &&
            area.findChild<QToolButton*>(QStringLiteral("splitTopBottomButton")) == nullptr,
        "the old split buttons are really gone, not merely relabeled");

    struct Contract {
        const char* objectName;
        const char* toolTip;
    };
    for (const auto& [objectName, toolTip] :
         {Contract{"panelContextMenuButton", "Panel options"},
          Contract{"maximizeAreaButton", "Maximize area"},
          Contract{"closeAreaButton", "Close area"}}) {
        auto* button = area.findChild<QToolButton*>(QString::fromLatin1(objectName));
        expectations.expect(button != nullptr,
                            std::string{"the header exposes "} + objectName);
        if (button == nullptr) {
            continue;
        }
        expectations.expect(!button->icon().isNull(),
                            std::string{objectName} + " draws a Kinetik icon");
        expectations.expect(button->text().isEmpty(),
                            std::string{objectName} + " carries no typed glyph");
        expectations.expect(button->toolTip() == QString::fromLatin1(toolTip),
                            std::string{objectName} + " has its tooltip");
        expectations.expect(button->accessibleName() == QString::fromLatin1(toolTip),
                            std::string{objectName} + " has its accessible name: an icon never "
                                                      "replaces one");
        expectations.expect(button->iconSize().width() == kit::px(kit::Size::IconSmall),
                            std::string{objectName} + " uses the dense-chrome icon box");
    }

}

void testTheContextMenuOffersAllFourOperations(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    EditorArea area(registry, "bloom.probe", QString{});
    auto* contextButton =
        area.findChild<QToolButton*>(QStringLiteral("panelContextMenuButton"));
    expectations.expect(contextButton != nullptr, "the context-menu button exists");
    if (contextButton == nullptr) {
        return;
    }
    auto* menu = contextButton->menu();
    expectations.expect(menu != nullptr && menu->objectName() == QStringLiteral("panelOptionsMenu"),
                        "the button owns a kit-styled, identifiable QMenu");
    if (menu == nullptr) {
        return;
    }
    for (const char* objectName :
         {"panelSplitHorizontalAction", "panelSplitVerticalAction", "panelMaximizeAction",
          "panelCloseAction"}) {
        expectations.expect(menu->findChild<QAction*>(QString::fromLatin1(objectName)) != nullptr,
                            std::string{"the menu offers "} + objectName);
    }
}

void testMaximizeSwapsIconAndNamesWithoutChangingIdentity(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    EditorArea area(registry, "bloom.probe", QString{});
    auto* button = area.findChild<QToolButton*>(QStringLiteral("maximizeAreaButton"));
    expectations.expect(button != nullptr, "the maximize control exists");
    if (button == nullptr) {
        return;
    }

    const qint64 restingIcon = button->icon().cacheKey();
    area.setMaximizedAppearance(true);
    expectations.expect(button->icon().cacheKey() != restingIcon,
                        "maximizing swaps the glyph rather than only the tooltip");
    expectations.expect(button->toolTip() == QStringLiteral("Restore area"),
                        "maximizing updates the tooltip");
    expectations.expect(button->accessibleName() == QStringLiteral("Restore area"),
                        "maximizing updates the accessible name alongside the glyph");
    expectations.expect(button->objectName() == QStringLiteral("maximizeAreaButton"),
                        "the objectName is a contract and never changes with appearance");

    area.setMaximizedAppearance(false);
    expectations.expect(button->toolTip() == QStringLiteral("Maximize area"),
                        "restoring puts the resting tooltip back");
}

void testHeaderControlsStillDriveTheirSignals(Expectations& expectations) {
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
    area.findChild<QToolButton*>(QStringLiteral("closeAreaButton"))->click();
    area.findChild<QToolButton*>(QStringLiteral("maximizeAreaButton"))->click();
    // The menu's own Maximize/Close actions route to the SAME signals the standalone buttons do.
    menu->findChild<QAction*>(QStringLiteral("panelMaximizeAction"))->trigger();
    menu->findChild<QAction*>(QStringLiteral("panelCloseAction"))->trigger();

    expectations.expect(splitSpy.count() == 2, "both menu split actions request a split");
    expectations.expect(closeSpy.count() == 2,
                        "the close button AND the menu's close action request a close");
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
        "split enablement now reaches the menu's two split actions");
    expectations.expect(
        !area.findChild<QToolButton*>(QStringLiteral("closeAreaButton"))->isEnabled() &&
            !menu->findChild<QAction*>(QStringLiteral("panelCloseAction"))->isEnabled(),
        "close enablement reaches both the standalone button and the menu's close action");

    area.setSplitEnabled(true);
    area.setCloseEnabled(true);
    expectations.expect(
        menu->findChild<QAction*>(QStringLiteral("panelSplitHorizontalAction"))->isEnabled() &&
            menu->findChild<QAction*>(QStringLiteral("panelSplitVerticalAction"))->isEnabled(),
        "and re-enabling reaches them too");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const QApplication application(argc, argv);
    Expectations expectations;
    testHeaderControlsAreIconsWithTheirNamesIntact(expectations);
    testTheContextMenuOffersAllFourOperations(expectations);
    testMaximizeSwapsIconAndNamesWithoutChangingIdentity(expectations);
    testHeaderControlsStillDriveTheirSignals(expectations);
    testSplitAndCloseEnablementIsUnchanged(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
