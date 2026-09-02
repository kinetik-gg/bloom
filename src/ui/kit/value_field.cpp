#include <bloom/ui/kit/value_field.hpp>

#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>

#include <QEnterEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace bloom::ui::kit {
namespace {

// The stepper column, and the width the label column claims before the cell begins.
constexpr int kStepperWidth = 14;
constexpr int kLabelColumnWidth = 72;

} // namespace

KValueField::KValueField(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("kValueField"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setFont(kit::font(TypeRole::Ui));
}

void KValueField::setLabel(const QString& label) {
    label_ = label;
    update();
}

QString KValueField::label() const { return label_; }

void KValueField::setUnit(const QString& unit) {
    unit_ = unit;
    updateGeometry();
    update();
}

QString KValueField::unit() const { return unit_; }

void KValueField::setRange(const double minimum, const double maximum) {
    minimum_ = minimum;
    maximum_ = std::max(maximum, minimum);
    commitValue(value_);
    update();
}

double KValueField::minimum() const noexcept { return minimum_; }

double KValueField::maximum() const noexcept { return maximum_; }

void KValueField::setSingleStep(const double step) { step_ = std::abs(step); }

double KValueField::singleStep() const noexcept { return step_; }

void KValueField::setDecimals(const int decimals) {
    decimals_ = std::clamp(decimals, 0, 9);
    updateGeometry();
    update();
}

int KValueField::decimals() const noexcept { return decimals_; }

double KValueField::value() const noexcept { return value_; }

void KValueField::setValue(const double value) { commitValue(value); }

void KValueField::commitValue(const double value) {
    const double clamped = std::clamp(value, minimum_, maximum_);
    if (clamped == value_) {
        return;
    }
    value_ = clamped;
    update();
    Q_EMIT valueChanged(value_);
}

void KValueField::stepBy(const int steps) {
    commitValue(value_ + step_ * static_cast<double>(steps));
}

QString KValueField::displayedValue() const { return QString::number(value_, 'f', decimals_); }

QRectF KValueField::labelRect() const {
    if (label_.isEmpty()) {
        return {};
    }
    return {0.0, 0.0, static_cast<qreal>(kLabelColumnWidth), static_cast<qreal>(height())};
}

QRectF KValueField::cellRect() const {
    const qreal left = label_.isEmpty() ? 0.0 : kLabelColumnWidth + px(Spacing::S);
    return QRectF(left, 0.0, std::max(0.0, width() - left), static_cast<qreal>(height()))
        .adjusted(0.0, kFocusRingWidth, 0.0, -kFocusRingWidth);
}

QRectF KValueField::stepUpRect() const {
    const QRectF cell = cellRect();
    return {cell.right() - kStepperWidth, cell.top(), static_cast<qreal>(kStepperWidth),
            cell.height() / 2.0};
}

QRectF KValueField::stepDownRect() const {
    const QRectF cell = cellRect();
    return {cell.right() - kStepperWidth, cell.center().y(), static_cast<qreal>(kStepperWidth),
            cell.height() / 2.0};
}

State KValueField::visualState() const {
    if (!isEnabled()) {
        return State::Disabled;
    }
    if (hovered_) {
        return State::Hover;
    }
    if (hasFocus()) {
        return State::Focused;
    }
    return State::Normal;
}

QSize KValueField::sizeHint() const {
    const QFontMetrics valueMetrics(kit::font(TypeRole::Value));
    // Sized for the widest number the range can produce, not for the number currently in it: the
    // field must not resize as digits change.
    const QString widest =
        QString::number(std::max(std::abs(minimum_), std::abs(maximum_)), 'f', decimals_);
    int cellWidth = valueMetrics.horizontalAdvance(widest) + px(Spacing::S) * 2 + kStepperWidth;
    if (!unit_.isEmpty()) {
        cellWidth += valueMetrics.horizontalAdvance(unit_) + px(Spacing::XS);
    }
    const int labelWidth = label_.isEmpty() ? 0 : kLabelColumnWidth + px(Spacing::S);
    const auto ringMargin = static_cast<int>(std::lround(kFocusRingWidth)) * 2;
    return {labelWidth + cellWidth, px(Size::Control) + ringMargin};
}

QSize KValueField::minimumSizeHint() const { return sizeHint(); }

void KValueField::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && isEnabled()) {
        if (stepUpRect().contains(event->position())) {
            stepBy(1);
            event->accept();
            return;
        }
        if (stepDownRect().contains(event->position())) {
            stepBy(-1);
            event->accept();
            return;
        }
        setFocus(Qt::MouseFocusReason);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void KValueField::wheelEvent(QWheelEvent* event) {
    if (!hasFocus() || !isEnabled()) {
        // A wheel over an unfocused field belongs to whatever is scrolling behind it. Stealing it
        // would silently change a parameter the artist was only scrolling past.
        QWidget::wheelEvent(event);
        return;
    }
    stepBy(event->angleDelta().y() > 0 ? 1 : -1);
    event->accept();
}

bool KValueField::event(QEvent* event) {
    // A focused value field owns its editing keys the way a spin box does: claim the
    // ShortcutOverride so window-level shortcuts (frame stepping, transport jumps) stay inert
    // while the artist is adjusting a value. Left/Right/Home/End are claimed for parity with the
    // spin-box widgets this field replaces even though only Up/Down/Page currently step.
    if (event->type() == QEvent::ShortcutOverride && hasFocus()) {
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Home:
        case Qt::Key_End:
            event->accept();
            return true;
        default:
            break;
        }
    }
    return QWidget::event(event);
}

void KValueField::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Up:
        stepBy(1);
        event->accept();
        return;
    case Qt::Key_Down:
        stepBy(-1);
        event->accept();
        return;
    case Qt::Key_PageUp:
        stepBy(10);
        event->accept();
        return;
    case Qt::Key_PageDown:
        stepBy(-10);
        event->accept();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void KValueField::enterEvent(QEnterEvent* event) {
    if (isEnabled()) {
        hovered_ = true;
        update();
    }
    QWidget::enterEvent(event);
}

void KValueField::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void KValueField::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        hovered_ = false;
    }
    QWidget::changeEvent(event);
}

void KValueField::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    const State state = visualState();

    if (!label_.isEmpty()) {
        painter.setFont(kit::font(TypeRole::Ui));
        painter.setPen(inkForState(Color::Muted, state));
        const QFontMetrics metrics(painter.font());
        painter.drawText(labelRect(), Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(label_, Qt::ElideRight, kLabelColumnWidth));
    }

    const QRectF cell = cellRect();
    fillRoundedSurface(painter, cell, color(surfaceForState(Color::Field, state)),
                       color(borderForState(state)), Radius::Small);
    if (hasFocus() && state != State::Disabled) {
        drawFocusRing(painter, cell, Radius::Small);
    }

    // The value takes the monospaced role; the unit takes muted ink so it reads as a unit rather
    // than as part of the number.
    painter.setFont(kit::font(TypeRole::Value));
    const QFontMetrics valueMetrics(painter.font());
    const QRectF text = cell.adjusted(px(Spacing::S), 0.0, -kStepperWidth - px(Spacing::XS), 0.0);
    QRectF unitRect;
    if (!unit_.isEmpty()) {
        const auto unitWidth = static_cast<qreal>(valueMetrics.horizontalAdvance(unit_));
        unitRect = QRectF(text.right() - unitWidth, text.top(), unitWidth, text.height());
        painter.setPen(inkForState(Color::Muted, state));
        painter.drawText(unitRect, Qt::AlignVCenter | Qt::AlignRight, unit_);
    }
    painter.setPen(inkForState(Color::Foreground, state));
    const qreal valueRight = unit_.isEmpty() ? text.right() : unitRect.left() - px(Spacing::XS);
    painter.drawText(QRectF(text.left(), text.top(), valueRight - text.left(), text.height()),
                     Qt::AlignVCenter | Qt::AlignRight, displayedValue());

    const QColor stepperInk = inkForState(Color::Faint, state);
    const auto glyph = static_cast<qreal>(px(Size::IconSmall));
    for (const auto& [rect, id] :
         {std::pair{stepUpRect(), IconId::CaretUp}, std::pair{stepDownRect(), IconId::CaretDown}}) {
        const QRectF box(rect.center().x() - glyph / 2.0, rect.center().y() - glyph / 2.0, glyph,
                         glyph);
        painter.drawPixmap(box.toRect(),
                           iconPixmap(id, Size::IconSmall, stepperInk, devicePixelRatioF()));
    }
}

} // namespace bloom::ui::kit
