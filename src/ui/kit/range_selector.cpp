#include <bloom/ui/kit/range_selector.hpp>

#include <bloom/ui/kit/painting.hpp>

#include <QEnterEvent>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>

namespace bloom::ui::kit {
namespace {

constexpr int kTrackHeight = 6;
constexpr int kHandleWidth = 8;

} // namespace

KRangeSelector::KRangeSelector(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("kRangeSelector"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setCursor(Qt::SizeHorCursor);
}

void KRangeSelector::setRange(const double minimum, const double maximum) {
    minimum_ = minimum;
    maximum_ = std::max(maximum, minimum + std::numeric_limits<double>::epsilon());
    setValues(lower_, upper_);
    update();
}

double KRangeSelector::minimum() const noexcept { return minimum_; }

double KRangeSelector::maximum() const noexcept { return maximum_; }

double KRangeSelector::lowerValue() const noexcept { return lower_; }

double KRangeSelector::upperValue() const noexcept { return upper_; }

void KRangeSelector::setLowerValue(const double value) {
    // Clamped to the upper handle, never swapped past it: a swap would silently change which
    // handle the artist believes they are holding.
    const double clamped = std::clamp(value, minimum_, upper_);
    if (clamped == lower_) {
        return;
    }
    lower_ = clamped;
    update();
    Q_EMIT lowerValueChanged(lower_);
}

void KRangeSelector::setUpperValue(const double value) {
    const double clamped = std::clamp(value, lower_, maximum_);
    if (clamped == upper_) {
        return;
    }
    upper_ = clamped;
    update();
    Q_EMIT upperValueChanged(upper_);
}

void KRangeSelector::setValues(const double lower, const double upper) {
    const double newLower = std::clamp(lower, minimum_, maximum_);
    const double newUpper = std::clamp(std::max(upper, newLower), minimum_, maximum_);
    if (newLower != lower_) {
        lower_ = newLower;
        Q_EMIT lowerValueChanged(lower_);
    }
    if (newUpper != upper_) {
        upper_ = newUpper;
        Q_EMIT upperValueChanged(upper_);
    }
    update();
}

double KRangeSelector::normalized(const double value) const {
    return (value - minimum_) / (maximum_ - minimum_);
}

QRectF KRangeSelector::trackRect() const {
    const auto handle = static_cast<qreal>(kHandleWidth);
    return {handle / 2.0, (height() - kTrackHeight) / 2.0, std::max(0.0, width() - handle),
            static_cast<qreal>(kTrackHeight)};
}

QRectF KRangeSelector::spanRect() const {
    const QRectF track = trackRect();
    const qreal left = track.left() + track.width() * normalized(lower_);
    const qreal right = track.left() + track.width() * normalized(upper_);
    return {left, track.top(), std::max(0.0, right - left), track.height()};
}

QRectF KRangeSelector::handleRect(const Handle handle) const {
    if (handle == Handle::None) {
        return {};
    }
    const QRectF track = trackRect();
    const double at = handle == Handle::Lower ? lower_ : upper_;
    const qreal centre = track.left() + track.width() * normalized(at);
    const auto handleHeight = static_cast<qreal>(kTrackHeight + px(Spacing::S));
    return {centre - kHandleWidth / 2.0, track.center().y() - handleHeight / 2.0,
            static_cast<qreal>(kHandleWidth), handleHeight};
}

KRangeSelector::Handle KRangeSelector::handleAt(const QPointF& point) const {
    // The nearer handle wins a tie, so a press exactly between them grabs one deterministically
    // rather than depending on comparison order.
    const bool onLower = handleRect(Handle::Lower).adjusted(-4.0, -4.0, 4.0, 4.0).contains(point);
    const bool onUpper = handleRect(Handle::Upper).adjusted(-4.0, -4.0, 4.0, 4.0).contains(point);
    if (onLower && onUpper) {
        const qreal toLower = std::abs(point.x() - handleRect(Handle::Lower).center().x());
        const qreal toUpper = std::abs(point.x() - handleRect(Handle::Upper).center().x());
        return toLower <= toUpper ? Handle::Lower : Handle::Upper;
    }
    if (onLower) {
        return Handle::Lower;
    }
    if (onUpper) {
        return Handle::Upper;
    }
    return Handle::None;
}

double KRangeSelector::valueForPosition(const double x) const {
    const QRectF track = trackRect();
    if (track.width() <= 0.0) {
        return minimum_;
    }
    const double position = std::clamp((x - track.left()) / track.width(), 0.0, 1.0);
    return minimum_ + position * (maximum_ - minimum_);
}

KRangeSelector::Handle KRangeSelector::grabbedHandle() const noexcept { return grabbed_; }

State KRangeSelector::visualState() const {
    if (!isEnabled()) {
        return State::Disabled;
    }
    if (grabbed_ != Handle::None) {
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

QSize KRangeSelector::sizeHint() const {
    return {px(Size::ControlRoomy) * 5, px(Size::ControlCompact)};
}

QSize KRangeSelector::minimumSizeHint() const {
    return {kHandleWidth * 6, px(Size::ControlCompact)};
}

void KRangeSelector::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !isEnabled()) {
        QWidget::mousePressEvent(event);
        return;
    }
    grabbed_ = handleAt(event->position());
    if (grabbed_ == Handle::None) {
        // A press on bare track moves whichever handle is nearer, rather than doing nothing: the
        // artist pointed at a boundary they want moved.
        const double target = valueForPosition(event->position().x());
        grabbed_ =
            std::abs(target - lower_) <= std::abs(target - upper_) ? Handle::Lower : Handle::Upper;
        grabbed_ == Handle::Lower ? setLowerValue(target) : setUpperValue(target);
    }
    update();
    event->accept();
}

void KRangeSelector::mouseMoveEvent(QMouseEvent* event) {
    if (grabbed_ != Handle::None) {
        const double target = valueForPosition(event->position().x());
        grabbed_ == Handle::Lower ? setLowerValue(target) : setUpperValue(target);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void KRangeSelector::mouseReleaseEvent(QMouseEvent* event) {
    if (grabbed_ != Handle::None && event->button() == Qt::LeftButton) {
        grabbed_ = Handle::None;
        update();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void KRangeSelector::enterEvent(QEnterEvent* event) {
    if (isEnabled()) {
        hovered_ = true;
        update();
    }
    QWidget::enterEvent(event);
}

void KRangeSelector::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void KRangeSelector::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        hovered_ = false;
        grabbed_ = Handle::None;
    }
    QWidget::changeEvent(event);
}

void KRangeSelector::paintEvent(QPaintEvent* event) {
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
    const QRectF span = spanRect();
    if (span.width() > 0.0) {
        fillRoundedSurface(painter, span, accent, {}, Radius::Full);
    }

    QColor handleFill = color(Color::Foreground);
    if (state == State::Disabled) {
        handleFill = withOpacity(handleFill, kDisabledOpacity);
    }
    for (const Handle handle : {Handle::Lower, Handle::Upper}) {
        const QRectF bounds = handleRect(handle);
        fillRoundedSurface(painter, bounds, handleFill, color(borderForState(state)),
                           Radius::Small);
        if (hasFocus() && state != State::Disabled && handle == Handle::Lower) {
            drawFocusRing(painter, bounds, Radius::Small);
        }
    }
}

} // namespace bloom::ui::kit
