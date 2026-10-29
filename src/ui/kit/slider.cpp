#include <bloom/ui/kit/slider.hpp>

#include <bloom/ui/kit/accessibility.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>

#include <QEnterEvent>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QAccessible>
#include <QFocusEvent>
#include <QPainter>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace bloom::ui::kit {
namespace {

constexpr int kTrackHeight = px(Size::SliderTrack);
constexpr int kHandleDiameter = px(Size::SliderHandle);

} // namespace

KSlider::KSlider(QWidget* parent) : QWidget(parent) {
    installKitAccessibility();
    setFixedHeight(px(Size::Control));
    ensureKeyboardFocusTracking(*this);
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

void KSlider::setValue(const double value) {
    // A drag owns the handle until it ends, as a scrub owns a value field.
    if (dragging_)
        return;
    commitValue(value);
}

void KSlider::commitValue(const double value) {
    if (!std::isfinite(value))
        return;
    const double clamped = std::clamp(value, minimum_, maximum_);
    if (clamped == value_) {
        return;
    }
    value_ = clamped;
    update();
    if (QAccessible::isActive()) {
        QAccessibleValueChangeEvent change(this, value_);
        QAccessible::updateAccessibility(&change);
    }
    Q_EMIT valueChanged(value_);
}

void KSlider::beginInteraction() {
    if (interactionActive_)
        return;
    interactionActive_ = true;
    interactionBase_ = value_;
    Q_EMIT editStarted();
}

void KSlider::finishInteraction(const bool keep) {
    if (!interactionActive_)
        return;
    interactionActive_ = false;
    dragging_ = false;
    keyStepping_ = false;
    update();
    if (keep) {
        Q_EMIT editFinished();
        return;
    }
    {
        const QSignalBlocker blocker(this);
        commitValue(interactionBase_);
    }
    Q_EMIT editCancelled();
}

void KSlider::cancelEdit() { finishInteraction(false); }

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
    if (hasKeyboardFocus(*this)) {
        return State::Focused;
    }
    return State::Normal;
}

Color KSlider::borderToken() const {
    return borderForInteraction(isEnabled(), hasKeyboardFocus(*this), hovered_);
}

bool KSlider::isDragging() const noexcept { return dragging_; }

QSize KSlider::sizeHint() const { return {px(Size::ControlRoomy) * 4, px(Size::Control)}; }

QSize KSlider::minimumSizeHint() const { return {kHandleDiameter * 3, px(Size::Control)}; }

void KSlider::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !isEnabled()) {
        QWidget::mousePressEvent(event);
        return;
    }
    beginInteraction();
    dragging_ = true;
    const QRectF handle = handleRect();
    const qreal x = event->position().x();
    if (x >= handle.left() && x <= handle.right()) {
        // Pressing the handle grabs it where it was pressed: nothing moves until the pointer does.
        grabOffset_ = x - handle.center().x();
    } else {
        // Pressing the bare track takes the value there immediately rather than nudging toward
        // it: the artist asked for that value by pointing at it.
        grabOffset_ = 0.0;
        commitValue(valueForPosition(x));
    }
    update();
    event->accept();
}

void KSlider::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_) {
        commitValue(valueForPosition(event->position().x() - grabOffset_));
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void KSlider::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_ && event->button() == Qt::LeftButton) {
        finishInteraction(true);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void KSlider::keyPressEvent(QKeyEvent* event) {
    const double span = maximum_ - minimum_;
    std::optional<double> target;
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Down:
        target = value_ - span * kKeyStepFraction;
        break;
    case Qt::Key_Right:
    case Qt::Key_Up:
        target = value_ + span * kKeyStepFraction;
        break;
    case Qt::Key_PageDown:
        target = value_ - span * kKeyStepFraction * kCoarseStepMultiple;
        break;
    case Qt::Key_PageUp:
        target = value_ + span * kKeyStepFraction * kCoarseStepMultiple;
        break;
    case Qt::Key_Home:
        target = minimum_;
        break;
    case Qt::Key_End:
        target = maximum_;
        break;
    case Qt::Key_Escape:
        // Escape confirms a key edit in progress; at rest it belongs to whatever is around.
        if (keyStepping_) {
            finishInteraction(true);
            event->accept();
            return;
        }
        break;
    default:
        break;
    }
    if (target.has_value() && isEnabled()) {
        // Held or repeated keys are one edit, ending on the release.
        if (!interactionActive_) {
            beginInteraction();
            keyStepping_ = true;
        }
        commitValue(*target);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void KSlider::keyReleaseEvent(QKeyEvent* event) {
    if (keyStepping_ && !event->isAutoRepeat()) {
        switch (event->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_Home:
        case Qt::Key_End:
            finishInteraction(true);
            event->accept();
            return;
        default:
            break;
        }
    }
    QWidget::keyReleaseEvent(event);
}

void KSlider::focusOutEvent(QFocusEvent* event) {
    if (keyStepping_)
        finishInteraction(true);
    QWidget::focusOutEvent(event);
}

bool KSlider::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::ShortcutOverride: {
        // A focused slider steps with the arrows, so the frame-step shortcuts stand down.
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (hasFocus() && (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up ||
                           key == Qt::Key_Down || key == Qt::Key_PageUp ||
                           key == Qt::Key_PageDown || key == Qt::Key_Home || key == Qt::Key_End ||
                           (key == Qt::Key_Escape && keyStepping_))) {
            event->accept();
            return true;
        }
        break;
    }
    case QEvent::UngrabMouse:
    case QEvent::WindowDeactivate:
        // The drag ends where it is, as a commit, when the pointer or the window goes away.
        if (dragging_ || (event->type() == QEvent::WindowDeactivate && keyStepping_))
            finishInteraction(true);
        break;
    default:
        break;
    }
    return QWidget::event(event);
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
        finishInteraction(true);
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

    QColor handleFill = color(Color::Foreground);
    if (state == State::Disabled) {
        handleFill = withOpacity(handleFill, kDisabledOpacity);
    }
    // The handle's own single border carries focus; the ring outside it is gone.
    fillRoundedSurface(painter, handleRect(), handleFill, color(borderToken()), Radius::Full);
}

} // namespace bloom::ui::kit
