#include <bloom/ui/kit/mnemonic_style.hpp>

#include <bloom/ui/kit/popup_window.hpp>

#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/typography.hpp>

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMargins>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRect>
#include <QRectF>
#include <QStyleFactory>
#include <QStyleOptionMenuItem>
#include <QStyleOptionViewItem>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace bloom::ui::kit {
namespace {

constexpr auto kKeyboardFocusProperty = "bloomKeyboardFocus";
constexpr auto kRestyleProperty = "bloomFocusRestyle";

[[nodiscard]] bool restylesOnFocus(const QWidget& widget) {
    return widget.property(kRestyleProperty).toBool() ||
           qobject_cast<const QPushButton*>(&widget) != nullptr ||
           qobject_cast<const QComboBox*>(&widget) != nullptr ||
           qobject_cast<const QLineEdit*>(&widget) != nullptr ||
           qobject_cast<const QHeaderView*>(&widget) != nullptr;
}

void applyKeyboardFocus(QWidget& widget, const bool keyboard) {
    if (widget.property(kKeyboardFocusProperty).toBool() == keyboard &&
        widget.property(kKeyboardFocusProperty).isValid()) {
        widget.update();
        return;
    }
    widget.setProperty(kKeyboardFocusProperty, keyboard);
    if (restylesOnFocus(widget)) {
        widget.style()->unpolish(&widget);
        widget.style()->polish(&widget);
    }
    widget.update();
    // An item view paints its current-item focus inset in its viewport.
    if (auto* view = qobject_cast<QAbstractScrollArea*>(&widget); view && view->viewport())
        view->viewport()->update();
}

class KeyboardFocusTracker final : public QObject {
  public:
    explicit KeyboardFocusTracker(QObject* parent) : QObject(parent) {}

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::FocusIn && event->type() != QEvent::FocusOut) {
            return QObject::eventFilter(watched, event);
        }
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget == nullptr) {
            return QObject::eventFilter(watched, event);
        }
        const auto reason = static_cast<QFocusEvent*>(event)->reason();
        applyKeyboardFocus(*widget,
                           event->type() == QEvent::FocusIn &&
                               (reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason));
        return QObject::eventFilter(watched, event);
    }
};

void installKeyboardFocusTracker(QApplication& application) {
    if (application.property(kKeyboardFocusProperty).toBool()) {
        return;
    }
    auto* tracker = new KeyboardFocusTracker(&application);
    application.installEventFilter(tracker);
    application.setProperty(kKeyboardFocusProperty, true);
}

// The menu's shadow gutter on its right and bottom edges: the Popup elevation's blur, plus its
// vertical offset below.
[[nodiscard]] QMargins menuShadowGutter() {
    // No gutter where transparency is not guaranteed: an opaque gutter is a dark band under the
    // last row (owner, 2026-10-29: "it looks like a chin").
    if (!popupTranslucencySupported())
        return {};
    const Shadow popup = shadow(Elevation::Popup);
    return {0, 0, popup.blurRadius / 2 + popup.offsetX, popup.blurRadius / 2 + popup.offsetY};
}

// Wraps a plain-text tooltip that would run wider than Size::TooltipMaxWidth.
class TooltipWrapper final : public QObject {
  public:
    using QObject::QObject;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Show) {
            if (auto* tip = qobject_cast<QLabel*>(watched)) {
                const int cap = px(Size::TooltipMaxWidth);
                if (!tip->wordWrap() && tip->sizeHint().width() > cap) {
                    tip->setWordWrap(true);
                    tip->resize(cap, tip->heightForWidth(cap));
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

QObject* tooltipWrapper() {
    static auto* wrapper = new TooltipWrapper(QCoreApplication::instance());
    return wrapper;
}

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

// The current row of a keyboard-focused item view: one Accent hairline inset inside the row, the
// list form of "the control's own border turns Accent".
void paintItemFocusInset(QPainter& painter, const QRectF& row) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    applyHairlinePen(painter, color(Color::Accent));
    painter.setBrush(Qt::NoBrush);
    const QRectF box = snappedToDevicePixels(painter, row);
    const qreal inset = painter.pen().widthF() / 2.0;
    painter.drawRect(box.adjusted(inset, inset, -inset, -inset));
    painter.restore();
}

// Item-view rows and cells: alternating rows one ladder step apart, the hover step, and the one
// selected-row recipe (paintSelectedRow) -- painted for the branch area and every cell alike, so a
// selected tree row has no notch at its indentation.
void paintItemViewBackground(const QStyle::PrimitiveElement element, const QStyleOption& option,
                             QPainter& painter, const QAbstractItemView& view) {
    const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(&option);
    const QRectF rect(option.rect);
    const bool selected = (option.state & QStyle::State_Selected) != 0;
    const bool alternate =
        item != nullptr && item->features.testFlag(QStyleOptionViewItem::Alternate);
    const Color base = alternate ? Color::Surface : Color::Background;
    if (selected) {
        // The leading Accent edge belongs to the row's first painted cell only.
        const bool leading = element == QStyle::PE_PanelItemViewRow && rect.left() <= 0.0;
        paintSelectedRow(painter, rect, base, leading);
        return;
    }
    if (element == QStyle::PE_PanelItemViewRow) {
        if (alternate)
            painter.fillRect(rect, color(Color::Surface));
        return;
    }
    if ((option.state & QStyle::State_MouseOver) != 0 && view.isEnabled() &&
        (option.state & QStyle::State_Enabled) != 0) {
        painter.fillRect(rect, color(surfaceStep(base, 1)));
    } else if (item != nullptr && item->backgroundBrush.style() != Qt::NoBrush) {
        painter.fillRect(rect, item->backgroundBrush);
    }
}

// A tree's expand/collapse affordance: the same Phosphor carets the Timeline discloses with.
void paintBranchIndicator(const QStyleOption& option, QPainter& painter) {
    if ((option.state & QStyle::State_Children) == 0)
        return;
    const bool open = (option.state & QStyle::State_Open) != 0;
    const qreal ratio = painter.device()->devicePixelRatioF();
    const QPixmap glyph = iconPixmap(
        open ? IconId::CaretDown : IconId::CaretRight, IconRole::Chrome, Color::Muted,
        (option.state & QStyle::State_MouseOver) != 0 ? kit::State::Hover : kit::State::Normal,
        ratio);
    const QSizeF extent = glyph.deviceIndependentSize();
    const QRectF rect(option.rect);
    painter.drawPixmap(
        QPointF(std::round((rect.center().x() - extent.width() / 2.0) * ratio) / ratio,
                std::round((rect.center().y() - extent.height() / 2.0) * ratio) / ratio),
        glyph);
}

// The menu frame's corner: Radius::Small where the menu is translucent, square where it is not
// (its corner pixels would be opaque).
qreal menuFrameCorner(const QWidget* menu, const QRectF& frame) {
    if (menu != nullptr && !menu->testAttribute(Qt::WA_TranslucentBackground))
        return 0.0;
    return radiusPx(Radius::Small, static_cast<int>(std::min(frame.width(), frame.height())));
}

// The menu's Accent hover bar: full width, clipped by an antialiased path to the frame's rounded
// inner edge, so a first or last row meets the frame's corner without squaring it off. `widget` is
// the menu; a row painted on a child of it (a flowed submenu's body) maps the frame across.
void paintMenuHoverBar(QPainter& painter, const QRect& row, const QWidget* widget) {
    const auto* menu = qobject_cast<const QMenu*>(widget);
    if (menu == nullptr) {
        painter.fillRect(row, color(Color::Accent));
        return;
    }
    const QMargins gutter = menu->contentsMargins();
    QRectF frame = QRectF(menu->rect())
                       .adjusted(gutter.left(), gutter.top(), -gutter.right(), -gutter.bottom());
    if (const auto* target = dynamic_cast<const QWidget*>(painter.device());
        target != nullptr && target != menu && menu->isAncestorOf(target))
        frame.translate(QPointF(target->mapFrom(menu, QPoint(0, 0))));
    const qreal border = std::max(1.0, std::round(kHairlineWidth));
    const QRectF inner = frame.adjusted(border, border, -border, -border);
    const qreal corner = std::max(0.0, menuFrameCorner(menu, frame) - border);
    QPainterPath clip;
    clip.addRoundedRect(inner, corner, corner);
    QPainterPath bar;
    bar.addRect(QRectF(row));
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color(Color::Accent));
    painter.drawPath(bar.intersected(clip));
    painter.restore();
}

} // namespace

bool showMnemonicUnderline(const Qt::KeyboardModifiers modifiers) noexcept {
    return modifiers.testFlag(Qt::AltModifier);
}

void installKeyboardFocusTracking(QApplication& application) {
    installKeyboardFocusTracker(application);
}

void ensureKeyboardFocusTracking(QWidget& widget) {
    auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (application != nullptr) {
        installKeyboardFocusTracker(*application);
    }
    Q_UNUSED(widget)
}

void restyleOnKeyboardFocus(QWidget& widget) {
    ensureKeyboardFocusTracking(widget);
    widget.setProperty(kRestyleProperty, true);
}

bool hasKeyboardFocus(const QWidget& widget) noexcept {
    return widget.hasFocus() && widget.property(kKeyboardFocusProperty).toBool();
}

int menuIconColumnWidth() { return px(Size::IconMedium); }

int menuRowHeight(const QFont& font) {
    return std::max(QFontMetrics(font).height(), menuIconColumnWidth()) +
           px(Spacing::MenuItemY) * 2;
}

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
        // The frame below is drawn with rounded corners and casts the Popup shadow; without this
        // the corner pixels and the shadow gutter would be an opaque plate. Where the platform does
        // not guarantee transparency the menu is a square, gutterless plate instead.
        if (popupTranslucencySupported())
            widget->setAttribute(Qt::WA_TranslucentBackground, true);
        // The shadow falls right and below, in a transparent gutter the menu lays its rows out
        // inside of (QMenu honours contents margins). Top and left stay flush, so a menu still
        // opens exactly at its anchor.
        const QMargins gutter = menuShadowGutter();
        widget->setContentsMargins(0, 0, gutter.right(), gutter.bottom());
        // The UI type role, set on the widget itself. Qt resolves a menu's default font from the
        // platform theme's per-class MenuFont, which outranks the application-wide default, and
        // that entry is re-applied whenever the application style changes -- so the only place the
        // role reliably survives is here, where every menu passes exactly once.
        widget->setFont(font(TypeRole::Ui));
    }
    if (qobject_cast<QMenuBar*>(widget) || qobject_cast<QHeaderView*>(widget))
        widget->setFont(font(TypeRole::Ui));
    if (widget->inherits("QTipLabel")) {
        // The platform theme's per-class tooltip font would otherwise win; and a plain-text tip
        // never wraps in Qt, so the tip wraps itself once it would pass TooltipMaxWidth.
        widget->setFont(font(TypeRole::Ui));
        // The tip's frame is rounded (Radius::Small); its corners stay unpainted.
        widget->setAttribute(Qt::WA_TranslucentBackground, true);
        widget->installEventFilter(tooltipWrapper());
    }
    QProxyStyle::polish(widget);
    // Every other widget that never named a font and still resolves a platform face takes its
    // role here, the one place every widget passes.
    enforceTypeRole(*widget);
}

void AltUnderlineProxyStyle::polish(QApplication* application) {
    QProxyStyle::polish(application);
    applyTypeRoleFonts();
}

void AltUnderlineProxyStyle::drawPrimitive(const PrimitiveElement element,
                                           const QStyleOption* option, QPainter* painter,
                                           const QWidget* widget) const {
    const auto* view = qobject_cast<const QAbstractItemView*>(widget);
    if (view == nullptr && element == QStyle::PE_FrameFocusRect && painter != nullptr) {
        // A tree's full-row focus rectangle arrives without its widget; the painter's device is
        // the view's viewport.
        if (const auto* viewport = dynamic_cast<const QWidget*>(painter->device()))
            view = qobject_cast<const QAbstractItemView*>(viewport->parentWidget());
    }
    if (element == QStyle::PE_FrameFocusRect) {
        // Never Qt's dotted rectangle. The one exception is an item view's current row under
        // KEYBOARD focus: an Accent inset, because a list's only focus affordance is the row.
        if (view != nullptr && option != nullptr && painter != nullptr && hasKeyboardFocus(*view)) {
            paintItemFocusInset(*painter, QRectF(option->rect));
        }
        return;
    }
    if (view != nullptr && option != nullptr && painter != nullptr &&
        (element == QStyle::PE_PanelItemViewRow || element == QStyle::PE_PanelItemViewItem)) {
        paintItemViewBackground(element, *option, *painter, *view);
        return;
    }
    if (element == QStyle::PE_IndicatorBranch && option != nullptr && painter != nullptr) {
        paintBranchIndicator(*option, *painter);
        return;
    }
    if (element == QStyle::PE_PanelMenu && option != nullptr && painter != nullptr) {
        // The menu frame, task F1 item F5: SurfaceRaised, one Border hairline, Radius::Small --
        // the same shape and ladder rung the dropdown popup uses, so a menu and a dropdown are
        // visibly the same kind of surface -- casting the same Popup elevation.
        const QMargins gutter = widget != nullptr ? widget->contentsMargins() : QMargins{};
        const QRectF frame =
            QRectF(option->rect)
                .adjusted(gutter.left(), gutter.top(), -gutter.right(), -gutter.bottom());
        // The casting shape is pulled in so the soft edge lands inside the right/bottom gutter
        // and never shows a cut where the gutter ends.
        const qreal blur = shadow(Elevation::Popup).blurRadius;
        if (!gutter.isNull())
            drawElevationShadow(*painter, frame.adjusted(blur, blur, -blur / 2.0, -blur / 2.0),
                                Radius::Small, Elevation::Popup);
        fillRoundedSurface(*painter, frame, color(Color::SurfaceRaised), color(Color::Border),
                           menuFrameCorner(widget, frame));
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
        // None: the first and last rows meet the frame, exactly as a dropdown's do, so nothing is
        // left under the last row. Their hover bars are clipped to the frame's rounded corners.
        return 0;
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
        paintMenuHoverBar(*painter, rect, widget);
    }

    // Qualified: QStyle already has a nested `State`, and this class IS a QStyle.
    using kit::State;
    const State inkState =
        !enabled ? State::Disabled : (selected ? State::Selected : State::Normal);
    const QColor labelInk =
        selected ? color(Color::OnAccent) : inkForState(Color::Foreground, inkState);
    // The shortcut is deliberately the quietest thing in the row -- and on the Accent hover bar
    // it is OnAccent at the secondary-ink opacity, because Faint on Accent is unreadable.
    const QColor shortcutInk =
        selected ? withOpacity(color(Color::OnAccent), kSecondaryInkOnAccentOpacity)
                 : inkForState(Color::Faint, enabled ? State::Normal : State::Disabled);
    const qreal ratio = painter->device()->devicePixelRatioF();
    const auto drawGlyph = [&](const QRect& box, const QPixmap& pixmap) {
        const QSizeF extent = pixmap.deviceIndependentSize();
        painter->drawPixmap(
            QPointF(std::round((box.x() + (box.width() - extent.width()) / 2.0) * ratio) / ratio,
                    std::round((box.y() + (box.height() - extent.height()) / 2.0) * ratio) / ratio),
            pixmap);
    };

    const int iconColumn = menuIconColumnWidth();
    const QRect iconRect(rect.left() + px(Spacing::MenuItemX), rect.center().y() - iconColumn / 2,
                         iconColumn, iconColumn);
    const bool checked = item->checkType != QStyleOptionMenuItem::NotCheckable && item->checked;
    if (!item->icon.isNull()) {
        const QIcon::Mode mode =
            !enabled ? QIcon::Disabled : (selected ? QIcon::Selected : QIcon::Normal);
        drawGlyph(iconRect, item->icon.pixmap(QSize(iconColumn, iconColumn), ratio, mode,
                                              checked ? QIcon::On : QIcon::Off));
        if (checked) {
            // A checkable action that carries its own icon shows its checked state as a hairline
            // frame around that icon (and the icon's Fill weight), never by hiding the icon.
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            fillRoundedSurface(*painter,
                               QRectF(iconRect).adjusted(-px(Spacing::XXS), -px(Spacing::XXS),
                                                         px(Spacing::XXS), px(Spacing::XXS)),
                               {}, labelInk, Radius::Small);
            painter->restore();
        }
    } else if (checked) {
        // A checked item with no icon of its own marks itself in the SAME reserved column, so a
        // check mark never shifts a label either.
        drawGlyph(iconRect, iconPixmap(IconId::Check, Size::IconMedium, labelInk, ratio,
                                       iconWeight(IconRole::Chrome)));
    }

    int right = rect.right() - px(Spacing::MenuItemX);
    if (item->menuItemType == QStyleOptionMenuItem::SubMenu) {
        const int arrow = menuArrowWidth();
        const QRect arrowRect(right - arrow, rect.center().y() - arrow / 2, arrow, arrow);
        drawGlyph(arrowRect, iconPixmap(IconId::CaretRight, Size::IconSmall, labelInk, ratio,
                                        iconWeight(IconRole::Chrome)));
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
