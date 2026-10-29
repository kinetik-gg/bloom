#include <bloom/ui/kit/switch_control.hpp>

#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>

#include <QEnterEvent>
#include <QEvent>
#include <QPainter>
#include <QFontMetrics>
#include <QPropertyAnimation>
#include <QSignalBlocker>

#include <algorithm>

namespace bloom::ui::kit {
namespace {

// The track is a pill two thumbs wide; the thumb is inset from it by a hairline's worth of air.
constexpr int kTrackHeight = px(Size::SwitchTrackHeight);
constexpr int kTrackWidth = px(Size::SwitchTrackWidth);
constexpr int kThumbInset = px(Spacing::XXS);

} // namespace

KSwitch::KSwitch(QWidget* parent) : QAbstractButton(parent) {
    setFixedHeight(px(Size::Control));
    ensureKeyboardFocusTracking(*this);
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
    // Hover before Selected: an "on" switch under the pointer still answers it (AccentHover).
    if (hovered_) {
        return State::Hover;
    }
    if (isChecked()) {
        return State::Selected;
    }
    if (hasKeyboardFocus(*this)) {
        return State::Focused;
    }
    return State::Normal;
}

Color KSwitch::borderToken() const {
    return borderForInteraction(isEnabled(), hasKeyboardFocus(*this), hovered_);
}

QSize KSwitch::sizeHint() const {
    return {kTrackWidth + px(Spacing::ControlInset) * 2, px(Size::Control)};
}

QSize KSwitch::minimumSizeHint() const { return sizeHint(); }

void KSwitch::animateThumbTo(const qreal target) {
    if (slide_ != nullptr)
        slide_->stop();
    const int duration = durationMs(Motion::Fast);
    if (duration <= 0) {
        // Reduced motion: the toggle still toggles, it just does not travel.
        setThumbPosition(target);
        return;
    }
    slide_ = new QPropertyAnimation(this, "thumbPosition", this);
    slide_->setDuration(duration);
    slide_->setEasingCurve(easing(Motion::Fast));
    slide_->setStartValue(thumbPosition_);
    slide_->setEndValue(target);
    slide_->start(QAbstractAnimation::DeleteWhenStopped);
}

void KSwitch::checkStateSet() {
    // A programmatic setChecked() is a projection, not a gesture: the thumb is simply there.
    if (slide_ != nullptr)
        slide_->stop();
    setThumbPosition(isChecked() ? 1.0 : 0.0);
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
    const Color borderRole = borderToken();
    // An "on" track is a filled accent pill with no outline at rest or on hover; focus is the one
    // state that still draws a border there, because it is the only border the rule asserts.
    const QColor trackBorder = (on && borderRole != Color::Accent) ? QColor{} : color(borderRole);
    fillRoundedSurface(painter, track, trackFill, trackBorder, Radius::Full);

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

KCheckBox::KCheckBox(QWidget* parent) : KCheckBox(QString{}, parent) {}

KCheckBox::KCheckBox(const QString& label, QWidget* parent) : KSwitch(parent) {
    setObjectName(QStringLiteral("kCheckBox"));
    setFont(kit::font(TypeRole::Ui));
    setLabel(label);
}

void KCheckBox::setLabel(const QString& label) {
    setText(label);
    setFixedSize(sizeHint());
    update();
}

void KCheckBox::setMixed(const bool mixed) {
    if (mixed_ == mixed)
        return;
    if (mixed) {
        // Underneath, a mixed box is unchecked, so the click that resolves it is a real toggle.
        const QSignalBlocker blocker(this);
        setChecked(false);
    }
    mixed_ = mixed;
    update();
}

void KCheckBox::nextCheckState() {
    mixed_ = false;
    KSwitch::nextCheckState();
}

void KCheckBox::checkStateSet() {
    mixed_ = false;
    KSwitch::checkStateSet();
}

QRectF KCheckBox::boxRect() const {
    const auto boxSize = static_cast<qreal>(px(Size::PropertiesCheckBox));
    const auto cell = static_cast<qreal>(px(Size::Control));
    return {(cell - boxSize) / 2.0, (height() - boxSize) / 2.0, boxSize, boxSize};
}

QSize KCheckBox::sizeHint() const {
    const int labelWidth =
        text().isEmpty() ? 0
                         : px(Spacing::XS) +
                               QFontMetrics(kit::font(TypeRole::Ui)).horizontalAdvance(text());
    return {px(Size::Control) + labelWidth, px(Size::Control)};
}

QSize KCheckBox::minimumSizeHint() const { return sizeHint(); }

void KCheckBox::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const auto state = visualState();
    const QRectF box = boxRect();
    const bool filled = isChecked() || mixed_;
    const QColor fill = color(filled ? Color::Accent : Color::Field);
    fillRoundedSurface(painter, box, isEnabled() ? fill : withOpacity(fill, kDisabledOpacity),
                       filled ? QColor{} : color(borderToken()), Radius::Small);
    // A filled box is Accent already, so its keyboard focus is a ring outside it.
    if (filled && hasKeyboardFocus(*this) && isEnabled())
        drawFocusRing(painter, box, Radius::Small);
    if (filled)
        painter.drawPixmap(box.topLeft(),
                           iconPixmap(mixed_ ? IconId::Subtract : IconId::Check, Size::IconSmall,
                                      inkForState(Color::Foreground, state), devicePixelRatioF(),
                                      iconWeight(IconRole::Chrome)));
    if (!text().isEmpty()) {
        painter.setFont(kit::font(TypeRole::Ui));
        painter.setPen(inkForState(Color::Foreground, isEnabled() ? State::Normal : state));
        painter.drawText(QRectF(px(Size::Control) + px(Spacing::XS), 0.0,
                                width() - px(Size::Control) - px(Spacing::XS), height()),
                         Qt::AlignVCenter | Qt::AlignLeft, text());
    }
}

} // namespace bloom::ui::kit
