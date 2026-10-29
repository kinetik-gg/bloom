#include <bloom/ui/kit/range_selector.hpp>

#include <bloom/ui/kit/accessibility.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>

#include <QAccessible>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QSignalBlocker>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace bloom::ui::kit {
namespace {

constexpr int kTrackHeight = px(Size::RangeTrack);
constexpr int kHandleWidth = px(Size::RangeHandle);
// How far outside its painted body a handle still takes a press.
constexpr qreal kHandleSlop = px(Spacing::XS);

} // namespace

KRangeSelector::KRangeSelector(QWidget* parent) : QWidget(parent) {
    installKitAccessibility();
    ensureKeyboardFocusTracking(*this);
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
    const auto slop = [this](const Handle handle) {
        return handleRect(handle).adjusted(-kHandleSlop, -kHandleSlop, kHandleSlop, kHandleSlop);
    };
    const bool onLower = slop(Handle::Lower).contains(point);
    const bool onUpper = slop(Handle::Upper).contains(point);
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
    if (hasKeyboardFocus(*this)) {
        return State::Focused;
    }
    return State::Normal;
}

QSize KRangeSelector::sizeHint() const { return {px(Size::ControlRoomy) * 5, px(Size::Control)}; }

QSize KRangeSelector::minimumSizeHint() const { return {kHandleWidth * 6, px(Size::Control)}; }

void KRangeSelector::beginInteraction() {
    if (interactionActive_)
        return;
    interactionActive_ = true;
    lowerBase_ = lower_;
    upperBase_ = upper_;
    Q_EMIT editStarted();
}

void KRangeSelector::finishInteraction(const bool keep) {
    if (!interactionActive_)
        return;
    interactionActive_ = false;
    grabbed_ = Handle::None;
    undecided_ = false;
    keyStepping_ = false;
    update();
    if (keep) {
        Q_EMIT editFinished();
        return;
    }
    {
        const QSignalBlocker blocker(this);
        setValues(lowerBase_, upperBase_);
    }
    Q_EMIT editCancelled();
}

void KRangeSelector::cancelEdit() { finishInteraction(false); }

void KRangeSelector::moveHandle(const Handle handle, const double value) {
    if (handle == Handle::Lower)
        setLowerValue(value);
    else if (handle == Handle::Upper)
        setUpperValue(value);
    if (QAccessible::isActive()) {
        QAccessibleValueChangeEvent change(this, handle == Handle::Upper ? upper_ : lower_);
        QAccessible::updateAccessibility(&change);
    }
}

void KRangeSelector::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !isEnabled()) {
        QWidget::mousePressEvent(event);
        return;
    }
    beginInteraction();
    grabbed_ = handleAt(event->position());
    pressX_ = event->position().x();
    // Handles on top of each other: either could be meant, and only the first move says which.
    undecided_ = grabbed_ != Handle::None && handleRect(Handle::Lower).center().x() ==
                                                 handleRect(Handle::Upper).center().x();
    if (grabbed_ == Handle::None) {
        // A press on bare track moves whichever handle is nearer, rather than doing nothing: the
        // artist pointed at a boundary they want moved.
        const double target = valueForPosition(event->position().x());
        grabbed_ =
            std::abs(target - lower_) <= std::abs(target - upper_) ? Handle::Lower : Handle::Upper;
        moveHandle(grabbed_, target);
    }
    active_ = grabbed_;
    update();
    event->accept();
}

void KRangeSelector::mouseMoveEvent(QMouseEvent* event) {
    if (grabbed_ != Handle::None) {
        if (undecided_) {
            if (event->position().x() == pressX_) {
                event->accept();
                return;
            }
            grabbed_ = event->position().x() < pressX_ ? Handle::Lower : Handle::Upper;
            active_ = grabbed_;
            undecided_ = false;
        }
        moveHandle(grabbed_, valueForPosition(event->position().x()));
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void KRangeSelector::mouseReleaseEvent(QMouseEvent* event) {
    if (grabbed_ != Handle::None && event->button() == Qt::LeftButton) {
        finishInteraction(true);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void KRangeSelector::keyPressEvent(QKeyEvent* event) {
    const double span = maximum_ - minimum_;
    const double current = active_ == Handle::Upper ? upper_ : lower_;
    std::optional<double> target;
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Down:
        target = current - span * kKeyStepFraction;
        break;
    case Qt::Key_Right:
    case Qt::Key_Up:
        target = current + span * kKeyStepFraction;
        break;
    case Qt::Key_PageDown:
        target = current - span * kKeyStepFraction * kCoarseStepMultiple;
        break;
    case Qt::Key_PageUp:
        target = current + span * kKeyStepFraction * kCoarseStepMultiple;
        break;
    case Qt::Key_Home:
        target = minimum_;
        break;
    case Qt::Key_End:
        target = maximum_;
        break;
    case Qt::Key_Escape:
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
        if (!interactionActive_) {
            beginInteraction();
            keyStepping_ = true;
        }
        moveHandle(active_, *target);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void KRangeSelector::keyReleaseEvent(QKeyEvent* event) {
    if (keyStepping_ && !event->isAutoRepeat() && event->key() != Qt::Key_Tab &&
        event->key() != Qt::Key_Backtab && event->key() != Qt::Key_Shift) {
        finishInteraction(true);
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void KRangeSelector::focusInEvent(QFocusEvent* event) {
    // Shift+Tab arrives on the Upper handle, as it would on the last of two separate controls.
    if (event->reason() == Qt::TabFocusReason)
        active_ = Handle::Lower;
    else if (event->reason() == Qt::BacktabFocusReason)
        active_ = Handle::Upper;
    update();
    QWidget::focusInEvent(event);
}

void KRangeSelector::focusOutEvent(QFocusEvent* event) {
    if (keyStepping_)
        finishInteraction(true);
    QWidget::focusOutEvent(event);
}

bool KRangeSelector::focusNextPrevChild(const bool next) {
    // Tab walks the two handles before it leaves the control.
    if (next && active_ == Handle::Lower) {
        active_ = Handle::Upper;
        update();
        return true;
    }
    if (!next && active_ == Handle::Upper) {
        active_ = Handle::Lower;
        update();
        return true;
    }
    return QWidget::focusNextPrevChild(next);
}

bool KRangeSelector::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::ShortcutOverride: {
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
    case QEvent::KeyPress: {
        // Tab between the handles is the control's own, so it never reaches the window's chain.
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) &&
            !key->modifiers().testAnyFlags(Qt::ControlModifier | Qt::AltModifier)) {
            const bool next = key->key() == Qt::Key_Tab &&
                              !key->modifiers().testFlag(Qt::ShiftModifier);
            if ((next && active_ == Handle::Lower) || (!next && active_ == Handle::Upper)) {
                focusNextPrevChild(next);
                return true;
            }
        }
        break;
    }
    case QEvent::UngrabMouse:
    case QEvent::WindowDeactivate:
        if (grabbed_ != Handle::None || (event->type() == QEvent::WindowDeactivate && keyStepping_))
            finishInteraction(true);
        break;
    default:
        break;
    }
    return QWidget::event(event);
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
        finishInteraction(true);
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
        if (hasKeyboardFocus(*this) && state != State::Disabled && handle == active_) {
            drawFocusRing(painter, bounds, Radius::Small);
        }
    }
}

} // namespace bloom::ui::kit
