#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QIcon>
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

void testHeaderControlsAreIconsWithTheirNamesIntact(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    EditorArea area(registry, "bloom.probe", QString{});

    struct Contract {
        const char* objectName;
        const char* toolTip;
    };
    for (const auto& [objectName, toolTip] :
         {Contract{"splitLeftRightButton", "Split area left/right"},
          Contract{"splitTopBottomButton", "Split area top/bottom"},
          Contract{"maximizeAreaButton", "Maximize area"},
          Contract{"closeAreaButton", "Close area"}}) {
        auto* button = area.findChild<QToolButton*>(QString::fromLatin1(objectName));
        expectations.expect(button != nullptr,
                            std::string{"the header still exposes "} + objectName);
        if (button == nullptr) {
            continue;
        }
        expectations.expect(!button->icon().isNull(),
                            std::string{objectName} + " draws a Kinetik icon");
        expectations.expect(button->text().isEmpty(),
                            std::string{objectName} + " no longer carries a typed glyph");
        expectations.expect(button->toolTip() == QString::fromLatin1(toolTip),
                            std::string{objectName} + " keeps its tooltip");
        expectations.expect(button->accessibleName() == QString::fromLatin1(toolTip),
                            std::string{objectName} + " keeps its accessible name: an icon never "
                                                      "replaces one");
        expectations.expect(button->iconSize().width() == kit::px(kit::Size::IconSmall),
                            std::string{objectName} + " uses the dense-chrome icon box");
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

    QSignalSpy splitSpy(&area, &EditorArea::splitRequested);
    QSignalSpy closeSpy(&area, &EditorArea::closeRequested);
    QSignalSpy maximizeSpy(&area, &EditorArea::maximizeRequested);

    area.findChild<QToolButton*>(QStringLiteral("splitLeftRightButton"))->click();
    area.findChild<QToolButton*>(QStringLiteral("splitTopBottomButton"))->click();
    area.findChild<QToolButton*>(QStringLiteral("closeAreaButton"))->click();
    area.findChild<QToolButton*>(QStringLiteral("maximizeAreaButton"))->click();

    // The split controls keep their behavior in this slice: replacing them with a context menu is
    // a later slice's decision, not a side effect of an icon change.
    expectations.expect(splitSpy.count() == 2, "both split controls still request a split");
    expectations.expect(closeSpy.count() == 1, "the close control still requests a close");
    expectations.expect(maximizeSpy.count() == 1, "the maximize control still requests a maximize");
}

void testSplitAndCloseEnablementIsUnchanged(Expectations& expectations) {
    const EditorRegistry registry = makeRegistry();
    EditorArea area(registry, "bloom.probe", QString{});
    area.setSplitEnabled(false);
    area.setCloseEnabled(false);
    expectations.expect(
        !area.findChild<QToolButton*>(QStringLiteral("splitLeftRightButton"))->isEnabled() &&
            !area.findChild<QToolButton*>(QStringLiteral("splitTopBottomButton"))->isEnabled(),
        "split enablement still reaches both controls");
    expectations.expect(
        !area.findChild<QToolButton*>(QStringLiteral("closeAreaButton"))->isEnabled(),
        "close enablement still reaches its control");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const QApplication application(argc, argv);
    Expectations expectations;
    testHeaderControlsAreIconsWithTheirNamesIntact(expectations);
    testMaximizeSwapsIconAndNamesWithoutChangingIdentity(expectations);
    testHeaderControlsStillDriveTheirSignals(expectations);
    testSplitAndCloseEnablementIsUnchanged(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
