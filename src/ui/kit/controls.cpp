#include <QAction>
#include <QButtonGroup>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStyle>
#include <QStyleOptionMenuItem>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>
namespace bloom::ui::kit {
KMenuButton::KMenuButton(QWidget* parent) : QToolButton(parent) {
    // Styled by the generated sheet from the MenuItem button recipe, including its keyboard focus
    // border, so a focus change must re-resolve its rules.
    restyleOnKeyboardFocus(*this);
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
KMenuFlow::KMenuFlow(QMenu* menu, QList<QAction*> actions, const int heightCap, QWidget* parent)
    : QWidget(parent), menu_(menu), actions_(std::move(actions)), rows_(1) {
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
    setFocusPolicy(Qt::StrongFocus);
    setProperty("kitControl", true);
    setProperty("menuFlow", true);
    int width = px(Size::MenuMinWidth);
    int height = 0;
    for (int index = 0; index < itemCount(); ++index) {
        QStyleOptionMenuItem option;
        fillOption(option, index);
        const QFontMetrics metrics(option.font);
        const QSize contents(metrics.horizontalAdvance(option.text) + 2 * px(Spacing::MenuItemX),
                             metrics.height());
        const QSize cell =
            menu_->style()->sizeFromContents(QStyle::CT_MenuItem, &option, contents, menu_);
        width = std::max(width, cell.width());
        height = std::max(height, cell.height());
    }
    cell_ = {width, std::max(height, px(Size::Control))};
    rows_ = std::max(1, heightCap / cell_.height());
    columns_ = std::max(1, (itemCount() + rows_ - 1) / rows_);
}
void KMenuFlow::fillOption(QStyleOptionMenuItem& option, const int index) const {
    const auto* action = actions_[index];
    option.initFrom(menu_);
    option.font = menu_->font();
    option.menuItemType = QStyleOptionMenuItem::Normal;
    option.checkType = QStyleOptionMenuItem::NotCheckable;
    option.menuHasCheckableItems = false;
    option.icon = action->icon();
    option.text = action->text();
    if (!action->shortcut().isEmpty())
        option.text += QLatin1Char('\t') + action->shortcut().toString(QKeySequence::NativeText);
    option.state = QStyle::State_None;
    if (action->isEnabled())
        option.state |= QStyle::State_Enabled;
    if (index == hovered_ && action->isEnabled())
        option.state |= QStyle::State_Selected;
    option.rect = itemRect(index);
}
QRect KMenuFlow::itemRect(const int index) const {
    if (index < 0 || index >= itemCount())
        return {};
    return {(index / rows_) * cell_.width(), (index % rows_) * cell_.height(), cell_.width(),
            cell_.height()};
}
QSize KMenuFlow::sizeHint() const {
    return {columns_ * cell_.width(), std::min(rows_, std::max(1, itemCount())) * cell_.height()};
}
int KMenuFlow::indexAt(const QPoint position) const {
    for (int index = 0; index < itemCount(); ++index)
        if (itemRect(index).contains(position))
            return index;
    return -1;
}
void KMenuFlow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    for (int index = 0; index < itemCount(); ++index) {
        QStyleOptionMenuItem option;
        fillOption(option, index);
        menu_->style()->drawControl(QStyle::CE_MenuItem, &option, &painter, menu_);
    }
}
void KMenuFlow::mouseMoveEvent(QMouseEvent* event) {
    const int index = indexAt(event->pos());
    if (index != hovered_) {
        hovered_ = index;
        update();
    }
    QWidget::mouseMoveEvent(event);
}
void KMenuFlow::leaveEvent(QEvent* event) {
    if (hovered_ != -1) {
        hovered_ = -1;
        update();
    }
    QWidget::leaveEvent(event);
}
void KMenuFlow::trigger(const int index) {
    auto* action = actions_[index];
    for (auto* menu = menu_; menu; menu = qobject_cast<QMenu*>(menu->parentWidget()))
        menu->close();
    action->trigger();
}
void KMenuFlow::mouseReleaseEvent(QMouseEvent* event) {
    const int index = indexAt(event->pos());
    if (event->button() == Qt::LeftButton && index >= 0 && actions_[index]->isEnabled()) {
        trigger(index);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}
void KMenuFlow::setCurrentIndex(const int index) {
    const int next = index >= 0 && index < itemCount() && actions_[index]->isEnabled() ? index : -1;
    if (next != hovered_) {
        hovered_ = next;
        update();
    }
}
// The first enabled row at or after `index`, walking by `step`; -1 when there is none.
int KMenuFlow::enabledFrom(int index, const int step) const {
    for (; index >= 0 && index < itemCount(); index += step)
        if (actions_[index]->isEnabled())
            return index;
    return -1;
}
void KMenuFlow::focusInEvent(QFocusEvent* event) {
    if (hovered_ < 0)
        setCurrentIndex(enabledFrom(0, 1));
    QWidget::focusInEvent(event);
}
void KMenuFlow::keyPressEvent(QKeyEvent* event) {
    const int current = hovered_;
    const int row = current < 0 ? -1 : current % rows_;
    const int column = current < 0 ? 0 : current / rows_;
    int target = -2;
    switch (event->key()) {
    case Qt::Key_Down:
        target = enabledFrom(current < 0 ? 0 : current + 1, 1);
        break;
    case Qt::Key_Up:
        target = current < 0 ? enabledFrom(itemCount() - 1, -1) : enabledFrom(current - 1, -1);
        break;
    case Qt::Key_Home:
        target = enabledFrom(0, 1);
        break;
    case Qt::Key_End:
        target = enabledFrom(itemCount() - 1, -1);
        break;
    case Qt::Key_Right:
    case Qt::Key_Left: {
        const int direction = event->key() == Qt::Key_Right ? 1 : -1;
        if (direction < 0 && column == 0) {
            // Left from the first column leaves the submenu, as Left does in any submenu.
            menu_->close();
            event->accept();
            return;
        }
        // The same row in the neighbouring column, or that column's last row when it is shorter.
        for (int next = column + direction; next >= 0 && next < columns_; next += direction) {
            const int base = next * rows_;
            const int candidate = std::min(base + std::max(row, 0), itemCount() - 1);
            const int found = enabledFrom(candidate, -1);
            if (found >= base) {
                target = found;
                break;
            }
            const int after = enabledFrom(candidate, 1);
            if (after >= 0 && after < base + rows_) {
                target = after;
                break;
            }
        }
        if (target == -2) {
            event->accept();
            return;
        }
        break;
    }
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        if (current >= 0 && actions_[current]->isEnabled())
            trigger(current);
        event->accept();
        return;
    case Qt::Key_Escape:
        menu_->close();
        event->accept();
        return;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    if (target >= 0)
        setCurrentIndex(target);
    event->accept();
}
KIconButton::KIconButton(QWidget* parent) : QToolButton(parent) {
    ensureKeyboardFocusTracking(*this);
    setFont(kit::font(TypeRole::Ui));
    setFixedSize(px(Size::Control), px(Size::Control));
    setIconSize(QSize(px(Size::IconMedium), px(Size::IconMedium)));
    setAutoRaise(true);
    setAttribute(Qt::WA_Hover, true);
    setProperty("kitControl", true);
}
State KIconButton::visualState() const {
    if (!isEnabled())
        return State::Disabled;
    if (isDown())
        return State::Pressed;
    if (isChecked())
        return State::Selected;
    return hovered_ ? State::Hover : State::Normal;
}
Color KIconButton::borderToken() const {
    return borderForInteraction(isEnabled(), hasKeyboardFocus(*this), hovered_);
}
void KIconButton::enterEvent(QEnterEvent* event) {
    hovered_ = isEnabled();
    update();
    QToolButton::enterEvent(event);
}
void KIconButton::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QToolButton::leaveEvent(event);
}
void KIconButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const bool heading = parentWidget() && parentWidget()->property("headerRow").toBool();
    const State state = visualState();
    const ButtonColors colors = buttonColors(ButtonKind::Recessed, state, hasKeyboardFocus(*this));
    if (!heading && !property("disclosure").toBool())
        fillRoundedSurface(painter, controlBounds(*this), colors.fill, colors.border,
                           Radius::Small);
    const QIcon::Mode mode = state == State::Disabled ? QIcon::Disabled
                             : state == State::Hover  ? QIcon::Active
                                                      : QIcon::Normal;
    auto glyph =
        icon().pixmap(iconSize(), devicePixelRatioF(), mode, isChecked() ? QIcon::On : QIcon::Off);
    if (isChecked() && !glyph.isNull()) {
        QPainter ink(&glyph);
        ink.setCompositionMode(QPainter::CompositionMode_SourceIn);
        ink.fillRect(glyph.rect(), colors.ink);
    }
    const auto extent = glyph.deviceIndependentSize();
    const auto dpr = devicePixelRatioF();
    painter.drawPixmap(QPointF(std::round((width() - extent.width()) * dpr / 2) / dpr,
                               std::round((height() - extent.height()) * dpr / 2) / dpr),
                       glyph);
    painter.setPen(colors.ink);
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
    if (property("layerSwitch").toBool()) {
        if (!isChecked())
            return {};
        return iconPixmap(glyph_, Size::IconMedium, Color::Foreground,
                          isEnabled() ? State::Normal : State::Disabled, IconWeight::Regular,
                          devicePixelRatioF());
    }
    if (property("toolChoice").toBool()) {
        // A tool reads at the chrome box and weight; the chosen one takes the Selected plate's
        // OnAccent ink in the Fill weight.
        if (isChecked())
            return iconPixmap(glyph_, Size::IconMedium, Color::OnAccent, State::Normal,
                              IconWeight::Fill, devicePixelRatioF());
        return iconPixmap(glyph_, Size::IconMedium, Color::Muted,
                          !isEnabled() ? State::Disabled
                          : underMouse() ? State::Hover
                                         : State::Normal,
                          iconWeight(IconRole::Chrome), devicePixelRatioF());
    }
    return iconPixmap(glyph_, Size::IconLarge, Color::Muted,
                      !isEnabled()  ? State::Disabled
                      : isChecked() ? State::Selected
                                    : State::Normal,
                      isChecked() ? IconWeight::Fill : IconWeight::Regular, devicePixelRatioF());
}
void KIconToggle::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const QRectF box = rect();
    if (property("toolChoice").toBool()) {
        // The kit's one Selected recipe on a chrome-weight (Ghost) plate: nothing at rest, the
        // hover step under the pointer, the Accent plate when chosen -- the same plate every
        // checked Viewer toggle shows, at the same ToggleCell size.
        const auto colors = buttonColors(ButtonKind::Ghost, visualState(), hasKeyboardFocus(*this));
        if (colors.fill.isValid() || colors.border.isValid())
            fillRoundedSurface(painter, box, colors.fill.isValid() ? colors.fill : Qt::transparent,
                               colors.border, Radius::Small);
    } else {
        fillRoundedSurface(
            painter, box, color(Color::Surface),
            color(borderForInteraction(isEnabled(), hasKeyboardFocus(*this), underMouse())),
            Radius::Small);
    }
    const auto glyph = glyphPixmap();
    const auto size = glyph.deviceIndependentSize();
    const qreal dpr = devicePixelRatioF();
    const QPointF origin(std::round((width() - size.width()) * dpr / 2) / dpr,
                         std::round((height() - size.height()) * dpr / 2) / dpr);
    painter.drawPixmap(origin, glyph);
}
KToolColumn::KToolColumn(QWidget* parent) : QWidget(parent), group_(new QButtonGroup(this)) {
    setFixedWidth(px(Size::ToolColumnWidth));
    setAutoFillBackground(true);
    auto colors = palette();
    colors.setColor(QPalette::Window, color(Color::Surface));
    setPalette(colors);
    group_->setExclusive(true);
    more_ = new KIconButton(this);
    more_->setObjectName(QStringLiteral("toolColumnMoreButton"));
    more_->setIcon(icon(IconId::ContextMenu, IconRole::Chrome));
    more_->setToolTip(tr("More tools"));
    more_->setAccessibleName(tr("More tools"));
    more_->setFixedSize(px(Size::ToggleCell), px(Size::ToggleCell));
    more_->setPopupMode(QToolButton::InstantPopup);
    more_->setMenu(makeMenu(this));
    more_->hide();
}
KIconToggle* KToolColumn::addTool(IconId id, const QString& label, const QString& objectName,
                                  bool enabled) {
    auto* button = new KIconToggle(id, this);
    button->setObjectName(objectName);
    button->setAccessibleName(label.section(QLatin1Char('\n'), 0, 0));
    button->setToolTip(label);
    button->setProperty("toolChoice", true);
    button->setFixedSize(px(Size::ToggleCell), px(Size::ToggleCell));
    button->setEnabled(enabled);
    group_->addButton(button);
    connect(button, &QAbstractButton::toggled, this, [this] {
        if (more_->isVisible())
            rebuildMoreMenu();
    });
    Item item;
    item.tool = button;
    items_.push_back(item);
    arrange();
    return button;
}
void KToolColumn::addSeparator() {
    items_.push_back({});
    arrange();
}
QSize KToolColumn::sizeHint() const {
    int height = 2 * px(Spacing::ChromePadding);
    for (const auto& item : items_)
        height += (item.tool ? px(Size::ToggleCell) : px(Spacing::S)) + px(Spacing::ChromeGap);
    return {px(Size::ToolColumnWidth), height};
}
QSize KToolColumn::minimumSizeHint() const {
    // One tool and the More button.
    return {px(Size::ToolColumnWidth), 2 * px(Spacing::ChromePadding) +
                                           2 * px(Size::ToggleCell) + px(Spacing::ChromeGap)};
}
void KToolColumn::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    arrange();
}
void KToolColumn::arrange() {
    const int padding = px(Spacing::ChromePadding);
    const int gap = px(Spacing::ChromeGap);
    const int cell = px(Size::ToggleCell);
    const int x = (width() - cell) / 2;
    const bool fitsAll = sizeHint().height() - gap <= height();
    // Everything above this line is the column's; below it, the More button when tools collapse.
    const int limit = fitsAll ? height() - padding : height() - padding - cell - gap;
    int y = padding;
    bool collapsing = false;
    for (std::size_t index = 0; index < items_.size(); ++index) {
        auto& item = items_[index];
        if (!item.tool) {
            // A separator shows only between two visible tools.
            const bool next = index + 1 < items_.size() && items_[index + 1].tool &&
                              y + px(Spacing::S) + cell <= limit;
            item.separator = !collapsing && next && y > padding
                                 ? QRectF(padding, y + px(Spacing::S) / 2.0 - gap / 2.0,
                                          width() - 2 * padding, px(Size::Hairline))
                                 : QRectF{};
            if (!item.separator.isNull())
                y += px(Spacing::S);
            continue;
        }
        collapsing = collapsing || y + cell > limit;
        item.tool->setVisible(!collapsing);
        item.tool->move(x, collapsing ? 0 : y);
        if (!collapsing)
            y += cell + gap;
    }
    more_->setVisible(collapsing);
    more_->move(x, height() - padding - cell);
    rebuildMoreMenu();
    update();
}
void KToolColumn::rebuildMoreMenu() {
    auto* menu = more_->menu();
    // Deferred deletion: this also runs from a tool the menu itself just chose.
    for (auto* action : menu->actions()) {
        menu->removeAction(action);
        action->deleteLater();
    }
    bool activeHidden = false;
    for (const auto& item : items_) {
        if (!item.tool || item.tool->isVisibleTo(this))
            continue;
        auto* tool = item.tool;
        auto* action = menu->addAction(icon(tool->glyph(), IconRole::Chrome),
                                       tool->toolTip().section(QLatin1Char('\n'), 0, 0));
        action->setCheckable(true);
        action->setChecked(tool->isChecked());
        action->setEnabled(tool->isEnabled());
        connect(action, &QAction::triggered, tool, &QAbstractButton::click);
        activeHidden = activeHidden || tool->isChecked();
    }
    // The active tool is never hidden from view: the More button carries its selected state.
    more_->setCheckable(true);
    more_->setChecked(activeHidden);
}
void KToolColumn::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    QPainter painter(this);
    for (const auto& item : items_)
        if (!item.separator.isNull())
            painter.fillRect(snappedToDevicePixels(painter, item.separator), color(Color::Border));
}
KLabel::KLabel(QWidget* parent) : KLabel(QString{}, parent) {}
KLabel::KLabel(const QString& text, QWidget* parent, TypeRole role) : QLabel(text, parent) {
    setTypeRole(role);
    applyHeightRule();
}
void KLabel::setTypeRole(TypeRole role) { setFont(kit::font(role)); }
void KLabel::setElidedText(const QString& text) {
    fullText_ = text;
    elides_ = true;
    QLabel::setWordWrap(false);
    applyHeightRule();
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setToolTip(text);
    setText(fontMetrics().elidedText(fullText_, Qt::ElideRight, width()));
}
void KLabel::setWordWrap(const bool on) {
    QLabel::setWordWrap(on);
    applyHeightRule();
}
void KLabel::setTone(const Color tone) {
    tone_ = tone;
    QPalette colors = palette();
    colors.setColor(QPalette::Active, QPalette::WindowText, color(tone));
    colors.setColor(QPalette::Inactive, QPalette::WindowText, color(tone));
    colors.setColor(QPalette::Disabled, QPalette::WindowText, disabledInk(color(tone)));
    setPalette(colors);
}
void KLabel::applyHeightRule() {
    if (wordWrap()) {
        // Wrapped text takes its height-for-width from QLabel; one control is only the floor.
        setMinimumHeight(px(Size::Control));
        setMaximumHeight(QWIDGETSIZE_MAX);
        if (sizePolicy().verticalPolicy() == QSizePolicy::Fixed) {
            auto policy = sizePolicy();
            policy.setVerticalPolicy(QSizePolicy::Preferred);
            setSizePolicy(policy);
        }
    } else {
        setFixedHeight(px(Size::Control));
    }
    updateGeometry();
}
void KLabel::showEvent(QShowEvent* event) {
    // QLabel::setWordWrap is not virtual: a wrap mode set through a QLabel pointer still gets the
    // right height rule before the label is first seen.
    const bool fixed = minimumHeight() == maximumHeight();
    if (fixed == wordWrap())
        applyHeightRule();
    QLabel::showEvent(event);
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
    // While the body is flowed, the menu's own keyboard handling has only one item to walk (the
    // flowed body itself); the flow's rows take the navigation keys instead.
    void keyPressEvent(QKeyEvent* event) override {
        if (flow_ != nullptr) {
            switch (event->key()) {
            case Qt::Key_Up:
            case Qt::Key_Down:
            case Qt::Key_Left:
            case Qt::Key_Right:
            case Qt::Key_Home:
            case Qt::Key_End:
            case Qt::Key_Return:
            case Qt::Key_Enter:
            case Qt::Key_Space:
                flow_->setFocus(Qt::PopupFocusReason);
                QCoreApplication::sendEvent(flow_, event);
                return;
            default:
                break;
            }
        }
        QMenu::keyPressEvent(event);
    }
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
        flow_ = nullptr;
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
        originals_ = actions();
        QList<QAction*> flowed;
        for (auto* action : originals_)
            if (action->isVisible() && !action->isSeparator())
                flowed.push_back(action);
        auto* content = new QWidget;
        auto* layout = new QVBoxLayout(content);
        // No padding above or below the rows: the first and last rows meet the menu frame, as a
        // plain menu's and a dropdown's do, and nothing is left under the last one.
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        // The frame and the widget action's margins sit inside the cap.
        auto* flow = new KMenuFlow(this, flowed, cap - 2 * padding, content);
        layout->addWidget(flow);
        // A keyboard-selected widget action hands focus to its widget; the flow takes it.
        content->setFocusPolicy(Qt::StrongFocus);
        content->setFocusProxy(flow);
        flow_ = flow;
        for (auto* action : originals_)
            removeAction(action);
        columns_ = new QWidgetAction(this);
        columns_->setDefaultWidget(content);
        addAction(columns_);
        setProperty("flowColumns", flow->columnCount());
        setProperty("flowHeightCap", cap);
    }
    QList<QAction*> originals_;
    QWidgetAction* columns_ = nullptr;
    KMenuFlow* flow_ = nullptr;
};
} // namespace
QMenu* makeMenu(QWidget* parent) { return new FlowMenu({}, parent); }
QMenu* makeMenu(const QString& title, QWidget* parent) { return new FlowMenu(title, parent); }
} // namespace bloom::ui::kit
