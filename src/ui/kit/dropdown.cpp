#include <bloom/ui/kit/dropdown.hpp>

#include <bloom/ui/kit/dropdown_popup.hpp>
#include <bloom/ui/kit/icons.hpp>
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
    setObjectName(QStringLiteral("kDropdown"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
    setFont(kit::font(TypeRole::Ui));

    model_ = new QStandardItemModel(this);
    popup_ = new KDropdownPopup(this);
    popup_->setModel(model_);
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

int KDropdown::count() const { return model_->rowCount(); }

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
    update();
    Q_EMIT currentIndexChanged(index);
}

QString KDropdown::currentText() const { return itemText(currentIndex_); }

QString KDropdown::displayedText() const {
    const QFontMetrics metrics(font());
    const int available =
        std::max(0, width() - px(Spacing::M) * 2 - caretColumnWidth() - px(Spacing::S));
    return metrics.elidedText(currentText(), Qt::ElideRight, available);
}

void KDropdown::setControlSize(const ControlSize size) {
    if (controlSize_ == size) {
        return;
    }
    controlSize_ = size;
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
    if (hasFocus()) {
        return State::Focused;
    }
    return State::Normal;
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
        widest = std::max(widest, metrics.horizontalAdvance(itemText(index)));
    }
    const auto ringMargin = static_cast<int>(std::lround(kFocusRingWidth)) * 2;
    const int width =
        widest + px(Spacing::M) * 2 + px(Spacing::S) + caretColumnWidth() + ringMargin;
    return {width, controlExtent() + ringMargin};
}

QSize KDropdown::minimumSizeHint() const {
    const auto ringMargin = static_cast<int>(std::lround(kFocusRingWidth)) * 2;
    return {px(Spacing::M) * 2 + caretColumnWidth() + ringMargin, controlExtent() + ringMargin};
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

    // A closed field rests on Field, the top rung of the surface ladder, so its hover step is the
    // border alone -- exactly what the ladder's clamp says it should be.
    fillRoundedSurface(painter, bounds, color(surfaceForState(Color::Field, state)),
                       color(borderForState(state)), Radius::Small);
    if (hasFocus() && state != State::Disabled) {
        drawFocusRing(painter, bounds, Radius::Small);
    }

    const QColor ink = inkForState(Color::Foreground, state);
    const auto caretWidth = static_cast<qreal>(caretColumnWidth());
    const QRectF caretColumn(bounds.right() - px(Spacing::M) - caretWidth, bounds.top(), caretWidth,
                             bounds.height());

    // The caret pair: an up and a down chevron stacked, which reads as "this opens" rather than
    // "this scrolls one way".
    const auto caretBox = static_cast<qreal>(px(Size::IconSmall));
    const qreal caretGap = px(Spacing::XXS);
    const qreal caretTop = caretColumn.center().y() - caretBox + caretGap / 2.0;
    painter.drawPixmap(QRectF(caretColumn.left(), caretTop, caretBox, caretBox).toRect(),
                       iconPixmap(IconId::CaretUp, Size::IconSmall, ink, devicePixelRatioF()));
    painter.drawPixmap(
        QRectF(caretColumn.left(), caretTop + caretBox - caretGap, caretBox, caretBox).toRect(),
        iconPixmap(IconId::CaretDown, Size::IconSmall, ink, devicePixelRatioF()));

    painter.setPen(ink);
    painter.setFont(font());
    const QRectF label(bounds.left() + px(Spacing::M), bounds.top(),
                       caretColumn.left() - px(Spacing::S) - bounds.left() - px(Spacing::M),
                       bounds.height());
    painter.drawText(label, Qt::AlignVCenter | Qt::AlignLeft, displayedText());
}

} // namespace bloom::ui::kit
