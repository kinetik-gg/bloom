#include <bloom/ui/kit/dropdown.hpp>

#include <bloom/ui/kit/dropdown_popup.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>

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

namespace bloom::ui::kit {
namespace {

// The caret pair sits in a column this wide, inside the field's trailing padding.
[[nodiscard]] int caretColumnWidth() { return px(Size::IconSmall); }

} // namespace

KDropdown::KDropdown(QWidget* parent) : QWidget(parent) {
    ensureKeyboardFocusTracking(*this);
    setObjectName(QStringLiteral("kDropdown"));
    setFocusPolicy(Qt::StrongFocus);
    setFixedHeight(px(Size::Control));
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
    setFont(kit::font(TypeRole::Ui));

    model_ = new QStandardItemModel(this);
    popup_ = new KDropdownPopup(this);
    popup_->setModel(model_);
    popup_->view()->setIconSize(QSize(px(Size::IconChrome), px(Size::IconChrome)));
    connect(popup_, &KDropdownPopup::itemChosen, this, [this](const int index) {
        commitIndex(index);
        update();
    });
}

KDropdown::~KDropdown() = default;

int KDropdown::addItem(const QString& text, const QVariant& data) {
    auto* item = new QStandardItem(text);
    item->setData(data, Qt::UserRole + 1);
    item->setEditable(false);
    model_->appendRow(item);
    const int index = model_->rowCount() - 1;
    if (currentIndex_ < 0) {
        commitIndex(index);
    }
    updateGeometry();
    return index;
}

QIcon KDropdown::itemIcon(const int index) const {
    const auto* item = model_->item(index);
    return item == nullptr ? QIcon{} : item->icon();
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
    if (objectName() == QStringLiteral("kDropdown"))
        setObjectName(QStringLiteral("kPanelSwitcher")); // Preserve the icon variant's legacy name.
    const int index = addItem(text, data);
    model_->item(index)->setIcon(icon);
    updateGeometry();
    return index;
}

int KDropdown::count() const { return model_->rowCount(); }

void KDropdown::clearItems() {
    model_->clear();
    currentIndex_ = -1;
    updateGeometry();
    update();
}

QString KDropdown::itemText(const int index) const {
    const auto* item = model_->item(index);
    return item == nullptr ? QString{} : item->text();
}

QVariant KDropdown::itemData(const int index) const {
    const auto* item = model_->item(index);
    return item == nullptr ? QVariant{} : item->data(Qt::UserRole + 1);
}

void KDropdown::setItemEnabled(const int index, const bool enabled) {
    auto* item = model_->item(index);
    if (item == nullptr) {
        return;
    }
    item->setEnabled(enabled);
    if (!enabled && currentIndex_ == index) {
        currentIndex_ = -1;
        update();
    }
}

bool KDropdown::isItemEnabled(const int index) const {
    const auto* item = model_->item(index);
    return item != nullptr && item->isEnabled();
}

int KDropdown::currentIndex() const { return currentIndex_; }

void KDropdown::setCurrentIndex(const int index) { commitIndex(index); }

void KDropdown::commitIndex(const int index) {
    if (index < 0 || index >= model_->rowCount()) {
        return;
    }
    // A disabled item is refused here too, not only on click: a programmatic path must not be able
    // to select a value the interface says is unavailable.
    if (!isItemEnabled(index) || currentIndex_ == index) {
        return;
    }
    currentIndex_ = index;
    setToolTip(model_->item(index)->toolTip());
    update();
    Q_EMIT currentIndexChanged(index);
}

QString KDropdown::currentText() const { return itemText(currentIndex_); }

int KDropdown::horizontalPadding() const {
    return px(controlSize_ == ControlSize::Compact ? Spacing::XS : Spacing::M);
}
int KDropdown::caretGap() const {
    return px(controlSize_ == ControlSize::Compact ? Spacing::XXS : Spacing::S);
}

QString KDropdown::displayedText() const {
    const QFontMetrics metrics(font());
    const int available =
        std::max(0, width() - horizontalPadding() * 2 - caretColumnWidth() - caretGap());
    return metrics.elidedText(
        currentText(), Qt::ElideRight,
        available - (itemIcon(currentIndex_).isNull() ? 0 : px(Size::IconChrome) + caretGap()));
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
    popup_->openBelow(*this, currentIndex_);
    update();
}

void KDropdown::hidePopup() { popup_->close(); }

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
    switch (controlSize_) {
    case ControlSize::Compact:
        return px(Size::ControlCompact);
    case ControlSize::Default:
        return px(Size::Control);
    case ControlSize::Roomy:
        return px(Size::ControlRoomy);
    }
    return px(Size::Control);
}

QSize KDropdown::sizeHint() const {
    const QFontMetrics metrics(font());
    int widest = 0;
    for (int index = 0; index < model_->rowCount(); ++index) {
        widest = std::max(widest,
                          metrics.horizontalAdvance(itemText(index)) +
                              (itemIcon(index).isNull() ? 0 : px(Size::IconChrome) + caretGap()));
    }
    const auto ringMargin = static_cast<int>(std::lround(kFocusRingWidth)) * 2;
    const int width =
        widest + horizontalPadding() * 2 + caretGap() + caretColumnWidth() + ringMargin;
    return {width, controlExtent()};
}

QSize KDropdown::minimumSizeHint() const { return sizeHint(); }
void KDropdown::setWidthFloor(int width) {
    QWidget::setFixedWidth(std::max(width, sizeHint().width()));
}

void KDropdown::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && isEnabled()) {
        isPopupVisible() ? hidePopup() : showPopup();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void KDropdown::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Down:
        showPopup();
        event->accept();
        return;
    case Qt::Key_Escape:
        if (isPopupVisible()) {
            hidePopup();
            event->accept();
            return;
        }
        break;
    default:
        break;
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
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        hovered_ = false;
    }
    QWidget::changeEvent(event);
}

void KDropdown::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    const State state = visualState();
    const auto ringMargin = kFocusRingWidth;
    const QRectF bounds = QRectF(rect()).adjusted(ringMargin, ringMargin, -ringMargin, -ringMargin);

    // A closed field rests on ControlSurface (task U8, issue 131, formal amendment 1, A2 --
    // previously Field). ControlSurface is not a surfaceStep() rung, so its hover step is the
    // border alone, exactly like Field's own behavior at the top of the ladder before it.
    fillRoundedSurface(painter, bounds, color(surfaceForState(Color::ControlSurface, state)),
                       color(borderToken()), Radius::Small);

    const QColor ink = inkForState(Color::Foreground, state);
    const auto caretWidth = static_cast<qreal>(caretColumnWidth());
    const QRectF caretColumn(bounds.right() - horizontalPadding() - caretWidth, bounds.top(),
                             caretWidth, bounds.height());

    // The caret pair (task U8, issue #131, fix 3): Phosphor's own caret-up-down glyph, a single
    // vendored double chevron, rather than two separately stacked CaretUp/CaretDown icons -- it
    // reads as "this opens" rather than "this scrolls one way".
    const auto caretBox = static_cast<qreal>(px(Size::IconSmall));
    const QRectF caretRect(caretColumn.left(), caretColumn.center().y() - caretBox / 2.0, caretBox,
                           caretBox);
    painter.drawPixmap(caretRect.toRect(),
                       iconPixmap(IconId::CaretUpDown, Size::IconSmall, ink, devicePixelRatioF(),
                                  iconWeight(IconRole::Chrome)));

    painter.setPen(ink);
    painter.setFont(font());
    qreal contentLeft = bounds.left() + horizontalPadding();
    const auto glyph = itemIcon(currentIndex_);
    if (!glyph.isNull()) {
        const int box = px(Size::IconChrome);
        const auto pixmap = glyph.pixmap(QSize(box, box), devicePixelRatioF());
        const QPointF origin(contentLeft, bounds.center().y() - box / 2.0);
        painter.drawPixmap(origin, pixmap);
        contentLeft += box + caretGap();
    }
    const QRectF label(contentLeft, bounds.top(), caretColumn.left() - caretGap() - contentLeft,
                       bounds.height());
    painter.drawText(label, Qt::AlignVCenter | Qt::AlignLeft, displayedText());
}

} // namespace bloom::ui::kit
