#include <bloom/ui/kit/switch_control.hpp>

#include <bloom/ui/kit/painting.hpp>

#include <QEnterEvent>
#include <QEvent>
#include <QPainter>
#include <QPropertyAnimation>

#include <algorithm>

namespace bloom::ui::kit {
namespace {

// The track is a pill two thumbs wide; the thumb is inset from it by a hairline's worth of air.
constexpr int kTrackHeight = 16;
constexpr int kTrackWidth = 30;
constexpr int kThumbInset = 2;

} // namespace

KSwitch::KSwitch(QWidget* parent) : QAbstractButton(parent) {
    setObjectName(QStringLiteral("kSwitch"));
    setCheckable(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
}

qreal KSwitch::thumbPosition() const noexcept { return thumbPosition_; }

void KSwitch::setThumbPosition(const qreal position) {
    const qreal clamped = std::clamp(position, 0.0, 1.0);
    if (qFuzzyCompare(thumbPosition_ + 1.0, clamped + 1.0)) {
        return;
    }
    thumbPosition_ = clamped;
    update();
}

State KSwitch::visualState() const {
    if (!isEnabled()) {
        return State::Disabled;
    }
    if (isDown()) {
        return State::Pressed;
    }
    if (isChecked()) {
        return State::Selected;
    }
    if (hovered_) {
        return State::Hover;
    }
    if (hasFocus()) {
        return State::Focused;
    }
    return State::Normal;
}

QSize KSwitch::sizeHint() const {
    const auto ringMargin = static_cast<int>(std::lround(kFocusRingWidth)) * 2;
    return {kTrackWidth + ringMargin,
            std::max(kTrackHeight, px(Size::ControlCompact)) + ringMargin};
}

QSize KSwitch::minimumSizeHint() const { return sizeHint(); }

void KSwitch::animateThumbTo(const qreal target) {
    const int duration = durationMs(Motion::Fast);
    if (duration <= 0) {
        // Reduced motion: the toggle still toggles, it just does not travel.
        setThumbPosition(target);
        return;
    }
    auto* slide = new QPropertyAnimation(this, "thumbPosition", this);
    slide->setDuration(duration);
    slide->setEasingCurve(easing(Motion::Fast));
    slide->setStartValue(thumbPosition_);
    slide->setEndValue(target);
    slide->start(QAbstractAnimation::DeleteWhenStopped);
}

void KSwitch::checkStateSet() {
    animateThumbTo(isChecked() ? 1.0 : 0.0);
    QAbstractButton::checkStateSet();
}

void KSwitch::nextCheckState() {
    QAbstractButton::nextCheckState();
    animateThumbTo(isChecked() ? 1.0 : 0.0);
}

void KSwitch::enterEvent(QEnterEvent* event) {
    if (isEnabled()) {
        hovered_ = true;
        update();
    }
    QAbstractButton::enterEvent(event);
}

void KSwitch::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void KSwitch::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        hovered_ = false;
    }
    QAbstractButton::changeEvent(event);
}

void KSwitch::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    const State state = visualState();

    const QRectF track(QRectF(rect()).center().x() - kTrackWidth / 2.0,
                       QRectF(rect()).center().y() - kTrackHeight / 2.0, kTrackWidth, kTrackHeight);

    // Off is a field with a hairline; on is an accent fill. The thumb keeps full-strength ink in
    // both, so the switch never communicates its state by color alone -- the thumb's position does.
    const bool on = thumbPosition_ > 0.5;
    QColor trackFill = on ? color(Color::Accent) : color(surfaceForState(Color::Field, state));
    if (on && state == State::Hover) {
        trackFill = color(Color::AccentHover);
    } else if (on && state == State::Pressed) {
        trackFill = color(Color::AccentPressed);
    }
    if (state == State::Disabled) {
        trackFill = withOpacity(trackFill, kDisabledOpacity);
    }
    const QColor trackBorder = on ? QColor{} : color(borderForState(state));
    fillRoundedSurface(painter, track, trackFill, trackBorder, Radius::Full);

    if (hasFocus() && state != State::Disabled) {
        drawFocusRing(painter, track, Radius::Full);
    }

    const auto thumbDiameter = static_cast<qreal>(kTrackHeight - kThumbInset * 2);
    const qreal travel = track.width() - thumbDiameter - kThumbInset * 2;
    const QRectF thumb(track.left() + kThumbInset + travel * thumbPosition_,
                       track.top() + kThumbInset, thumbDiameter, thumbDiameter);
    QColor thumbFill = color(Color::Foreground);
    if (state == State::Disabled) {
        thumbFill = withOpacity(thumbFill, kDisabledOpacity);
    }
    fillRoundedSurface(painter, thumb, thumbFill, {}, Radius::Full);
}

} // namespace bloom::ui::kit
