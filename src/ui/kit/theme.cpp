#include <bloom/ui/kit/theme.hpp>

#include <bloom/ui/kit/fonts.hpp>
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
        {QLatin1StringView("Surface"), Color::Surface},
        {QLatin1StringView("SurfaceRaised"), Color::SurfaceRaised},
        {QLatin1StringView("Field"), Color::Field},
        {QLatin1StringView("Foreground"), Color::Foreground},
        {QLatin1StringView("Muted"), Color::Muted},
        {QLatin1StringView("Faint"), Color::Faint},
        {QLatin1StringView("Border"), Color::Border},
        {QLatin1StringView("BorderHover"), Color::BorderHover},
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
        {QLatin1StringView("radius.Small"), radiusPx(Radius::Small, 0)},
        {QLatin1StringView("radius.Medium"), radiusPx(Radius::Medium, 0)},
        {QLatin1StringView("radius.Large"), radiusPx(Radius::Large, 0)},
        {QLatin1StringView("radius.XLarge"), radiusPx(Radius::XLarge, 0)},
        {QLatin1StringView("size.ControlCompact"), px(Size::ControlCompact)},
        {QLatin1StringView("size.Control"), px(Size::Control)},
        {QLatin1StringView("size.ControlRoomy"), px(Size::ControlRoomy)},
        {QLatin1StringView("size.IconSmall"), px(Size::IconSmall)},
        {QLatin1StringView("size.IconMedium"), px(Size::IconMedium)},
        {QLatin1StringView("size.IconLarge"), px(Size::IconLarge)},
        {QLatin1StringView("size.TitleBar"), px(Size::TitleBar)},
        {QLatin1StringView("size.PanelHeader"), px(Size::PanelHeader)},
        {QLatin1StringView("size.TimelineRow"), px(Size::TimelineRow)},
        {QLatin1StringView("size.ScrollBar"), px(Size::ScrollBar)},
        {QLatin1StringView("size.ScrollBarHover"), px(Size::ScrollBarHover)},
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
QMainWindow, QMenuBar, QMenu {
    background: {color.Background};
    color: {color.Foreground};
}
QMenuBar {
    border-bottom: {border.Hairline}px solid {color.Border};
    padding: {space.XXS}px {space.XS}px;
}
QMenuBar::item {
    padding: {space.XXS}px {space.S}px;
    border-radius: {radius.Small}px;
    background: transparent;
}
QMenuBar::item:selected {
    background: {color.Accent};
    color: {color.Foreground};
}
QMenuBar::item:checked {
    background: {color.SurfaceRaised};
    border-radius: {radius.Small}px;
}
QMenuBar::item:disabled {
    color: {color.DisabledInk};
}
QMenu {
    background: {color.SurfaceRaised};
    border: {border.Hairline}px solid {color.Border};
    border-radius: {radius.Medium}px;
    padding: {space.XXS}px;
}
QMenu::item {
    padding: {space.XS}px {space.M}px;
    border-radius: {radius.Small}px;
}
QMenu::item:selected {
    background: {color.Accent};
    color: {color.Foreground};
}
QMenu::item:disabled {
    color: {color.DisabledInk};
}
QMenu::separator {
    height: {border.Hairline}px;
    background: {color.Border};
    margin: {space.XXS}px {space.XS}px;
}
QFrame#editorArea {
    background: {color.Background};
    border: {border.Hairline}px solid {color.Border};
}
QFrame#editorArea[active="true"] {
    border-color: {color.Accent};
}
QWidget#editorHeader {
    background: {color.Surface};
    border-bottom: {border.Hairline}px solid {color.Border};
    min-height: {size.PanelHeader}px;
}
QLabel#unavailableEditorPlaceholder {
    color: {color.Faint};
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
    background: {color.Field};
    color: {color.Foreground};
    border: {border.Hairline}px solid {color.Border};
    border-radius: {radius.Small}px;
    padding: {space.XXS}px {space.S}px;
    min-height: {size.ControlCompact}px;
}
QComboBox:hover {
    border-color: {color.BorderHover};
}
QComboBox:focus {
    border-color: {color.Accent};
}
QComboBox:disabled {
    color: {color.DisabledInk};
}
QComboBox::drop-down {
    border: none;
    width: {size.IconLarge}px;
}
QComboBox QAbstractItemView {
    background: {color.SurfaceRaised};
    color: {color.Foreground};
    border: {border.Hairline}px solid {color.Border};
    border-radius: {radius.Medium}px;
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
    // Fusion rather than the platform style: Kinetik is one interface on Linux, macOS, and Windows,
    // and only a style that honors the application palette and stylesheet uniformly can deliver
    // that. The native style on macOS and Windows overrides large parts of both.
    if (auto* style = QStyleFactory::create(QStringLiteral("Fusion")); style != nullptr) {
        QApplication::setStyle(style);
    }
    // Bundled faces are registered before the application font is set, so the very first widget
    // already renders in Plus Jakarta Sans rather than flashing the platform family. A face that
    // will not load produces a diagnostic and a platform fallback, never a failure to open.
    (void)registerBundledFonts();
    QApplication::setPalette(kinetikPalette());
    QApplication::setFont(font(TypeRole::Ui));
    application.setStyleSheet(kinetikStyleSheet());
}

} // namespace bloom::ui::kit
