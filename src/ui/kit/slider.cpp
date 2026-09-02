#include <bloom/ui/kit/slider.hpp>

#include <bloom/ui/kit/painting.hpp>

#include <QEnterEvent>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>

namespace bloom::ui::kit {
namespace {

constexpr int kTrackHeight = 4;
constexpr int kHandleDiameter = 12;

// One arrow key press moves this fraction of the range; with Page it moves ten times as far.
constexpr double kKeyStepFraction = 0.01;

} // namespace

KSlider::KSlider(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("kSlider"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
}

void KSlider::setRange(const double minimum, const double maximum) {
    minimum_ = minimum;
    // A collapsed range would make every position mean every value; keep the maximum strictly
    // above the minimum and let normalizedValue() stay well defined.
    maximum_ = std::max(maximum, minimum + std::numeric_limits<double>::epsilon());
    commitValue(value_);
    update();
}

double KSlider::minimum() const noexcept { return minimum_; }

double KSlider::maximum() const noexcept { return maximum_; }

double KSlider::value() const noexcept { return value_; }

void KSlider::setValue(const double value) { commitValue(value); }

void KSlider::commitValue(const double value) {
    const double clamped = std::clamp(value, minimum_, maximum_);
    if (clamped == value_) {
        return;
    }
    value_ = clamped;
    update();
    Q_EMIT valueChanged(value_);
}

double KSlider::normalizedValue() const { return (value_ - minimum_) / (maximum_ - minimum_); }

QRectF KSlider::trackRect() const {
    const auto handle = static_cast<qreal>(kHandleDiameter);
    return {handle / 2.0, (height() - kTrackHeight) / 2.0, std::max(0.0, width() - handle),
            static_cast<qreal>(kTrackHeight)};
}

QRectF KSlider::fillRect() const {
    const QRectF track = trackRect();
    return {track.left(), track.top(), track.width() * normalizedValue(), track.height()};
}

QRectF KSlider::handleRect() const {
    const QRectF track = trackRect();
    const auto handle = static_cast<qreal>(kHandleDiameter);
    const qreal centre = track.left() + track.width() * normalizedValue();
    return {centre - handle / 2.0, track.center().y() - handle / 2.0, handle, handle};
}

double KSlider::valueForPosition(const double x) const {
    const QRectF track = trackRect();
    if (track.width() <= 0.0) {
        return minimum_;
    }
    const double normalized = std::clamp((x - track.left()) / track.width(), 0.0, 1.0);
    return minimum_ + normalized * (maximum_ - minimum_);
}

State KSlider::visualState() const {
    if (!isEnabled()) {
        return State::Disabled;
    }
    if (dragging_) {
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

bool KSlider::isDragging() const noexcept { return dragging_; }

QSize KSlider::sizeHint() const { return {px(Size::ControlRoomy) * 4, px(Size::ControlCompact)}; }

QSize KSlider::minimumSizeHint() const { return {kHandleDiameter * 3, px(Size::ControlCompact)}; }

void KSlider::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !isEnabled()) {
        QWidget::mousePressEvent(event);
        return;
    }
    dragging_ = true;
    // Pressing anywhere on the track takes the value there immediately rather than nudging toward
    // it: the artist asked for that value by pointing at it.
    commitValue(valueForPosition(event->position().x()));
    update();
    event->accept();
}

void KSlider::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_) {
        commitValue(valueForPosition(event->position().x()));
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void KSlider::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_ && event->button() == Qt::LeftButton) {
        dragging_ = false;
        update();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void KSlider::keyPressEvent(QKeyEvent* event) {
    const double span = maximum_ - minimum_;
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Down:
        commitValue(value_ - span * kKeyStepFraction);
        event->accept();
        return;
    case Qt::Key_Right:
    case Qt::Key_Up:
        commitValue(value_ + span * kKeyStepFraction);
        event->accept();
        return;
    case Qt::Key_PageDown:
        commitValue(value_ - span * kKeyStepFraction * 10.0);
        event->accept();
        return;
    case Qt::Key_PageUp:
        commitValue(value_ + span * kKeyStepFraction * 10.0);
        event->accept();
        return;
    case Qt::Key_Home:
        commitValue(minimum_);
        event->accept();
        return;
    case Qt::Key_End:
        commitValue(maximum_);
        event->accept();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void KSlider::enterEvent(QEnterEvent* event) {
    if (isEnabled()) {
        hovered_ = true;
        update();
    }
    QWidget::enterEvent(event);
}

void KSlider::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void KSlider::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        hovered_ = false;
        dragging_ = false;
    }
    QWidget::changeEvent(event);
}

void KSlider::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    const State state = visualState();

    fillRoundedSurface(painter, trackRect(), color(surfaceForState(Color::Field, state)), {},
                       Radius::Full);

    QColor accent = color(Color::Accent);
    if (state == State::Hover) {
        accent = color(Color::AccentHover);
    } else if (state == State::Pressed) {
        accent = color(Color::AccentPressed);
    } else if (state == State::Disabled) {
        accent = withOpacity(accent, kDisabledOpacity);
    }
    const QRectF filled = fillRect();
    if (filled.width() > 0.0) {
        fillRoundedSurface(painter, filled, accent, {}, Radius::Full);
    }

    if (hasFocus() && state != State::Disabled) {
        drawFocusRing(painter, handleRect(), Radius::Full);
    }

    QColor handleFill = color(Color::Foreground);
    if (state == State::Disabled) {
        handleFill = withOpacity(handleFill, kDisabledOpacity);
    }
    fillRoundedSurface(painter, handleRect(), handleFill, color(borderForState(state)),
                       Radius::Full);
}

} // namespace bloom::ui::kit
