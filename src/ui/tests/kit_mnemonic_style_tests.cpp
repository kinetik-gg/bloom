#include <bloom/ui/kit/mnemonic_style.hpp>

#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QImage>
#include <QKeySequence>
#include <QMenu>
#include <QRect>
#include <QRegion>
#include <QStyle>
#include <QStyleFactory>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
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

// The pure decision both branches: an offscreen test cannot reliably synthesize a real, global
// Alt-held keyboard state (QGuiApplication::keyboardModifiers() reflects actual hardware/compositor
// state, not something a unit test can fake by posting an event), so both branches of the
// SH_UnderlineShortcut decision are asserted directly against kit::showMnemonicUnderline() instead
// of through a live style-hint query under a simulated key press. Reported per the task's own
// "say which" guidance.
void testUnderlineShortcutDecisionBothWays(Expectations& expectations) {
    expectations.expect(!kit::showMnemonicUnderline(Qt::NoModifier),
                        "mnemonics stay hidden with no modifier held");
    expectations.expect(!kit::showMnemonicUnderline(Qt::ShiftModifier | Qt::ControlModifier),
                        "mnemonics stay hidden for unrelated modifiers");
    expectations.expect(kit::showMnemonicUnderline(Qt::AltModifier),
                        "mnemonics reveal while Alt is held");
    expectations.expect(kit::showMnemonicUnderline(Qt::AltModifier | Qt::ShiftModifier),
                        "Alt still reveals mnemonics alongside another modifier");
}

// The real, live keyboard state in an offscreen test process has no Alt held, so the installed
// proxy's actual styleHint() call is asserted against exactly that one guaranteed branch --
// confirming the override is really wired to QStyle::styleHint() and not just to the pure helper
// above.
void testInstalledProxyRoutesThroughToTheRealStyleHintQuery(Expectations& expectations) {
    kit::AltUnderlineProxyStyle style;
    const int hint = style.styleHint(QStyle::SH_UnderlineShortcut);
    expectations.expect(hint == 0,
                        "with no Alt held (the only state an offscreen process can guarantee), "
                        "the installed proxy hides mnemonic underlines");

    // Every other style hint is still delegated to the wrapped base style rather than swallowed:
    // spot-check one hint that has nothing to do with mnemonics.
    const int tabFocus = style.styleHint(QStyle::SH_Widget_ShareActivation);
    const auto* fusion = QStyleFactory::create(QStringLiteral("Fusion"));
    expectations.expect(fusion != nullptr,
                        "a reference Fusion style is available to compare against");
    if (fusion != nullptr) {
        expectations.expect(tabFocus == fusion->styleHint(QStyle::SH_Widget_ShareActivation),
                            "hints other than SH_UnderlineShortcut fall through to the base style "
                            "unchanged");
        delete fusion;
    }
}

// Task F1, item F5. The menu rows are drawn by this proxy style, not by the stylesheet, and the
// icon column is reserved whether or not a row has an icon -- so an icon-less item and an
// icon-bearing one start their text at exactly the same x.
//
// Measured off a real offscreen render rather than off the layout code that produced it: the
// question is what the artist sees, and a reserved column that the painter then ignores would
// pass any geometry-only assertion.
[[nodiscard]] std::optional<int> firstInkedColumn(const QImage& canvas, const QRect& band,
                                                  const int fromX, const QColor& ground) {
    for (int x = std::max(fromX, band.left()); x <= band.right() && x < canvas.width(); ++x) {
        for (int y = band.top(); y <= band.bottom() && y < canvas.height(); ++y) {
            if (y < 0 || x < 0) {
                continue;
            }
            if (canvas.pixelColor(x, y) != ground) {
                return x;
            }
        }
    }
    return std::nullopt;
}

void testMenuRowsReserveOneIconColumnAndDrawSubmenus(Expectations& expectations) {
    QMenu menu;
    // The same label twice: the only difference between the two rows is the icon, so any
    // difference in where the text lands is the icon column failing to be constant.
    auto* plain = menu.addAction(QStringLiteral("Item"));
    plain->setShortcut(QKeySequence(QStringLiteral("Ctrl+O")));
    auto* withIcon = menu.addAction(kit::icon(kit::IconId::Folder, kit::Size::IconSmall),
                                    QStringLiteral("Item"));
    auto* submenu = menu.addMenu(QStringLiteral("More"));
    submenu->addAction(QStringLiteral("Deeper"));
    menu.show();
    QCoreApplication::processEvents();

    const QColor ground = kit::color(kit::Color::SurfaceRaised);
    QImage canvas(menu.size(), QImage::Format_ARGB32_Premultiplied);
    canvas.fill(ground);
    menu.render(&canvas, QPoint(), QRegion(), QWidget::DrawChildren);

    const QRect plainRow = menu.actionGeometry(plain);
    const QRect iconRow = menu.actionGeometry(withIcon);
    const QRect submenuRow = menu.actionGeometry(submenu->menuAction());
    expectations.expect(plainRow.isValid() && iconRow.isValid() && submenuRow.isValid(),
                        "the menu laid out all three rows");

    const int textStart = kit::menuItemTextOffset();
    const auto plainText = firstInkedColumn(canvas, plainRow, plainRow.left() + textStart, ground);
    const auto iconText = firstInkedColumn(canvas, iconRow, iconRow.left() + textStart, ground);
    expectations.expect(plainText.has_value() && iconText.has_value(),
                        "both rows actually painted their label");
    if (plainText.has_value() && iconText.has_value()) {
        expectations.expect(*plainText - plainRow.left() == *iconText - iconRow.left(),
                            "an icon-less row and an icon row start their text at the same x (" +
                                std::to_string(*plainText - plainRow.left()) + " vs " +
                                std::to_string(*iconText - iconRow.left()) + ')');
    }

    // The icon really is inside the reserved column, ahead of the text, and the icon-less row
    // leaves that column empty rather than pulling its label left into it.
    const auto iconInk = firstInkedColumn(
        canvas, iconRow, iconRow.left() + kit::px(kit::Spacing::MenuItemX), ground);
    expectations.expect(iconInk.has_value() && *iconInk - iconRow.left() < textStart,
                        "the icon paints inside the reserved column");
    const auto plainInk = firstInkedColumn(
        canvas, plainRow, plainRow.left() + kit::px(kit::Spacing::MenuItemX), ground);
    expectations.expect(plainInk.has_value() && plainText.has_value() &&
                            plainInk.value() == plainText.value(),
                        "an icon-less row paints nothing at all in the reserved column");

    // The submenu row carries the caret: something is inked in the arrow column on its right.
    const QRect arrowBand(submenuRow.right() - kit::px(kit::Spacing::MenuItemX) -
                              kit::px(kit::Size::IconSmall),
                          submenuRow.top(), kit::px(kit::Size::IconSmall), submenuRow.height());
    expectations.expect(firstInkedColumn(canvas, arrowBand, arrowBand.left(), ground).has_value(),
                        "the submenu row draws its caret in the trailing arrow column");
    const QRect plainArrowBand(plainRow.right() - kit::px(kit::Spacing::MenuItemX) -
                                   kit::px(kit::Size::IconSmall),
                               plainRow.top(), kit::px(kit::Size::IconSmall), plainRow.height());
    expectations.expect(submenuRow.width() == plainRow.width(),
                        "every row is the same width, so the arrow column is a real column");
    (void)plainArrowBand;

    // The shortcut is painted, and in the quiet ink -- never the label's Foreground. Text is
    // antialiased, and the runner's font rasterization need not produce a single pixel at the
    // exact token value, so the pin compares the brightest ink of the label's half of the row
    // with the brightest ink of the shortcut's half: the label reaches well above Faint, the
    // shortcut stays well below Foreground, and both are inked above the ground.
    const auto brightest = [&](const int left, const int right) {
        int value = 0;
        for (int x = std::max(left, 0); x <= right && x < canvas.width(); ++x) {
            for (int y = plainRow.top(); y <= plainRow.bottom() && y < canvas.height(); ++y) {
                const QColor pixel = canvas.pixelColor(x, y);
                if (pixel != ground) {
                    value = std::max(value, pixel.lightness());
                }
            }
        }
        return value;
    };
    const int middle = plainRow.left() + plainRow.width() / 2;
    const int labelInk = brightest(plainRow.left(), middle);
    const int shortcutInk = brightest(middle + 1, plainRow.right());
    const int faintLightness = kit::color(kit::Color::Faint).lightness();
    const int foregroundLightness = kit::color(kit::Color::Foreground).lightness();
    expectations.expect(shortcutInk > ground.lightness() && shortcutInk <= faintLightness + 24 &&
                            shortcutInk < foregroundLightness - 64,
                        "the shortcut column is painted in Faint, the quietest ink in the row");
    expectations.expect(labelInk > shortcutInk + 40 && labelInk > faintLightness + 24,
                        "and the label itself is still full-strength Foreground");
}

// Task S1, item 3: no menu is narrower than Size::MenuMinWidth, and the rows still reach the
// frame's own edges so their hover bars stay full width.
void testEveryMenuIsAtLeastTheMinimumWidth(Expectations& expectations) {
    QMenu menu;
    auto* fit = menu.addAction(QStringLiteral("Fit"));
    menu.addSeparator();
    menu.addAction(QStringLiteral("100%"));
    menu.show();
    QCoreApplication::processEvents();

    const QRect row = menu.actionGeometry(fit);
    expectations.expect(
        row.width() >= kit::px(kit::Size::MenuMinWidth),
        "a menu of short labels still lays its rows out at the minimum width, got " +
            std::to_string(row.width()));
    expectations.expect(menu.width() >= kit::px(kit::Size::MenuMinWidth),
                        "so the popup itself is at least that wide, got " +
                            std::to_string(menu.width()));
    // The frame's own hairline on each side is all that separates a row from the popup's edge: the
    // row is not inset any further, which is what makes its accent hover bar full width.
    const int frame = menu.style()->pixelMetric(QStyle::PM_MenuPanelWidth, nullptr, &menu) +
                      menu.style()->pixelMetric(QStyle::PM_MenuHMargin, nullptr, &menu);
    expectations.expect(row.left() == frame && row.right() == menu.width() - 1 - frame,
                        "and the row runs edge to edge inside the frame");

    // A long label still widens the menu past the minimum: this is a floor, not a fixed width.
    QMenu wide;
    auto* verbose = wide.addAction(
        QStringLiteral("A deliberately long menu label that outgrows the minimum width"));
    wide.show();
    QCoreApplication::processEvents();
    expectations.expect(wide.actionGeometry(verbose).width() > kit::px(kit::Size::MenuMinWidth),
                        "the minimum width is a floor, never a cap");
}

void testTheMenuFrameAndRowMetricsComeFromTokens(Expectations& expectations) {
    QMenu menu;
    auto* first = menu.addAction(QStringLiteral("Item"));
    menu.addSeparator();
    menu.addAction(QStringLiteral("Other"));
    menu.show();
    QCoreApplication::processEvents();

    expectations.expect(menu.font().pointSizeF() == kit::font(kit::TypeRole::Ui).pointSizeF(),
                        "a menu is set in the UI type role");
    const QRect firstRow = menu.actionGeometry(first);
    expectations.expect(firstRow.top() >= kit::px(kit::Spacing::XS),
                        "the frame keeps its own vertical padding above the first row");
    expectations.expect(kit::menuItemTextOffset() == kit::px(kit::Spacing::MenuItemX) +
                                                         kit::menuIconColumnWidth() +
                                                         kit::px(kit::Spacing::S),
                        "the text offset is the row's own horizontal padding, the reserved icon "
                        "column, and one gap -- stated once");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    // The menu tests below assert what the APPLICATION draws, so they need the application's own
    // theme and style installed exactly as apps/bloom/main.cpp installs them.
    try {
        kit::installKinetikTheme(application);
        // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks) -- QApplication::setStyle() owns
        // it, exactly as at the real install site in apps/bloom/main.cpp.
        auto* kitStyle = new kit::AltUnderlineProxyStyle;
        QApplication::setStyle(kitStyle);
        Expectations expectations;
        testUnderlineShortcutDecisionBothWays(expectations);
        testInstalledProxyRoutesThroughToTheRealStyleHintQuery(expectations);
        testMenuRowsReserveOneIconColumnAndDrawSubmenus(expectations);
        testTheMenuFrameAndRowMetricsComeFromTokens(expectations);
        testEveryMenuIsAtLeastTheMinimumWidth(expectations);
        return expectations.failures() == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "kit mnemonic style test failed with an exception: " << error.what() << '\n';
        return 1;
    }
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
}
