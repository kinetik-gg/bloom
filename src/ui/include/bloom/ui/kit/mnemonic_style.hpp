#pragma once

#include <QProxyStyle>
#include <QSize>
#include <QString>

class QPainter;
class QStyleOption;
class QApplication;
class QWidget;

namespace bloom::ui::kit {

// Bloom's application proxy style: the handful of things the Kinetik look needs from Qt's own
// widgets that neither a stylesheet nor a kit widget can deliver.
//
// 1. Mnemonic underlines only while Alt is held (task U2, issue #118). Bloom's menu bar and menus
//    carry "&File"-style mnemonics, but the underline itself must stay hidden until the artist
//    actually presses Alt, matching the platform-standard "hold Alt to reveal access keys"
//    convention rather than showing every underline permanently. Qt's own style hint
//    (QStyle::SH_UnderlineShortcut) decides this, and the platform/Fusion default does not
//    reliably key it to the live Alt state on every platform Bloom ships to.
// 2. No Qt focus rectangle anywhere (task F1, item F2).
// 3. Every application menu row (task F1, item F5): a reserved icon column, the shortcut column,
//    and the submenu caret. A stylesheet can reach none of those three -- QSS has no selector for
//    a menu item's shortcut text at all -- and the moment ANY QMenu rule with a box exists,
//    QStyleSheetStyle draws the whole item itself and this style is never called. Verified
//    empirically, which is why the theme's sheet carries no QMenu rule at all: the menu frame and
//    every row it holds are painted here.
//
// Scope: installed once, application-wide, in apps/bloom/main.cpp right after
// installKinetikTheme() -- see that call site's comment. Not a per-widget style.
class AltUnderlineProxyStyle final : public QProxyStyle {
    Q_OBJECT

  public:
    // Wraps a freshly created "Fusion" base style (the same style installKinetikTheme() selects),
    // so replacing the application style with this proxy never double-frees or aliases the style
    // object installKinetikTheme() already installed.
    AltUnderlineProxyStyle();
    explicit AltUnderlineProxyStyle(QStyle* baseStyle);

    [[nodiscard]] int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
                                const QWidget* widget = nullptr,
                                QStyleHintReturn* returnData = nullptr) const override;

    // Task F1, item F2: Qt's own dotted focus rectangle is never drawn. Bloom's focus affordance is
    // the control's single border turning Accent (kit::borderForInteraction), so letting the base
    // style add PE_FrameFocusRect on top would be exactly the second outline that rule forbids --
    // and on a kit widget, which paints its own border, it would read as a double ring. Suppressed
    // here rather than per widget because the widgets that would receive it are Qt's own
    // (QToolButton, QMenu, item views), which no kit paint method touches.
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget = nullptr) const override;

    // Task F1, item F5: CE_MenuItem is drawn here, in full.
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget = nullptr) const override;

    // The row metrics that go with it: CT_MenuItem's width reserves the icon column, the shortcut
    // column, and the submenu caret whether or not this particular row uses them.
    [[nodiscard]] QSize sizeFromContents(ContentsType type, const QStyleOption* option,
                                         const QSize& size,
                                         const QWidget* widget = nullptr) const override;

    // The menu frame's own metrics: its hairline, and the vertical padding above the first row and
    // below the last.
    [[nodiscard]] int pixelMetric(PixelMetric metric, const QStyleOption* option = nullptr,
                                  const QWidget* widget = nullptr) const override;

    // A menu is painted with rounded corners, so its window must be able to leave those corners
    // unpainted rather than showing a square plate behind them.
    void polish(QWidget* widget) override;
    using QProxyStyle::polish;
};

// The pure decision AltUnderlineProxyStyle::styleHint() applies for SH_UnderlineShortcut, factored
// out so both branches (Alt held / Alt not held) are directly unit-testable without depending on
// real global keyboard state, which an offscreen test cannot reliably drive.
[[nodiscard]] bool showMnemonicUnderline(Qt::KeyboardModifiers modifiers) noexcept;

// Focus is a visible affordance only when Qt says focus arrived through keyboard traversal. The
// tracker records that reason on each widget so custom kit painters and QSS selectors share one
// application-wide decision instead of treating pointer focus as a focus ring.
void installKeyboardFocusTracking(QApplication& application);
void ensureKeyboardFocusTracking(QWidget& widget);
[[nodiscard]] bool hasKeyboardFocus(const QWidget& widget) noexcept;

// Where a menu row's text starts, measured from the row's own left edge, in design pixels
// (task F1, item F5). One number for every row: the icon column is reserved whether the row has an
// icon or not, so "Open" and an icon-bearing "Save" put their first letter at exactly the same x.
[[nodiscard]] int menuItemTextOffset();

// The width the icon column occupies, and the row's vertical padding -- exposed so a test can
// state the layout rule in the same terms the painter does rather than re-deriving it.
[[nodiscard]] int menuIconColumnWidth();

} // namespace bloom::ui::kit
