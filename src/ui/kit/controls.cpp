#include <QAction>
#include <QButtonGroup>
#include <QFontMetrics>
#include <QGridLayout>
#include <QKeySequence>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>
namespace bloom::ui::kit {
KMenuButton::KMenuButton(QWidget* parent) : QToolButton(parent) {
    setFont(kit::font(TypeRole::Ui));
    setFixedHeight(px(Size::Control));
    setAutoRaise(true);
    setPopupMode(QToolButton::InstantPopup);
    setProperty("headerMenuButton", true);
    setProperty("kitControl", true);
}
QSize KMenuButton::sizeHint() const {
    auto size = QToolButton::sizeHint();
    size.setHeight(px(Size::Control));
    return size;
}
KMenuItem::KMenuItem(QWidget* parent) : QToolButton(parent) {
    setFont(kit::font(TypeRole::Ui));
    setFixedHeight(px(Size::Control));
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    setAutoRaise(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setProperty("kitControl", true);
    setProperty("menuItem", true);
}
QSize KMenuItem::sizeHint() const {
    const QFontMetrics metrics(font());
    int width = 2 * px(Spacing::MenuItemX) + metrics.horizontalAdvance(text());
    if (const auto* action = defaultAction(); action != nullptr && !action->shortcut().isEmpty())
        width += px(Spacing::M) +
                 metrics.horizontalAdvance(action->shortcut().toString(QKeySequence::NativeText));
    return {std::max(width, px(Size::MenuMinWidth)), px(Size::Control)};
}
void KMenuItem::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (isEnabled() && (underMouse() || hasFocus() || isDown())) {
        const QColor surface = color(isDown() ? Color::Field : Color::SurfaceRaised);
        fillRoundedSurface(painter, rect(), surface, surface, Radius::Small);
    }
    painter.setFont(font());
    const int inset = px(Spacing::MenuItemX);
    const QRect textRect = rect().adjusted(inset, 0, -inset, 0);
    painter.setPen(color(isEnabled() ? Color::Foreground : Color::Muted));
    painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text());
    if (const auto* action = defaultAction(); action != nullptr && !action->shortcut().isEmpty()) {
        painter.setPen(color(Color::Muted));
        painter.drawText(textRect, Qt::AlignRight | Qt::AlignVCenter,
                         action->shortcut().toString(QKeySequence::NativeText));
    }
}
KIconButton::KIconButton(QWidget* parent) : QToolButton(parent) {
    setFont(kit::font(TypeRole::Ui));
    setFixedSize(px(Size::Control), px(Size::Control));
    setIconSize(QSize(px(Size::IconChrome), px(Size::IconChrome)));
    setAutoRaise(true);
    setProperty("kitControl", true);
}
void KIconButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const bool heading = parentWidget() && parentWidget()->property("headerRow").toBool();
    if (!heading)
        fillRoundedSurface(painter, rect(),
                           color(isChecked() ? (isDown() ? Color::AccentPressed : Color::Accent)
                                             : Color::ControlSurface),
                           color(borderForInteraction(isEnabled(), hasFocus(), underMouse())),
                           Radius::Small);
    auto glyph = icon().pixmap(iconSize(), devicePixelRatioF(),
                               isEnabled() ? QIcon::Normal : QIcon::Disabled,
                               isChecked() ? QIcon::On : QIcon::Off);
    if (isChecked() && !glyph.isNull()) {
        QPainter ink(&glyph);
        ink.setCompositionMode(QPainter::CompositionMode_SourceIn);
        ink.fillRect(glyph.rect(), isEnabled()
                                       ? color(Color::OnAccent)
                                       : withOpacity(color(Color::OnAccent), kDisabledOpacity));
    }
    const auto extent = glyph.deviceIndependentSize();
    const auto dpr = devicePixelRatioF();
    painter.drawPixmap(QPointF(std::round((width() - extent.width()) * dpr / 2) / dpr,
                               std::round((height() - extent.height()) * dpr / 2) / dpr),
                       glyph);
    painter.setPen(color(isChecked() ? Color::OnAccent : Color::Muted));
    if (icon().isNull())
        painter.drawText(rect(), Qt::AlignCenter, text());
}

KIconToggle::KIconToggle(IconId id, QWidget* parent) : KIconButton(parent), glyph_(id) {
    setCheckable(true);
    setFixedSize(px(Size::ToggleCell), px(Size::ToggleCell));
    ensureKeyboardFocusTracking(*this);
}
void KIconToggle::setGlyph(IconId id) {
    glyph_ = id;
    update();
}
QPixmap KIconToggle::glyphPixmap() const {
    if (property("toolChoice").toBool() && isChecked())
        return iconPixmap(glyph_, Size::IconControl, Color::OnAccent, State::Normal,
                          IconWeight::Fill, devicePixelRatioF());
    return iconPixmap(glyph_, Size::IconControl, Color::Muted,
                      !isEnabled()  ? State::Disabled
                      : isChecked() ? State::Selected
                                    : State::Normal,
                      isChecked() ? IconWeight::Fill : IconWeight::Regular, devicePixelRatioF());
}
void KIconToggle::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const QRectF box = rect();
    fillRoundedSurface(
        painter, box,
        color(property("toolChoice").toBool() && isChecked() ? Color::Accent : Color::Surface),
        color(borderForInteraction(isEnabled(), hasKeyboardFocus(*this), underMouse())),
        Radius::Small);
    const auto glyph = glyphPixmap();
    const auto size = glyph.deviceIndependentSize();
    const qreal dpr = devicePixelRatioF();
    const QPointF origin(std::round((width() - size.width()) * dpr / 2) / dpr,
                         std::round((height() - size.height()) * dpr / 2) / dpr);
    painter.drawPixmap(origin, glyph);
}
KToolColumn::KToolColumn(QWidget* parent)
    : QWidget(parent), column_(new QVBoxLayout(this)), group_(new QButtonGroup(this)) {
    setFixedWidth(px(Size::ToolColumnWidth));
    column_->setContentsMargins(px(Spacing::ChromePadding), px(Spacing::ChromePadding),
                                px(Spacing::ChromePadding), px(Spacing::ChromePadding));
    column_->setSpacing(px(Spacing::ChromeGap));
    setAutoFillBackground(true);
    auto colors = palette();
    colors.setColor(QPalette::Window, color(Color::Surface));
    setPalette(colors);
    group_->setExclusive(true);
}
KIconToggle* KToolColumn::addTool(IconId id, const QString& label, const QString& objectName,
                                  bool enabled) {
    auto* button = new KIconToggle(id, this);
    button->setObjectName(objectName);
    button->setAccessibleName(label);
    button->setToolTip(label);
    button->setProperty("toolChoice", true);
    button->setFixedSize(px(Size::ToggleCell), px(Size::ToggleCell));
    button->setEnabled(enabled);
    group_->addButton(button);
    column_->addWidget(button, 0, Qt::AlignHCenter);
    return button;
}
KLabel::KLabel(QWidget* parent) : KLabel(QString{}, parent) {}
KLabel::KLabel(const QString& text, QWidget* parent, TypeRole role) : QLabel(text, parent) {
    setTypeRole(role);
    setFixedHeight(px(Size::Control));
}
void KLabel::setTypeRole(TypeRole role) { setFont(kit::font(role)); }
void KLabel::setElidedText(const QString& text) {
    fullText_ = text;
    elides_ = true;
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setToolTip(text);
    setText(fontMetrics().elidedText(fullText_, Qt::ElideRight, width()));
}
void KLabel::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    if (elides_)
        setText(fontMetrics().elidedText(fullText_, Qt::ElideRight, width()));
}
KLineEdit::KLineEdit(QWidget* parent) : KLineEdit(QString{}, parent) {}
KLineEdit::KLineEdit(const QString& text, QWidget* parent) : QLineEdit(text, parent) {
    setFont(kit::font(TypeRole::Ui));
    setFixedHeight(px(Size::Control));
    setProperty("kitControl", true);
}
KSearchField::KSearchField(QWidget* parent) : KLineEdit(parent) {}
namespace {
class FlowMenu final : public QMenu {
  public:
    FlowMenu(const QString& title, QWidget* parent) : QMenu(title, parent) {
        connect(this, &QMenu::aboutToShow, this, [this] { buildColumns(); });
        connect(this, &QMenu::aboutToHide, this, [this] { restoreActions(); });
    }

  protected:
    void showEvent(QShowEvent* event) override {
        QMenu::showEvent(event);
        if (!property("columnFlow").toBool())
            return;
        const auto bounds = ownerBounds();
        move(std::clamp(x(), bounds.left(), std::max(bounds.left(), bounds.right() - width() + 1)),
             std::clamp(y(), bounds.top(), std::max(bounds.top(), bounds.bottom() - height() + 1)));
    }

  private:
    QRect ownerBounds() const {
        auto* owner = parentWidget();
        while (owner && qobject_cast<QMenu*>(owner))
            owner = owner->parentWidget();
        if (!owner)
            return geometry();
        owner = owner->window();
        return {owner->mapToGlobal(QPoint()), owner->size()};
    }
    // Puts the menu back the way it was declared. The originals are REMOVED from the menu while
    // the flow shows, never hidden: QAction::setVisible(false) also clears the action's enabled
    // state, which the KMenuItems copied -- that is exactly why every flowed Add entry went grey
    // and refused to trigger (owner, 2026-09-15).
    void restoreActions() {
        if (columns_) {
            removeAction(columns_);
            columns_->deleteLater();
            columns_ = nullptr;
        }
        if (!originals_.isEmpty()) {
            addActions(originals_);
            originals_.clear();
        }
    }
    void buildColumns() {
        if (!property("columnFlow").toBool())
            return;
        restoreActions();
        const auto bounds = ownerBounds();
        const int padding = px(Spacing::ChromePadding);
        const int cap = static_cast<int>(bounds.height() * kMenuWindowHeightShare);
        const int rows = std::max(1, (cap - 4 * padding) / px(Size::Control));
        auto* content = new QWidget;
        auto* grid = new QGridLayout(content);
        grid->setContentsMargins(padding, padding, padding, padding);
        grid->setSpacing(0);
        originals_ = actions();
        int index = 0;
        for (auto* action : originals_) {
            if (!action->isVisible() || action->isSeparator())
                continue;
            auto* button = new KMenuItem(content);
            button->setDefaultAction(action);
            button->setFixedWidth(std::max(px(Size::MenuMinWidth), button->sizeHint().width()));
            grid->addWidget(button, index % rows, index / rows);
            ++index;
            connect(button, &QToolButton::clicked, this, [this] {
                for (auto* menu = static_cast<QMenu*>(this); menu;
                     menu = qobject_cast<QMenu*>(menu->parentWidget()))
                    menu->close();
            });
        }
        for (auto* action : originals_)
            removeAction(action);
        for (auto* button : content->findChildren<KMenuItem*>())
            button->show();
        columns_ = new QWidgetAction(this);
        columns_->setDefaultWidget(content);
        addAction(columns_);
        setProperty("flowColumns", grid->columnCount());
        setProperty("flowHeightCap", cap);
    }
    QList<QAction*> originals_;
    QWidgetAction* columns_ = nullptr;
};
} // namespace
QMenu* makeMenu(QWidget* parent) { return new FlowMenu({}, parent); }
QMenu* makeMenu(const QString& title, QWidget* parent) { return new FlowMenu(title, parent); }
} // namespace bloom::ui::kit
