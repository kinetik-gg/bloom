#include <bloom/ui/kit/dropdown.hpp>

#include <bloom/ui/kit/accessibility.hpp>
#include <bloom/ui/kit/dropdown_popup.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/popup_list.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>

#include <QAccessible>
#include <QApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QStandardItem>
#include <QStandardItemModel>

#include <algorithm>
#include <cmath>

namespace bloom::ui::kit {
namespace {

// The caret pair sits in a column this wide, inside the field's trailing padding.
[[nodiscard]] int caretColumnWidth() { return px(Size::IconSmall); }

constexpr int kDataRole = Qt::UserRole + 3;

} // namespace

KDropdown::KDropdown(QWidget* parent) : QWidget(parent) {
    installKitAccessibility();
    ensureKeyboardFocusTracking(*this);
    setObjectName(QStringLiteral("kDropdown"));
    setFocusPolicy(Qt::StrongFocus);
    setFixedHeight(px(Size::Control));
    setAttribute(Qt::WA_Hover, true);
    setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    setFont(kit::font(TypeRole::Ui));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    model_ = new QStandardItemModel(this);
    popup_ = new KDropdownPopup(this);
    popup_->setModel(model_);
    popup_->view()->setIconSize(QSize(px(Size::IconMedium), px(Size::IconMedium)));
    connect(popup_, &KDropdownPopup::itemChosen, this, [this](const int index) {
        chooseIndex(index);
        update();
    });
    connect(popup_, &KDropdownPopup::focusMoveRequested, this, [this](const bool forward) {
        // Tab from the open list moves on exactly as Tab from the closed field does.
        QKeyEvent tab(QEvent::KeyPress, forward ? Qt::Key_Tab : Qt::Key_Backtab,
                      forward ? Qt::NoModifier : Qt::ShiftModifier);
        QCoreApplication::sendEvent(this, &tab);
    });
    connect(popup_, &KPopupWindow::closed, this, [this] {
        update();
        if (QAccessible::isActive()) {
            QAccessible::State expanded;
            expanded.expanded = true;
            expanded.collapsed = true;
            QAccessibleStateChangeEvent event(this, expanded);
            QAccessible::updateAccessibility(&event);
        }
    });
}

KDropdown::~KDropdown() = default;

int KDropdown::addItem(const QString& text, const QVariant& data) {
    auto* item = new QStandardItem(text);
    item->setData(data, kDataRole);
    item->setEditable(false);
    model_->appendRow(item);
    const int index = model_->rowCount() - 1;
    if (currentIndex_ < 0) {
        // Silently: building a list is not a change of value, and must never reach a commit.
        currentIndex_ = index;
        setToolTip(item->toolTip());
        update();
    }
    invalidateWidest();
    Q_EMIT itemsChanged();
    return index;
}

QIcon KDropdown::itemIcon(const int index) const {
    const auto* item = model_->item(index);
    return item == nullptr ? QIcon{} : item->icon();
}

void KDropdown::setItemText(const int index, const QString& text) {
    auto* item = model_->item(index);
    if (item == nullptr || item->text() == text)
        return;
    item->setText(text);
    if (index == currentIndex_)
        update();
    invalidateWidest();
    Q_EMIT itemsChanged();
}

void KDropdown::setItemToolTip(const int index, const QString& toolTip) {
    auto* item = model_->item(index);
    if (item == nullptr) {
        return;
    }
    item->setToolTip(toolTip);
    if (index == currentIndex_) {
        setToolTip(toolTip);
    }
}

int KDropdown::findData(const QVariant& data) const {
    for (int index = 0; index < model_->rowCount(); ++index) {
        if (itemData(index) == data) {
            return index;
        }
    }
    return -1;
}

QVariant KDropdown::currentData() const { return itemData(currentIndex_); }

int KDropdown::addItem(const QIcon& icon, const QString& text, const QVariant& data) {
    const int index = addItem(text, data);
    model_->item(index)->setIcon(icon);
    invalidateWidest();
    return index;
}

int KDropdown::count() const { return model_->rowCount(); }

void KDropdown::clearItems() {
    model_->clear();
    currentIndex_ = -1;
    setToolTip({});
    invalidateWidest();
    update();
    Q_EMIT itemsChanged();
}

QString KDropdown::itemText(const int index) const {
    const auto* item = model_->item(index);
    return item == nullptr ? QString{} : item->text();
}

QVariant KDropdown::itemData(const int index) const {
    const auto* item = model_->item(index);
    return item == nullptr ? QVariant{} : item->data(kDataRole);
}

void KDropdown::setItemEnabled(const int index, const bool enabled) {
    auto* item = model_->item(index);
    if (item == nullptr || item->isEnabled() == enabled) {
        return;
    }
    item->setEnabled(enabled);
    if (index == currentIndex_) {
        // Still the value: repaint it muted, with the item's own explanation.
        setToolTip(item->toolTip());
        update();
    }
    Q_EMIT itemsChanged();
}

bool KDropdown::isItemEnabled(const int index) const {
    const auto* item = model_->item(index);
    return item != nullptr && item->isEnabled();
}

int KDropdown::currentIndex() const { return currentIndex_; }

void KDropdown::setCurrentIndex(const int index) { (void)commitIndex(index); }

void KDropdown::chooseIndex(const int index) {
    if (commitIndex(index))
        Q_EMIT activated(index);
}

bool KDropdown::commitIndex(const int index) {
    if (index < 0 || index >= model_->rowCount()) {
        return false;
    }
    // A disabled item is refused here too, not only on click: a programmatic path must not be able
    // to select a value the interface says is unavailable.
    if (!isItemEnabled(index) || currentIndex_ == index) {
        return false;
    }
    currentIndex_ = index;
    setToolTip(model_->item(index)->toolTip());
    update();
    if (QAccessible::isActive()) {
        QAccessibleValueChangeEvent event(this, currentText());
        QAccessible::updateAccessibility(&event);
    }
    Q_EMIT currentIndexChanged(index);
    return true;
}

QString KDropdown::currentText() const { return itemText(currentIndex_); }

int KDropdown::horizontalPadding() const {
    return px(controlSize_ == ControlSize::Dense ? Spacing::XS : Spacing::M);
}
int KDropdown::caretGap() const {
    return px(controlSize_ == ControlSize::Dense ? Spacing::XXS : Spacing::S);
}

int KDropdown::textOffset() const {
    const int icon = itemIcon(currentIndex_).isNull() ? 0 : px(Size::IconMedium) + caretGap();
    return px(Spacing::ControlInset) + horizontalPadding() + icon;
}

// The one rectangle the value's text occupies: inside the control box, after the padding and any
// icon, and short of the caret column. Painting and eliding both use it.
QRectF KDropdown::labelRect() const {
    const QRectF bounds = controlBounds(*this);
    const qreal left = static_cast<qreal>(textOffset());
    const qreal right = bounds.right() - horizontalPadding() - caretColumnWidth() - caretGap();
    return {left, bounds.top(), std::max<qreal>(0.0, right - left), bounds.height()};
}

QString KDropdown::displayedText() const {
    const QFontMetrics metrics(font());
    return metrics.elidedText(currentText(), Qt::ElideRight,
                              static_cast<int>(std::floor(labelRect().width())));
}

void KDropdown::setControlSize(const ControlSize size) {
    if (controlSize_ == size) {
        return;
    }
    controlSize_ = size;
    setFixedHeight(controlExtent());
    updateGeometry();
    update();
}

KDropdown::ControlSize KDropdown::controlSize() const noexcept { return controlSize_; }

void KDropdown::showPopup() {
    if (model_->rowCount() == 0) {
        return;
    }
    const QFontMetrics metrics(popup_->view()->font());
    int widestText = 0;
    for (int index = 0; index < model_->rowCount(); ++index)
        widestText = std::max(widestText, metrics.horizontalAdvance(itemText(index)));
    popup_->setWidestRowText(widestText);
    popup_->openFor(*this, currentIndex_, textOffset() - (itemIcon(currentIndex_).isNull()
                                                              ? 0
                                                              : px(Size::IconMedium) + caretGap()));
    update();
    if (QAccessible::isActive()) {
        QAccessible::State expanded;
        expanded.expanded = true;
        expanded.collapsed = true;
        QAccessibleStateChangeEvent event(this, expanded);
        QAccessible::updateAccessibility(&event);
    }
}

void KDropdown::hidePopup() { popup_->close(); }

void KDropdown::setSearchable(const bool searchable) { popup_->setSearchable(searchable); }

bool KDropdown::searchable() const noexcept { return popup_->searchable(); }

bool KDropdown::isPopupVisible() const { return popup_->isVisible(); }

KDropdownPopup* KDropdown::popup() const noexcept { return popup_; }

QListView* KDropdown::popupView() const noexcept { return popup_->view(); }

State KDropdown::visualState() const {
    if (!isEnabled()) {
        return State::Disabled;
    }
    if (isPopupVisible()) {
        return State::Pressed;
    }
    if (hovered_) {
        return State::Hover;
    }
    if (hasKeyboardFocus(*this)) {
        return State::Focused;
    }
    return State::Normal;
}

Color KDropdown::borderToken() const {
    return borderForInteraction(isEnabled(), isPopupVisible() || hasKeyboardFocus(*this), hovered_);
}

int KDropdown::controlExtent() const {
    return controlSize_ == ControlSize::Roomy ? px(Size::ControlRoomy) : px(Size::Control);
}

void KDropdown::invalidateWidest() {
    widest_ = -1;
    updateGeometry();
}

int KDropdown::widestItemWidth() const {
    // Measured once per change of the items, not once per layout pass: the font selector holds
    // the whole system catalogue.
    if (widest_ < 0) {
        // Fractional advances, rounded up: an item measured a fraction short would elide at its
        // own preferred width.
        const QFontMetricsF metrics(font());
        widest_ = 0;
        for (int index = 0; index < model_->rowCount(); ++index) {
            const auto text = static_cast<int>(std::ceil(metrics.horizontalAdvance(itemText(index))));
            widest_ = std::max(widest_, text + (itemIcon(index).isNull()
                                                    ? 0
                                                    : px(Size::IconMedium) + caretGap()));
        }
    }
    return widest_;
}

QSize KDropdown::sizeHint() const {
    const int chrome = 2 * px(Spacing::ControlInset) + horizontalPadding() * 2 + caretGap() +
                       caretColumnWidth();
    return {widestItemWidth() + chrome, controlExtent()};
}

QSize KDropdown::minimumSizeHint() const {
    const int chrome = 2 * px(Spacing::ControlInset) + horizontalPadding() * 2 + caretGap() +
                       caretColumnWidth();
    const int characters = QFontMetrics(font()).averageCharWidth() * kDropdownMinimumCharacters;
    return {chrome + std::min(widestItemWidth(), characters), controlExtent()};
}

void KDropdown::setWidthFloor(const int width) {
    // Hug the widest item when the layout has room, shrink (and elide) toward the floor when it
    // does not, and never grow past the widest item.
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    setMinimumWidth(width);
    updateGeometry();
}

void KDropdown::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && isEnabled()) {
        isPopupVisible() ? hidePopup() : showPopup();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void KDropdown::stepValue(const int direction) {
    for (int index = currentIndex_ + direction; index >= 0 && index < count(); index += direction) {
        if (isItemEnabled(index)) {
            chooseIndex(index);
            return;
        }
    }
}

void KDropdown::typeAhead(const QString& text) {
    if (!typedClock_.isValid() || typedClock_.elapsed() > QApplication::keyboardInputInterval())
        typed_.clear();
    typedClock_.start();
    typed_ += text;
    const int start = std::max(0, currentIndex_ + (typed_.size() == 1 ? 1 : 0));
    for (int step = 0; step < count(); ++step) {
        const int index = (start + step) % count();
        if (isItemEnabled(index) && itemText(index).startsWith(typed_, Qt::CaseInsensitive)) {
            chooseIndex(index);
            return;
        }
    }
}

void KDropdown::keyPressEvent(QKeyEvent* event) {
    const bool alt = event->modifiers().testFlag(Qt::AltModifier);
    switch (event->key()) {
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_F4:
        showPopup();
        event->accept();
        return;
    case Qt::Key_Up:
    case Qt::Key_Down:
#if defined(Q_OS_MACOS)
        // macOS opens a pop-up button's menu from the arrow keys rather than stepping its value.
        showPopup();
#else
        // Windows and Linux step the value of a closed dropdown; Alt+Down (or Alt+Up) opens it.
        if (alt)
            showPopup();
        else
            stepValue(event->key() == Qt::Key_Down ? 1 : -1);
#endif
        event->accept();
        return;
    case Qt::Key_Home:
    case Qt::Key_End:
        if (!alt) {
            const bool last = event->key() == Qt::Key_End;
            for (int step = 0; step < count(); ++step) {
                const int index = last ? count() - 1 - step : step;
                if (isItemEnabled(index)) {
                    chooseIndex(index);
                    break;
                }
            }
            event->accept();
            return;
        }
        break;
    default:
        break;
    }
    if ((event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) == 0 &&
        !event->text().isEmpty() && event->text().at(0).isPrint()) {
        typeAhead(event->text());
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void KDropdown::enterEvent(QEnterEvent* event) {
    if (isEnabled()) {
        hovered_ = true;
        update();
    }
    QWidget::enterEvent(event);
}

void KDropdown::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void KDropdown::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange) {
        if (!isEnabled())
            hovered_ = false;
        // A disabled control does not invite a click.
        setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        Q_EMIT itemsChanged();
    } else if (event->type() == QEvent::FontChange) {
        invalidateWidest();
    }
    QWidget::changeEvent(event);
}

void KDropdown::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    const State state = visualState();
    const QRectF bounds = snappedToDevicePixels(painter, controlBounds(*this));

    // A closed field is a button, so it wears the one control look (kit::buttonColors Secondary):
    // a SurfaceRaised plate stepped by state, Foreground ink. An open popup holds it pressed and
    // lights the border Accent, exactly as keyboard focus does.
    const ButtonColors colors = buttonColors(ButtonKind::Secondary, state,
                                             isPopupVisible() || hasKeyboardFocus(*this));
    fillRoundedSurface(painter, bounds, colors.fill, colors.border, Radius::Small);

    // A value that is no longer available (its item was disabled under it) reads muted.
    const bool unavailable = currentIndex_ >= 0 && !isItemEnabled(currentIndex_);
    const QColor ink =
        mutedValue_ || unavailable ? inkForState(Color::Muted, state) : colors.ink;
    const qreal ratio = devicePixelRatioF();
    const auto snap = [ratio](const qreal value) { return std::round(value * ratio) / ratio; };

    // The caret pair (task U8, issue #131, fix 3): Phosphor's own caret-up-down glyph, a single
    // vendored double chevron, rather than two separately stacked CaretUp/CaretDown icons -- it
    // reads as "this opens" rather than "this scrolls one way".
    const auto caretBox = static_cast<qreal>(px(Size::IconSmall));
    const QPointF caretOrigin(snap(bounds.right() - horizontalPadding() - caretColumnWidth()),
                              snap(bounds.center().y() - caretBox / 2.0));
    painter.drawPixmap(caretOrigin,
                       iconPixmap(IconId::CaretUpDown, Size::IconSmall, ink, ratio,
                                  iconWeight(IconRole::Chrome)));

    painter.setPen(ink);
    painter.setFont(font());
    const auto glyph = itemIcon(currentIndex_);
    if (!glyph.isNull()) {
        const int box = px(Size::IconMedium);
        // The closed field's glyph takes the field's Foreground ink with its label: a kit icon's
        // Active rendering is exactly that, and its Disabled one the faded ink.
        const auto pixmap = glyph.pixmap(QSize(box, box), ratio,
                                         isEnabled() ? QIcon::Active : QIcon::Disabled);
        painter.drawPixmap(QPointF(snap(bounds.left() + horizontalPadding()),
                                   snap(bounds.center().y() - box / 2.0)),
                           pixmap);
    }
    painter.drawText(labelRect(), Qt::AlignVCenter | Qt::AlignLeft, displayedText());
}

} // namespace bloom::ui::kit
