#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/title_bar.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QLabel>
#include <QMenuBar>
#include <QPoint>
#include <QSignalSpy>
#include <QString>
#include <QTest>

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

void testTitleBarShapeAndIdentity(Expectations& expectations) {
    kit::TitleBar titleBar;
    expectations.expect(titleBar.objectName() == QStringLiteral("kinetikTitleBar"),
                        "the title bar carries its stable objectName");
    expectations.expect(titleBar.height() == kit::px(kit::Size::TitleBar),
                        "the title bar is exactly Size::TitleBar tall");

    auto* minimize = titleBar.findChild<kit::KButton*>(QStringLiteral("titleBarMinimizeButton"));
    auto* maximize = titleBar.findChild<kit::KButton*>(QStringLiteral("titleBarMaximizeButton"));
    auto* close = titleBar.findChild<kit::KButton*>(QStringLiteral("titleBarCloseButton"));
    expectations.expect(minimize != nullptr && maximize != nullptr && close != nullptr,
                        "all three window-control buttons exist");
    if (minimize == nullptr || maximize == nullptr || close == nullptr) {
        return;
    }
    expectations.expect(minimize->variant() == kit::KButton::Variant::Ghost &&
                            maximize->variant() == kit::KButton::Variant::Ghost &&
                            close->variant() == kit::KButton::Variant::Ghost,
                        "all three are kit ghost buttons");
    expectations.expect(close->dangerOnHover(),
                        "close hover = Error fill, standard convention -- the others do not");
    expectations.expect(!minimize->dangerOnHover() && !maximize->dangerOnHover(),
                        "minimize/maximize stay plain ghost on hover");
    expectations.expect(minimize->iconId() == kit::IconId::Minimize &&
                            maximize->iconId() == kit::IconId::Maximize &&
                            close->iconId() == kit::IconId::Close,
                        "each button carries its semantic icon");
}

void testTitleFollowsTheProjectName(Expectations& expectations) {
    kit::TitleBar titleBar;
    auto* label = titleBar.findChild<QLabel*>(QStringLiteral("titleBarTitleLabel"));
    expectations.expect(label != nullptr, "the title label exists");
    if (label == nullptr) {
        return;
    }
    titleBar.setTitle(QStringLiteral("Bloom — Untitled"));
    expectations.expect(label->text() == QStringLiteral("Bloom — Untitled"),
                        "the label shows exactly what MainWindow sets");
    titleBar.setTitle(QStringLiteral("Bloom — My Project"));
    expectations.expect(label->text() == QStringLiteral("Bloom — My Project"),
                        "the title tracks a later project name change");
}

void testButtonsEmitTheirIntents(Expectations& expectations) {
    kit::TitleBar titleBar;
    auto* minimize = titleBar.findChild<kit::KButton*>(QStringLiteral("titleBarMinimizeButton"));
    auto* maximize = titleBar.findChild<kit::KButton*>(QStringLiteral("titleBarMaximizeButton"));
    auto* close = titleBar.findChild<kit::KButton*>(QStringLiteral("titleBarCloseButton"));
    expectations.expect(minimize != nullptr && maximize != nullptr && close != nullptr,
                        "the buttons exist for the signal-wiring assertions");
    if (minimize == nullptr || maximize == nullptr || close == nullptr) {
        return;
    }

    QSignalSpy minimizeSpy(&titleBar, &kit::TitleBar::minimizeRequested);
    QSignalSpy maximizeSpy(&titleBar, &kit::TitleBar::maximizeOrRestoreRequested);
    QSignalSpy closeSpy(&titleBar, &kit::TitleBar::closeRequested);

    minimize->click();
    maximize->click();
    close->click();

    expectations.expect(minimizeSpy.count() == 1, "the minimize button requests minimize");
    expectations.expect(maximizeSpy.count() == 1, "the maximize button requests maximize/restore");
    expectations.expect(closeSpy.count() == 1, "the close button requests close");
}

void testDoubleClickOnTheEmptyAreaTogglesMaximize(Expectations& expectations) {
    kit::TitleBar titleBar;
    titleBar.resize(600, kit::px(kit::Size::TitleBar));
    QSignalSpy maximizeSpy(&titleBar, &kit::TitleBar::maximizeOrRestoreRequested);
    // Far to the right of the title label and left of the button cluster: guaranteed empty bar
    // area regardless of exact button widths, so the event lands on TitleBar itself.
    QTest::mouseDClick(&titleBar, Qt::LeftButton, Qt::NoModifier, QPoint(300, 17));
    expectations.expect(maximizeSpy.count() == 1,
                        "double-clicking the empty bar area requests maximize/restore");
}

void testMaximizeIconSwapsWithAppearance(Expectations& expectations) {
    kit::TitleBar titleBar;
    auto* maximize = titleBar.findChild<kit::KButton*>(QStringLiteral("titleBarMaximizeButton"));
    expectations.expect(maximize != nullptr, "the maximize button exists");
    if (maximize == nullptr) {
        return;
    }
    expectations.expect(!titleBar.maximizedAppearance(), "a fresh title bar starts unmaximized");
    expectations.expect(maximize->iconId() == kit::IconId::Maximize,
                        "and shows the Maximize glyph at rest");

    titleBar.setMaximized(true);
    expectations.expect(titleBar.maximizedAppearance(), "setMaximized(true) records the appearance");
    expectations.expect(maximize->iconId() == kit::IconId::Restore,
                        "maximizing swaps the glyph to Restore");
    expectations.expect(maximize->toolTip() == QStringLiteral("Restore"),
                        "and updates the tooltip to match");

    titleBar.setMaximized(false);
    expectations.expect(maximize->iconId() == kit::IconId::Maximize,
                        "restoring swaps the glyph back");
}

void testMenuBarEmbedsIntoTheRow(Expectations& expectations) {
    kit::TitleBar titleBar;
    auto* bar = new QMenuBar();
    bar->addMenu(QStringLiteral("&File"));
    titleBar.setMenuBar(bar);
    expectations.expect(bar->parentWidget() == &titleBar,
                        "setMenuBar() reparents the menu bar into the title bar's own row");
    expectations.expect(titleBar.findChild<QMenuBar*>() == bar,
                        "the embedded menu bar is really a child of the title bar");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testTitleBarShapeAndIdentity(expectations);
    testTitleFollowsTheProjectName(expectations);
    testButtonsEmitTheirIntents(expectations);
    testDoubleClickOnTheEmptyAreaTogglesMaximize(expectations);
    testMaximizeIconSwapsWithAppearance(expectations);
    testMenuBarEmbedsIntoTheRow(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
