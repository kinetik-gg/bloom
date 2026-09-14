#include <bloom/ui/kit/mnemonic_style.hpp>

#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QFontMetrics>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QRect>
#include <QRectF>
#include <QStyleFactory>
#include <QStyleOptionMenuItem>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace bloom::ui::kit {
namespace {

// The gap between the icon column and the label, and between the label and the shortcut column.
[[nodiscard]] int menuColumnGap() { return px(Spacing::S); }
[[nodiscard]] int menuShortcutGap() { return px(Spacing::L); }

// The submenu caret's own column: the same box the icon column uses.
[[nodiscard]] int menuArrowWidth() { return px(Size::IconSmall); }

// A menu item's text is "Label\tShortcut" in Qt's own convention.
struct MenuText {
    QString label;
    QString shortcut;
};

[[nodiscard]] MenuText splitMenuText(const QString& text) {
    const qsizetype tab = text.indexOf(QLatin1Char('\t'));
    if (tab < 0) {
        return {text, {}};
    }
    return {text.left(tab), text.mid(tab + 1)};
}

} // namespace

bool showMnemonicUnderline(const Qt::KeyboardModifiers modifiers) noexcept {
    return modifiers.testFlag(Qt::AltModifier);
}

int menuIconColumnWidth() { return px(Size::IconSmall); }

int menuItemTextOffset() {
    return px(Spacing::MenuItemX) + menuIconColumnWidth() + menuColumnGap();
}

AltUnderlineProxyStyle::AltUnderlineProxyStyle()
    : AltUnderlineProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

AltUnderlineProxyStyle::AltUnderlineProxyStyle(QStyle* baseStyle) : QProxyStyle(baseStyle) {}

int AltUnderlineProxyStyle::styleHint(const StyleHint hint, const QStyleOption* option,
                                      const QWidget* widget, QStyleHintReturn* returnData) const {
    if (hint == QStyle::SH_UnderlineShortcut) {
        return showMnemonicUnderline(QGuiApplication::keyboardModifiers()) ? 1 : 0;
    }
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

void AltUnderlineProxyStyle::polish(QWidget* widget) {
    if (qobject_cast<QMenu*>(widget) != nullptr) {
        // The frame below is drawn with rounded corners; without this the four corner pixels would
        // be an opaque plate the rounding cannot remove.
        widget->setAttribute(Qt::WA_TranslucentBackground, true);
        // The UI type role, set on the widget itself. Qt resolves a menu's default font from the
        // platform theme's per-class MenuFont, which outranks the application-wide default, and
        // that entry is re-applied whenever the application style changes -- so the only place the
        // role reliably survives is here, where every menu passes exactly once.
        widget->setFont(font(TypeRole::Ui));
    }
    QProxyStyle::polish(widget);
}

void AltUnderlineProxyStyle::drawPrimitive(const PrimitiveElement element,
                                           const QStyleOption* option, QPainter* painter,
                                           const QWidget* widget) const {
    if (element == QStyle::PE_FrameFocusRect) {
        return;
    }
    if (element == QStyle::PE_PanelMenu && option != nullptr && painter != nullptr) {
        // The menu frame, task F1 item F5: SurfaceRaised, one Border hairline, Radius::Small --
        // the same shape and ladder rung the dropdown popup uses, so a menu and a dropdown are
        // visibly the same kind of surface.
        fillRoundedSurface(*painter, QRectF(option->rect), color(Color::SurfaceRaised),
                           color(Color::Border), Radius::Small);
        return;
    }
    if (element == QStyle::PE_FrameMenu) {
        // PE_PanelMenu above already stroked the one border this frame gets. Qt draws this second
        // primitive on top of it, which would be the double outline item F2 forbids.
        return;
    }
    QProxyStyle::drawPrimitive(element, option, painter, widget);
}

int AltUnderlineProxyStyle::pixelMetric(const PixelMetric metric, const QStyleOption* option,
                                        const QWidget* widget) const {
    switch (metric) {
    case QStyle::PM_MenuPanelWidth:
        return std::max(1, static_cast<int>(std::lround(kHairlineWidth)));
    case QStyle::PM_MenuVMargin:
        return px(Spacing::XS);
    case QStyle::PM_MenuHMargin:
        // The rows run edge to edge inside the frame, so the accent hover bar is full width.
        return 0;
    default:
        break;
    }
    return QProxyStyle::pixelMetric(metric, option, widget);
}

void AltUnderlineProxyStyle::drawControl(const ControlElement element, const QStyleOption* option,
                                         QPainter* painter, const QWidget* widget) const {
    const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
    if (element != QStyle::CE_MenuItem || item == nullptr || painter == nullptr) {
        QProxyStyle::drawControl(element, option, painter, widget);
        return;
    }

    const QRect rect = item->rect;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, false);

    if (item->menuItemType == QStyleOptionMenuItem::Separator) {
        // A hairline in the Border ink, inset so it reads as a divider between groups rather than
        // as an edge of the frame.
        QRect line = rect;
        line.setHeight(std::max(1, static_cast<int>(std::lround(kHairlineWidth))));
        line.moveTop(rect.center().y());
        line.adjust(px(Spacing::MenuItemX), 0, -px(Spacing::MenuItemX), 0);
        painter->fillRect(line, color(Color::Border));
        painter->restore();
        return;
    }

    const bool enabled = (item->state & QStyle::State_Enabled) != 0;
    const bool selected = (item->state & QStyle::State_Selected) != 0 && enabled;

    // The hover bar is the dropdown's: a full-width Accent rectangle, never a rounded pill and
    // never inset from the frame's own edges.
    if (selected) {
        painter->fillRect(rect, color(Color::Accent));
    }

    // Qualified: QStyle already has a nested `State`, and this class IS a QStyle.
    using kit::State;
    const State inkState =
        !enabled ? State::Disabled : (selected ? State::Selected : State::Normal);
    const QColor labelInk = inkForState(Color::Foreground, inkState);
    // The shortcut is deliberately the quietest thing in the row.
    const QColor shortcutInk = inkForState(Color::Faint, enabled ? State::Normal : State::Disabled);
    const qreal ratio = painter->device()->devicePixelRatio();

    const int iconColumn = menuIconColumnWidth();
    const QRect iconRect(rect.left() + px(Spacing::MenuItemX), rect.center().y() - iconColumn / 2,
                         iconColumn, iconColumn);
    if (!item->icon.isNull()) {
        painter->drawPixmap(iconRect, item->icon.pixmap(QSize(iconColumn, iconColumn),
                                                        enabled ? QIcon::Normal : QIcon::Disabled));
    } else if (item->checkType != QStyleOptionMenuItem::NotCheckable && item->checked) {
        // A checked item with no icon of its own marks itself in the SAME reserved column, so a
        // check mark never shifts a label either.
        painter->drawPixmap(iconRect, iconPixmap(IconId::Check, Size::IconSmall, labelInk, ratio,
                                                 iconWeight(IconRole::Chrome)));
    }

    int right = rect.right() - px(Spacing::MenuItemX);
    if (item->menuItemType == QStyleOptionMenuItem::SubMenu) {
        const int arrow = menuArrowWidth();
        const QRect arrowRect(right - arrow, rect.center().y() - arrow / 2, arrow, arrow);
        painter->drawPixmap(arrowRect, iconPixmap(IconId::CaretRight, Size::IconSmall, labelInk,
                                                  ratio, iconWeight(IconRole::Chrome)));
        right = arrowRect.left() - menuColumnGap();
    }

    painter->setFont(item->font);
    const MenuText text = splitMenuText(item->text);
    if (!text.shortcut.isEmpty()) {
        const QFontMetrics metrics(item->font);
        const int width = metrics.horizontalAdvance(text.shortcut);
        const QRect shortcutRect(right - width, rect.top(), width, rect.height());
        painter->setPen(shortcutInk);
        painter->drawText(shortcutRect, Qt::AlignVCenter | Qt::AlignRight, text.shortcut);
        right = shortcutRect.left() - menuShortcutGap();
    }

    const int textLeft = rect.left() + menuItemTextOffset();
    const QRect labelRect(textLeft, rect.top(), std::max(0, right - textLeft), rect.height());
    painter->setPen(labelInk);
    // Mnemonics resolve through the same style hint this class already owns, so "&File" underlines
    // only while Alt is held here exactly as it does in the menu bar.
    int textFlags = static_cast<int>(Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine);
    if (styleHint(QStyle::SH_UnderlineShortcut, item, widget) == 0) {
        textFlags |= static_cast<int>(Qt::TextHideMnemonic);
    }
    painter->drawText(labelRect, textFlags, text.label);
    painter->restore();
}

QSize AltUnderlineProxyStyle::sizeFromContents(const ContentsType type, const QStyleOption* option,
                                               const QSize& size, const QWidget* widget) const {
    const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
    if (type != QStyle::CT_MenuItem || item == nullptr) {
        return QProxyStyle::sizeFromContents(type, option, size, widget);
    }
    if (item->menuItemType == QStyleOptionMenuItem::Separator) {
        return {std::max(size.width(), px(Size::MenuMinWidth)), px(Spacing::MenuItemY) * 2};
    }

    const QFontMetrics metrics(item->font);
    const MenuText text = splitMenuText(item->text);
    int width =
        menuItemTextOffset() + metrics.horizontalAdvance(text.label) + px(Spacing::MenuItemX);
    if (!text.shortcut.isEmpty()) {
        width += menuShortcutGap() + metrics.horizontalAdvance(text.shortcut);
    }
    if (item->menuItemType == QStyleOptionMenuItem::SubMenu) {
        width += menuColumnGap() + menuArrowWidth();
    }
    const int height =
        std::max(metrics.height(), menuIconColumnWidth()) + px(Spacing::MenuItemY) * 2;
    // Every row claims at least Size::MenuMinWidth (task S1, item 3). A menu's width is the widest
    // row it holds, so this is what stops a short menu -- "Fit", "100%", "Cut" -- from collapsing
    // to a sliver the pointer has to aim at; it is applied to the row rather than to the popup so
    // the rows still run edge to edge inside the frame and their hover bars stay full width.
    return {std::max({width, size.width(), px(Size::MenuMinWidth)}), height};
}

} // namespace bloom::ui::kit
