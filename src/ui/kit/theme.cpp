#include <bloom/ui/kit/theme.hpp>

#include <bloom/ui/kit/fonts.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/typography.hpp>

#include <QApplication>
#include <QColor>
#include <QLatin1StringView>
#include <QStringList>

#include <array>
#include <utility>

namespace bloom::ui::kit {
namespace {

struct ColorPlaceholder {
    QLatin1StringView name;
    Color token;
};

const auto& colorPlaceholders() {
    static const auto entries = std::to_array<ColorPlaceholder>({
        {QLatin1StringView("Background"), Color::Background},
        {QLatin1StringView("SurfaceSunken"), Color::SurfaceSunken},
        {QLatin1StringView("Surface"), Color::Surface},
        {QLatin1StringView("SurfaceRaised"), Color::SurfaceRaised},
        {QLatin1StringView("Field"), Color::Field},
        {QLatin1StringView("Foreground"), Color::Foreground},
        {QLatin1StringView("Muted"), Color::Muted},
        {QLatin1StringView("Faint"), Color::Faint},
        {QLatin1StringView("Border"), Color::Border},
        {QLatin1StringView("BorderHover"), Color::BorderHover},
        {QLatin1StringView("BorderActive"), Color::BorderActive},
        {QLatin1StringView("Accent"), Color::Accent},
        {QLatin1StringView("OnAccent"), Color::OnAccent},
        {QLatin1StringView("AccentHover"), Color::AccentHover},
        {QLatin1StringView("AccentPressed"), Color::AccentPressed},
        {QLatin1StringView("Keyframe"), Color::Keyframe},
        {QLatin1StringView("Ok"), Color::Ok},
        {QLatin1StringView("Warn"), Color::Warn},
        {QLatin1StringView("Error"), Color::Error},
    });
    return entries;
}

struct NumberPlaceholder {
    QLatin1StringView name;
    int value;
};

const auto& numberPlaceholders() {
    static const auto entries = std::to_array<NumberPlaceholder>({
        {QLatin1StringView("space.XXS"), px(Spacing::XXS)},
        {QLatin1StringView("space.XS"), px(Spacing::XS)},
        {QLatin1StringView("space.S"), px(Spacing::S)},
        {QLatin1StringView("space.M"), px(Spacing::M)},
        {QLatin1StringView("space.L"), px(Spacing::L)},
        {QLatin1StringView("space.XL"), px(Spacing::XL)},
        {QLatin1StringView("space.XXL"), px(Spacing::XXL)},
        {QLatin1StringView("space.Gutter"), px(Spacing::Gutter)},
        {QLatin1StringView("space.PanelHeader"), px(Spacing::PanelHeader)},
        {QLatin1StringView("space.MenuItemY"), px(Spacing::MenuItemY)},
        {QLatin1StringView("space.MenuItemX"), px(Spacing::MenuItemX)},
        {QLatin1StringView("radius.Small"), radiusPx(Radius::Small, 0)},
        {QLatin1StringView("radius.Medium"), radiusPx(Radius::Medium, 0)},
        {QLatin1StringView("radius.Large"), radiusPx(Radius::Large, 0)},
        {QLatin1StringView("radius.XLarge"), radiusPx(Radius::XLarge, 0)},
        {QLatin1StringView("radius.Panel"), radiusPx(Radius::Panel, 0)},
        {QLatin1StringView("size.Control"), px(Size::Control)},
        {QLatin1StringView("size.ControlRoomy"), px(Size::ControlRoomy)},
        {QLatin1StringView("size.IconSmall"), px(Size::IconSmall)},
        {QLatin1StringView("size.IconMedium"), px(Size::IconMedium)},
        {QLatin1StringView("size.IconLarge"), px(Size::IconLarge)},
        {QLatin1StringView("size.TitleBar"), px(Size::TitleBar)},
        {QLatin1StringView("size.HeaderRow"), px(Size::HeaderRow)},
        {QLatin1StringView("size.ListRow"), px(Size::ListRow)},
        {QLatin1StringView("size.ScrollBarTrack"), px(Size::ScrollBarTrack)},
        {QLatin1StringView("size.ScrollBarThumbInset"),
         (px(Size::ScrollBarTrack) - px(Size::ScrollBarThumb)) / 2},
        {QLatin1StringView("size.MenuMinWidth"), px(Size::MenuMinWidth)},
        // A scrollbar thumb is a pill: Radius::Full against its own width, resting and hovered.
        {QLatin1StringView("radius.ScrollBarThumb"),
         radiusPx(Radius::Full, px(Size::ScrollBarThumb))},
        {QLatin1StringView("radius.ScrollBarThumbHover"),
         radiusPx(Radius::Full, px(Size::ScrollBarTrack))},
        {QLatin1StringView("border.Hairline"), static_cast<int>(kHairlineWidth)},
        {QLatin1StringView("border.Window"), static_cast<int>(kWindowBorderWidth)},
    });
    return entries;
}

[[nodiscard]] QString rgba(const QColor& value) {
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(value.red())
        .arg(value.green())
        .arg(value.blue())
        .arg(QString::number(static_cast<double>(value.alphaF()), 'f', 3));
}

// A colour as a QSS value: hex when opaque (so the token-hex audit can read it), rgba otherwise.
[[nodiscard]] QString cssColor(const QColor& value) {
    if (!value.isValid()) {
        return QStringLiteral("transparent");
    }
    return value.alpha() == 255 ? value.name(QColor::HexRgb) : rgba(value);
}

// One QSS declaration block for a button in one state, straight from kit::buttonColors().
[[nodiscard]] QString buttonDeclarations(const ButtonColors& colors) {
    return QStringLiteral("    color: %1;\n    background: %2;\n    border-color: %3;\n")
        .arg(cssColor(colors.ink), cssColor(colors.fill), cssColor(colors.border));
}

// The generated rules for a stock-painted button class, one per state of the ONE button recipe,
// so a QPushButton or a KMenuButton cannot drift from what KButton paints. The keyboard-focus rule
// comes last: it outranks hover at equal specificity, which is the "focus wins" rule.
[[nodiscard]] QString buttonRules(const QString& selector, const ButtonKind kind) {
    QString rules;
    const auto rule = [&rules](const QString& target, const ButtonColors& colors) {
        rules +=
            target + QStringLiteral(" {\n") + buttonDeclarations(colors) + QStringLiteral("}\n");
    };
    rule(selector, buttonColors(kind, State::Normal, false));
    rule(selector + QStringLiteral(":hover"), buttonColors(kind, State::Hover, false));
    rule(selector + QStringLiteral(":pressed"), buttonColors(kind, State::Pressed, false));
    rule(selector + QStringLiteral(":checked"), buttonColors(kind, State::Selected, false));
    rule(selector + QStringLiteral(":disabled"), buttonColors(kind, State::Disabled, false));
    rules += selector + QStringLiteral("[bloomKeyboardFocus=\"true\"] {\n    border-color: ") +
             cssColor(buttonColors(kind, State::Normal, true).border) + QStringLiteral(";\n}\n");
    return rules;
}

// The generated rules for the kit text fields (KTextField and its KLineEdit/KSearchField forms, and
// the multi-line KTextArea), from the one text-entry recipe (kit::fieldColors), so a typed name
// reads exactly like a typed number. The well is drawn in the control body: a ControlInset margin
// inside the Control-tall widget, like every kit control, with the kit's Spacing::S text padding --
// the padding a KValueField cell gives its number. A refused entry ("kitError") wears Error over
// every other state. An embedded field ("kitEmbedded": the editor inside a KValueField) draws no
// box at all, because its host paints the cell and the one border.
[[nodiscard]] QString lineEditRules() {
    QString rules;
    const auto block = [&rules](const QString& selector, const QString& pseudo,
                                const ButtonColors& colors) {
        rules += selector + pseudo + QStringLiteral(" {\n") + buttonDeclarations(colors) +
                 QStringLiteral("}\n");
    };
    for (const auto& selector : {QStringLiteral("QLineEdit[kitControl=\"true\"]"),
                                 QStringLiteral("QPlainTextEdit[kitControl=\"true\"]")}) {
        rules += selector + QStringLiteral(" {\n    border: %1px solid transparent;\n"
                                           "    border-radius: %2px;\n    margin: %3px;\n"
                                           "    padding: 0px %4px;\n"
                                           "    selection-background-color: %5;\n"
                                           "    selection-color: %6;\n}\n")
                                .arg(static_cast<int>(kHairlineWidth))
                                .arg(radiusPx(Radius::Small, 0))
                                .arg(px(Spacing::ControlInset))
                                .arg(px(Spacing::S))
                                .arg(hex(Color::Accent), hex(Color::Foreground));
        block(selector, {}, fieldColors(true, false, false, false));
        block(selector, QStringLiteral(":hover"), fieldColors(true, false, true, false));
        block(selector, QStringLiteral(":focus"), fieldColors(true, true, false, false));
        block(selector, QStringLiteral(":disabled"), fieldColors(false, false, false, false));
        rules += selector + QStringLiteral("[kitError=\"true\"] {\n    border-color: ") +
                 hex(Color::Error) + QStringLiteral(";\n}\n");
    }
    rules += QStringLiteral("QLineEdit[kitEmbedded=\"true\"] {\n    background: transparent;\n"
                            "    border: none;\n    padding: 0px;\n    margin: 0px;\n"
                            "    color: %1;\n    selection-background-color: %2;\n"
                            "    selection-color: %1;\n}\n")
                 .arg(hex(Color::Foreground), hex(Color::Accent));
    return rules;
}

} // namespace

QString expandTokens(const QString& templateText) {
    QString text = templateText;
    for (const auto& [name, token] : colorPlaceholders()) {
        text.replace(QStringLiteral("{color.%1}").arg(name), hex(token));
    }
    text.replace(QStringLiteral("{color.DisabledInk}"),
                 rgba(disabledInk(color(Color::Foreground))));
    for (const auto& [name, value] : numberPlaceholders()) {
        text.replace(QStringLiteral("{%1}").arg(name), QString::number(value));
    }
    return text;
}

QPalette kinetikPalette() {
    QPalette palette;
    const QColor ink = color(Color::Foreground);

    palette.setColor(QPalette::Window, color(Color::Background));
    palette.setColor(QPalette::WindowText, ink);
    palette.setColor(QPalette::Base, color(Color::Background));
    palette.setColor(QPalette::AlternateBase, color(Color::Surface));
    palette.setColor(QPalette::Text, ink);
    palette.setColor(QPalette::Button, color(Color::Surface));
    palette.setColor(QPalette::ButtonText, ink);
    palette.setColor(QPalette::BrightText, ink);
    palette.setColor(QPalette::Highlight, color(Color::Accent));
    palette.setColor(QPalette::HighlightedText, ink);
    palette.setColor(QPalette::PlaceholderText, color(Color::Faint));
    palette.setColor(QPalette::ToolTipBase, color(Color::SurfaceRaised));
    palette.setColor(QPalette::ToolTipText, ink);
    palette.setColor(QPalette::Link, color(Color::Accent));
    palette.setColor(QPalette::LinkVisited, color(Color::AccentPressed));

    // The three-dimensional roles carry Kinetik's separator vocabulary rather than Qt's default
    // light-theme bevel greys. Mid in particular is what timeline_ruler.cpp draws its ruler
    // baseline and keyframe-panel separator with, so it must stay legible against Base: Faint, not
    // Border, which would be all but invisible on #111111.
    palette.setColor(QPalette::Mid, color(Color::Faint));
    palette.setColor(QPalette::Midlight, color(Color::SurfaceRaised));
    palette.setColor(QPalette::Light, color(Color::BorderHover));
    palette.setColor(QPalette::Dark, color(Color::Border));
    palette.setColor(QPalette::Shadow, QColor(0, 0, 0));

    // The one disabled recipe (kit::disabledInk): each text role's own resting ink at 40%.
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText,
                            QPalette::HighlightedText, QPalette::PlaceholderText}) {
        palette.setColor(QPalette::Disabled, role,
                         disabledInk(palette.color(QPalette::Active, role)));
    }
    palette.setColor(QPalette::Disabled, QPalette::Highlight, color(Color::Surface));
    palette.setColor(QPalette::Disabled, QPalette::Base, color(Color::Background));
    palette.setColor(QPalette::Disabled, QPalette::Button, color(Color::Surface));
    return palette;
}

QString kinetikStyleSheet() {
    // Every objectName below already existed before this slice and is reproduced verbatim: they are
    // test contracts, and this sheet only restates their appearance in token terms.
    static const auto kTemplate = QStringLiteral(R"(
QMainWindow, QMenuBar {
    background: {color.Background};
    color: {color.Foreground};
}
QMenuBar {
    border-bottom: {border.Hairline}px solid {color.Border};
    /* task C1, item C3 (owner: "menus properly padded, not reaching the top edge"): the bar's own
       Spacing::S (8px) top/bottom padding is what keeps the row's items off the client area's top
       edge; symmetric top/bottom padding is also what centers the items vertically in the row. */
    padding: {space.S}px {space.XS}px;
}
QMenuBar::item {
    /* Spacing::MenuItemX (10px) horizontal item padding, per item, per C3. Vertical padding stays
       at the item's own resting XXS: the bar's own padding above already supplies the 8px of
       vertical breathing room the owner asked for. */
    padding: {space.XXS}px {space.MenuItemX}px;
    border-radius: {radius.Small}px;
    background: transparent;
}
QMenuBar::item:selected {
    background: {color.Accent};
    color: {color.OnAccent};
}
QMenuBar::item:disabled {
    color: {color.DisabledInk};
}
/* task F1, item F5: there is deliberately NO QMenu rule in this sheet at all. A menu's frame AND
   its rows are painted by kit::AltUnderlineProxyStyle, because a stylesheet cannot reach a menu
   item's shortcut column (QSS has no selector for it), cannot tint a vendored currentColor SVG
   into a submenu arrow, and cannot reserve an icon column the painter honours. Verified
   empirically: the instant ANY QMenu rule with a box exists, QStyleSheetStyle draws the whole
   item itself and the application style's drawControl(CE_MenuItem) is never called -- so the two
   cannot be mixed, and the proxy owns the lot. */
/* A panel's border is painted once, by kit::KPanelFrame on top of its children (Accent while the
   panel is active); the sheet only gives the panel its background. */
QFrame#editorArea {
    background: {color.Background};
    /* Transparent: it keeps the panel's one-hairline content inset, while KPanelFrame paints the
       visible border on top of the children. */
    border: {border.Hairline}px solid transparent;
    border-radius: {radius.Panel}px;
}
QWidget#editorHeader {
    background: {color.Surface};
    border-bottom: {border.Hairline}px solid {color.Border};
}
QLabel#unavailableEditorPlaceholder {
    color: {color.Faint};
}
/* Task NODES-1: the node editor's header-hosted menu buttons (Add/View/Select/Node, and the "..."
   overflow button that replaces all four once the header gets too narrow) read as bare text
   labels, the same flat/transparent-until-hovered treatment QMenuBar::item already gets above.
   Their state rules are generated below from the MenuItem button recipe. */
QToolButton[headerMenuButton="true"] {
    border: {border.Hairline}px solid transparent;
    border-radius: {radius.Small}px;
    padding: {space.XXS}px {space.MenuItemX}px;
}
QToolButton[headerMenuButton="true"]::menu-indicator {
    image: none;
    width: 0px;
}
QWidget#readOnlyPlaceholderPage {
    background: {color.Background};
}
QLabel#readOnlyPlaceholderHeading {
    color: {color.Foreground};
}
QLabel#readOnlyPlaceholderFileName {
    color: {color.Accent};
}
QLabel#readOnlyPlaceholderBody {
    color: {color.Muted};
}
QToolButton {
    border: {border.Hairline}px solid transparent;
    border-radius: {radius.Small}px;
    padding: {space.XXS}px {space.XS}px;
}
QPushButton {
    border: {border.Hairline}px solid {color.Border};
    border-radius: {radius.Small}px;
    padding: {space.XXS}px {space.M}px;
    min-height: {size.Control}px;
}
QSplitter::handle {
    background: {color.Background};
}
QSplitter::handle:hover {
    background: {color.SurfaceRaised};
}
QStatusBar {
    background: {color.Surface};
    color: {color.Muted};
    border-top: {border.Hairline}px solid {color.Border};
}
QStatusBar::item {
    border: none;
}
QToolTip {
    background: {color.SurfaceRaised};
    color: {color.Foreground};
    border: {border.Hairline}px solid {color.Border};
    border-radius: {radius.Small}px;
    padding: {space.XS}px {space.S}px;
}
/* Item views: rows, hover, selection, the current-row keyboard-focus inset and the tree carets
   are painted by kit::AltUnderlineProxyStyle (PE_PanelItemViewRow/Item, PE_FrameFocusRect,
   PE_IndicatorBranch), because a QSS ::item background cannot reach a tree's branch area -- that
   is what notched a selected row at its indentation. The sheet keeps only the box. */
QTreeView, QTableView, QListView {
    background: {color.Background};
    alternate-background-color: {color.Surface};
    color: {color.Foreground};
    border: none;
    selection-color: {color.Foreground};
}
QTreeView::item, QTableView::item, QListView::item {
    padding: {space.XXS}px {space.XS}px;
}
QHeaderView {
    background: {color.Surface};
    border: none;
}
QHeaderView::section {
    background: {color.Surface};
    color: {color.Muted};
    padding: {space.XXS}px {space.S}px;
    border: none;
    border-right: {border.Hairline}px solid {color.Border};
    border-bottom: {border.Hairline}px solid {color.Border};
}
QHeaderView::section:hover {
    background: {color.SurfaceRaised};
    color: {color.Foreground};
}
QHeaderView[bloomKeyboardFocus="true"]::section {
    border-bottom-color: {color.Accent};
}
/* One constant track width, so hovering never relayouts the view; the thumb rests inset inside
   the track and fills it under the pointer. */
QScrollBar:vertical {
    background: transparent;
    width: {size.ScrollBarTrack}px;
    margin: 0px;
}
QScrollBar::handle:vertical {
    background: {color.BorderHover};
    border-radius: {radius.ScrollBarThumb}px;
    min-height: {space.XXL}px;
    margin: 0px {size.ScrollBarThumbInset}px;
}
QScrollBar:horizontal {
    background: transparent;
    height: {size.ScrollBarTrack}px;
    margin: 0px;
}
QScrollBar::handle:horizontal {
    background: {color.BorderHover};
    border-radius: {radius.ScrollBarThumb}px;
    min-width: {space.XXL}px;
    margin: {size.ScrollBarThumbInset}px 0px;
}
QScrollBar::handle:hover {
    background: {color.BorderActive};
    border-radius: {radius.ScrollBarThumbHover}px;
    margin: 0px;
}
QScrollBar::handle:pressed {
    background: {color.Muted};
    border-radius: {radius.ScrollBarThumbHover}px;
    margin: 0px;
}
QScrollBar::add-line, QScrollBar::sub-line {
    width: 0px;
    height: 0px;
    background: none;
    border: none;
}
QScrollBar::add-page, QScrollBar::sub-page {
    background: none;
}
)");
    // The stock buttons' states, generated from the one button recipe. Order matters: the generic
    // QToolButton rules come before the menu-strip ones, and QPushButton:default (the Primary look
    // of a dialog's default button) after the plain QPushButton rules.
    static const QString kButtons =
        buttonRules(QStringLiteral("QToolButton"), ButtonKind::Ghost) +
        buttonRules(QStringLiteral("QToolButton[headerMenuButton=\"true\"]"),
                    ButtonKind::MenuItem) +
        QStringLiteral("QToolButton[headerMenuButton=\"true\"]:open {\n") +
        buttonDeclarations(buttonColors(ButtonKind::MenuItem, State::Pressed, false)) +
        QStringLiteral("}\n") + buttonRules(QStringLiteral("QPushButton"), ButtonKind::Secondary) +
        buttonRules(QStringLiteral("QPushButton:default"), ButtonKind::Primary) + lineEditRules();
    return expandTokens(kTemplate) + kButtons;
}

void installKinetikTheme(QApplication& application) {
    installKeyboardFocusTracking(application);
    // Fusion rather than the platform style: Kinetik is one interface on Linux, macOS, and Windows,
    // and only a style that honors the application palette and stylesheet uniformly can deliver
    // that. The native style on macOS and Windows overrides large parts of both.
    //
    // The kit proxy style wraps Fusion and is part of the theme: menu rows, item-view rows, tree
    // carets, the menu shadow and the suppressed focus rectangle are all painted there, so a
    // window built under the theme looks the same in the application and in a test.
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks) -- QApplication::setStyle() owns it.
    QApplication::setStyle(new AltUnderlineProxyStyle());
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
    // Bundled faces are registered before the application font is set, so the very first widget
    // already renders in Inter rather than flashing the platform family. A face that
    // will not load produces a diagnostic and a platform fallback, never a failure to open.
    (void)registerBundledFonts();
    QApplication::setPalette(kinetikPalette());
    // The application font AND every per-class font Qt takes from the platform theme become type
    // roles (kit/typography.hpp): a platform class font outranks the application default, so
    // setting only the latter left item views, labels and stock buttons in the desktop's face.
    // Qt refills the class fonts on every style change, and platform themes re-set the
    // application font after start-up, so the roles are re-applied on each of those events too.
    applyTypeRoleFonts();
    installTypeRoleEnforcement(application);
    application.setStyleSheet(kinetikStyleSheet());
}

} // namespace bloom::ui::kit
