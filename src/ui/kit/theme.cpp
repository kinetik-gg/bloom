#include <bloom/ui/kit/theme.hpp>

#include <bloom/ui/kit/fonts.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QColor>
#include <QLatin1StringView>
#include <QStringList>
#include <QStyleFactory>

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
        {QLatin1StringView("ControlSurface"), Color::ControlSurface},
        {QLatin1StringView("Foreground"), Color::Foreground},
        {QLatin1StringView("Muted"), Color::Muted},
        {QLatin1StringView("Faint"), Color::Faint},
        {QLatin1StringView("Border"), Color::Border},
        {QLatin1StringView("BorderHover"), Color::BorderHover},
        {QLatin1StringView("BorderActive"), Color::BorderActive},
        {QLatin1StringView("Accent"), Color::Accent},
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
        {QLatin1StringView("size.ControlCompact"), px(Size::ControlCompact)},
        {QLatin1StringView("size.Control"), px(Size::Control)},
        {QLatin1StringView("size.ControlRoomy"), px(Size::ControlRoomy)},
        {QLatin1StringView("size.IconSmall"), px(Size::IconSmall)},
        {QLatin1StringView("size.IconMedium"), px(Size::IconMedium)},
        {QLatin1StringView("size.IconLarge"), px(Size::IconLarge)},
        {QLatin1StringView("size.TitleBar"), px(Size::TitleBar)},
        {QLatin1StringView("size.PanelHeader"), px(Size::PanelHeader)},
        {QLatin1StringView("size.EditorHeader"), px(Size::EditorHeader)},
        {QLatin1StringView("size.TimelineRow"), px(Size::TimelineRow)},
        {QLatin1StringView("size.ScrollBar"), px(Size::ScrollBar)},
        {QLatin1StringView("size.ScrollBarHover"), px(Size::ScrollBarHover)},
        {QLatin1StringView("size.MenuMinWidth"), px(Size::MenuMinWidth)},
        // A scrollbar thumb is a pill: Radius::Full against the scrollbar's own extent.
        {QLatin1StringView("radius.ScrollBarThumb"), radiusPx(Radius::Full, px(Size::ScrollBar))},
        {QLatin1StringView("radius.ScrollBarThumbHover"),
         radiusPx(Radius::Full, px(Size::ScrollBarHover))},
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

// Disabled ink is the normal ink at kDisabledOpacity -- a real alpha, not a separate grey, so a
// disabled control fades against whatever surface it happens to sit on.
[[nodiscard]] QColor disabledInk() {
    return withOpacity(color(Color::Foreground), kDisabledOpacity);
}

} // namespace

QString expandTokens(const QString& templateText) {
    QString text = templateText;
    for (const auto& [name, token] : colorPlaceholders()) {
        text.replace(QStringLiteral("{color.%1}").arg(name), hex(token));
    }
    text.replace(QStringLiteral("{color.DisabledInk}"), rgba(disabledInk()));
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

    // Disabled ink is the normal ink at 40%, in every text role.
    const QColor disabled = disabledInk();
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
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
    color: {color.Foreground};
}
QMenuBar::item:disabled {
    color: {color.DisabledInk};
}
QWidget#kinetikTitleBar {
    background: {color.Surface};
    border-bottom: {border.Hairline}px solid {color.Border};
}
QWidget#kinetikTitleBar QMenuBar {
    background: {color.Surface};
    border-bottom: none;
}
QLabel#titleBarTitleLabel {
    color: {color.Foreground};
}
/* task F1, item F5: there is deliberately NO QMenu rule in this sheet at all. A menu's frame AND
   its rows are painted by kit::AltUnderlineProxyStyle, because a stylesheet cannot reach a menu
   item's shortcut column (QSS has no selector for it), cannot tint a vendored currentColor SVG
   into a submenu arrow, and cannot reserve an icon column the painter honours. Verified
   empirically: the instant ANY QMenu rule with a box exists, QStyleSheetStyle draws the whole
   item itself and the application style's drawControl(CE_MenuItem) is never called -- so the two
   cannot be mixed, and the proxy owns the lot. */
QFrame#editorArea {
    background: {color.Background};
    border: {border.Hairline}px solid {color.Border};
    border-radius: {radius.Panel}px;
}
QFrame#editorArea[active="true"] {
    border-color: {color.BorderActive};
}
QWidget#editorHeader {
    background: {color.Surface};
    border-bottom: {border.Hairline}px solid {color.Border};
    min-height: {size.EditorHeader}px;
}
QLabel#unavailableEditorPlaceholder {
    color: {color.Faint};
}
QToolButton#maximizeAreaButton {
    background: {color.ControlSurface};
    border: {border.Hairline}px solid {color.Border};
    border-radius: {radius.Small}px;
}
QToolButton#maximizeAreaButton:hover {
    border-color: {color.BorderHover};
}
/* Task NODES-1: the node editor's header-hosted menu buttons (Add/View/Select/Node, and the "..."
   overflow button that replaces all four once the header gets too narrow -- node_editor_menus.cpp's
   NodeHeaderMenuBar) read as bare text labels, the same flat/transparent-until-hovered treatment
   QMenuBar::item already gets above, rather than the bordered ControlSurface square every OTHER
   header button (maximizeAreaButton) uses -- there are up to five of them in a row here, and that
   many bordered boxes would read as a toolbar, not a menu strip. Selected via a dynamic property
   rather than an objectName because every instance is built the same way, from the same class,
   the same way QMenuBar::item is a type selector rather than one objectName per bar. */
QToolButton[headerMenuButton="true"] {
    background: transparent;
    border: none;
    border-radius: {radius.Small}px;
    padding: {space.XXS}px {space.MenuItemX}px;
    color: {color.Foreground};
}
QToolButton[headerMenuButton="true"]::menu-indicator {
    image: none;
    width: 0px;
}
QToolButton[headerMenuButton="true"]:hover {
    background: {color.Accent};
}
QWidget#readOnlyPlaceholderPage {
    background: {color.Background};
}
QLabel#readOnlyPlaceholderHeading {
    color: {color.Foreground};
    font-size: {space.XL}px;
    font-weight: 600;
}
QLabel#readOnlyPlaceholderFileName {
    color: {color.Accent};
    font-weight: 600;
}
QLabel#readOnlyPlaceholderBody {
    color: {color.Muted};
}
QComboBox {
    background: {color.ControlSurface};
    color: {color.Foreground};
    border: {border.Hairline}px solid {color.Border};
    border-radius: {radius.Small}px;
    padding: {space.XXS}px {space.S}px;
    min-height: {size.ControlCompact}px;
}
QComboBox:hover {
    border-color: {color.BorderHover};
}
QComboBox[bloomKeyboardFocus="true"] {
    border-color: {color.Accent};
}
QComboBox:disabled {
    color: {color.DisabledInk};
}
QComboBox::drop-down {
    border: none;
    width: {size.IconLarge}px;
}
/* task U8 (issue 131, fix 3): the design sheet's double up/down chevron (IconId::CaretUpDown,
   the same glyph KDropdown's own closed field paints) is not reachable here as a QSS down-arrow
   image. Bloom's icon engine resolves the vendored SVGs' fill="currentColor" into a real tint by
   rewriting a copy of the markup in C++ (icons.cpp's renderIcon()) before handing QSvgRenderer a
   pixmap; Qt Style Sheets' own `image: url(...)` can only reference a static resource and never
   invokes that C++ tinting step. Verified empirically: QSvgRenderer given the raw vendored file
   as-is (currentColor unresolved) paints nothing at all, not a black glyph -- confirmed by
   rendering caret-up-down.svg through QSvgRenderer with no substitution and finding zero opaque
   pixels in the result. A second, non-vendored, pre-tinted copy of the glyph would either bake a
   literal hex value into a checked-in asset (a token drifting silently out of sync with
   tokens.cpp) or require writing a pixmap to disk at startup for QSS to reference by path, and
   both are disproportionate to a chrome polish pass. The documented closest-faithful approach:
   QComboBox keeps the Fusion style's own down-arrow indicator, which already paints in the
   installed QPalette's Foreground/Button ink, so it stays legible even though it is Fusion's
   single arrow rather than the vendored double chevron. Everything else in the design sheet
   (bordered ControlSurface field per formal amendment 1's A2, BorderHover on hover,
   Radius::Small, bordered SurfaceRaised popup at Radius::Small) is reproduced exactly,
   including on the panel-switcher QComboBox. */
QComboBox QAbstractItemView {
    background: {color.SurfaceRaised};
    color: {color.Foreground};
    border: {border.Hairline}px solid {color.Border};
    border-radius: {radius.Small}px;
    padding: {space.XXS}px;
    outline: none;
    selection-background-color: {color.Accent};
    selection-color: {color.Foreground};
}
QToolButton {
    color: {color.Muted};
    background: transparent;
    border: {border.Hairline}px solid transparent;
    border-radius: {radius.Small}px;
    padding: {space.XXS}px {space.XS}px;
    min-height: {size.ControlCompact}px;
}
QToolButton:hover {
    background: {color.SurfaceRaised};
    border-color: {color.BorderHover};
    color: {color.Foreground};
}
QToolButton:pressed {
    background: {color.Surface};
}
QToolButton:checked {
    background: {color.Accent};
    color: {color.Foreground};
}
QToolButton:disabled {
    color: {color.DisabledInk};
    border-color: transparent;
    background: transparent;
}
QPushButton {
    color: {color.Foreground};
    background: {color.SurfaceRaised};
    border: {border.Hairline}px solid {color.Border};
    border-radius: {radius.Small}px;
    padding: {space.XXS}px {space.M}px;
    min-height: {size.Control}px;
}
QPushButton:hover {
    background: {color.Field};
    border-color: {color.BorderHover};
}
QPushButton:pressed {
    background: {color.Surface};
}
QPushButton:disabled {
    color: {color.DisabledInk};
}
QSplitter::handle {
    background: {color.Background};
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
    padding: {space.XXS}px {space.XS}px;
}
QTreeView, QTableView, QListView {
    background: {color.Background};
    alternate-background-color: {color.Surface};
    color: {color.Foreground};
    border: none;
    outline: none;
    selection-background-color: {color.Accent};
    selection-color: {color.Foreground};
}
QTreeView::item, QTableView::item, QListView::item {
    border: none;
    padding: {space.XXS}px {space.XS}px;
}
QTreeView::item:hover, QTableView::item:hover, QListView::item:hover {
    background: {color.SurfaceRaised};
}
QTreeView::item:selected, QTableView::item:selected, QListView::item:selected {
    background: {color.Accent};
    color: {color.Foreground};
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
QScrollBar:vertical {
    background: transparent;
    width: {size.ScrollBar}px;
    margin: 0px;
}
QScrollBar:vertical:hover {
    width: {size.ScrollBarHover}px;
}
QScrollBar::handle:vertical {
    background: {color.BorderHover};
    border-radius: {radius.ScrollBarThumb}px;
    min-height: {space.XXL}px;
}
QScrollBar:horizontal {
    background: transparent;
    height: {size.ScrollBar}px;
    margin: 0px;
}
QScrollBar:horizontal:hover {
    height: {size.ScrollBarHover}px;
}
QScrollBar::handle:horizontal {
    background: {color.BorderHover};
    border-radius: {radius.ScrollBarThumb}px;
    min-width: {space.XXL}px;
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
    return expandTokens(kTemplate);
}

void installKinetikTheme(QApplication& application) {
    installKeyboardFocusTracking(application);
    // Fusion rather than the platform style: Kinetik is one interface on Linux, macOS, and Windows,
    // and only a style that honors the application palette and stylesheet uniformly can deliver
    // that. The native style on macOS and Windows overrides large parts of both.
    if (auto* style = QStyleFactory::create(QStringLiteral("Fusion")); style != nullptr) {
        QApplication::setStyle(style);
    }
    // Bundled faces are registered before the application font is set, so the very first widget
    // already renders in DejaVu Sans rather than flashing the platform family. A face that
    // will not load produces a diagnostic and a platform fallback, never a failure to open.
    (void)registerBundledFonts();
    QApplication::setPalette(kinetikPalette());
    QApplication::setFont(font(TypeRole::Ui));
    // Menus need the role named explicitly (task F1, item F5). Qt resolves a widget's default font
    // from the platform theme's per-CLASS font (QPlatformTheme::MenuFont) before the
    // application-wide default -- which is exactly the "menu font size is too big" the interface
    // showed. Setting it here is not enough on its own: changing the application style re-applies
    // the platform theme's class fonts, and apps/bloom/main.cpp installs the kit proxy style right
    // after this call. kit::AltUnderlineProxyStyle::polish() sets it on every menu for that
    // reason; this line covers a menu built before any style change.
    QApplication::setFont(font(TypeRole::Ui), "QMenu");
    application.setStyleSheet(kinetikStyleSheet());
}

} // namespace bloom::ui::kit
