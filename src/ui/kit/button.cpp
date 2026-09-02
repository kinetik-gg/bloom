#include <bloom/ui/kit/button.hpp>

#include <bloom/ui/kit/painting.hpp>

#include <QEnterEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QPainter>
#include <QPixmap>
#include <QRectF>

#include <algorithm>
#include <utility>

namespace bloom::ui::kit {
namespace {

// The surface a variant rests on, before any state step. Ghost and Danger share Surface as their
// resting rung: neither draws a raised plate at rest, and both step up from the panel surface.
[[nodiscard]] Color restingSurface(const KButton::Variant variant) {
    switch (variant) {
    case KButton::Variant::Primary:
        return Color::Accent;
    case KButton::Variant::Secondary:
        return Color::SurfaceRaised;
    case KButton::Variant::Ghost:
    case KButton::Variant::Danger:
        return Color::Surface;
    }
    return Color::SurfaceRaised;
}

[[nodiscard]] Color restingInk(const KButton::Variant variant) {
    switch (variant) {
    case KButton::Variant::Primary:
    case KButton::Variant::Secondary:
        return Color::Foreground;
    case KButton::Variant::Ghost:
        return Color::Muted;
    case KButton::Variant::Danger:
        return Color::Error;
    }
    return Color::Foreground;
}

} // namespace

KButton::KButton(QWidget* parent) : QAbstractButton(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
    setFont(kit::font(TypeRole::Ui));
}

KButton::KButton(const QString& text, QWidget* parent) : KButton(parent) { setText(text); }

KButton::KButton(const IconId icon, const QString& text, QWidget* parent) : KButton(text, parent) {
    icon_ = icon;
}

void KButton::setVariant(const Variant variant) {
    if (variant_ == variant) {
        return;
    }
    variant_ = variant;
    update();
}

KButton::Variant KButton::variant() const noexcept { return variant_; }

void KButton::setControlSize(const ControlSize size) {
    if (controlSize_ == size) {
        return;
    }
    controlSize_ = size;
    updateGeometry();
    update();
}

KButton::ControlSize KButton::controlSize() const noexcept { return controlSize_; }

void KButton::setIconId(std::optional<IconId> icon) {
    icon_ = icon;
    updateGeometry();
    update();
}

std::optional<IconId> KButton::iconId() const noexcept { return icon_; }

void KButton::setDangerOnHover(const bool dangerOnHover) {
    if (dangerOnHover_ == dangerOnHover) {
        return;
    }
    dangerOnHover_ = dangerOnHover;
    update();
}

bool KButton::dangerOnHover() const noexcept { return dangerOnHover_; }

bool KButton::paintsAsDanger(const State state) const noexcept {
    if (variant_ == Variant::Danger) {
        return true;
    }
    return variant_ == Variant::Ghost && dangerOnHover_ &&
           (state == State::Hover || state == State::Pressed || state == State::Selected);
}

State KButton::visualState() const {
    // One rule, consulted by both painting and tests. Order matters: disabled outranks everything,
    // because a disabled control has no hover and no press response at all.
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

QColor KButton::fillForState(const State state) const {
    const bool filled = variant_ == Variant::Primary || paintsAsDanger(state);

    if (variant_ == Variant::Primary) {
        // The accent triple states the recipe outright; nothing derived is needed.
        switch (state) {
        case State::Hover:
            return color(Color::AccentHover);
        case State::Pressed:
            return color(Color::AccentPressed);
        case State::Disabled:
            return withOpacity(color(Color::Accent), kDisabledOpacity);
        case State::Normal:
        case State::Focused:
        case State::Selected:
            return color(Color::Accent);
        }
    }
    if (filled) {
        // Danger, once committed (or Ghost with dangerOnHover, once hovered/pressed): the same
        // rest/hover/press relation the accent triple states, applied to Error.
        const QColor base = color(Color::Error);
        return state == State::Pressed ? pressedFillFor(base) : hoverFillFor(base);
    }
    if (variant_ == Variant::Ghost &&
        (state == State::Normal || state == State::Focused || state == State::Disabled)) {
        // Chrome weight: no surface of its own at rest.
        return {};
    }
    return color(surfaceForState(restingSurface(variant_), state));
}

QColor KButton::inkForVisualState(const State state) const {
    if (variant_ == Variant::Primary) {
        return state == State::Disabled ? withOpacity(color(Color::Foreground), kDisabledOpacity)
                                        : color(Color::Foreground);
    }
    if (variant_ == Variant::Danger || (variant_ == Variant::Ghost && dangerOnHover_)) {
        switch (state) {
        case State::Hover:
        case State::Pressed:
        case State::Selected:
            return color(Color::Foreground);
        case State::Disabled:
            return variant_ == Variant::Danger
                       ? withOpacity(color(Color::Error), kDisabledOpacity)
                       : inkForState(restingInk(variant_), state);
        case State::Normal:
        case State::Focused:
            return variant_ == Variant::Danger ? color(Color::Error)
                                                : inkForState(restingInk(variant_), state);
        }
    }
    return inkForState(restingInk(variant_), state);
}

int KButton::controlExtent() const {
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

Size KButton::iconBox() const {
    switch (controlSize_) {
    case ControlSize::Compact:
        return Size::IconSmall;
    case ControlSize::Default:
        return Size::IconMedium;
    case ControlSize::Roomy:
        return Size::IconLarge;
    }
    return Size::IconMedium;
}

QSize KButton::sizeHint() const {
    const QFontMetrics metrics(font());
    int width = px(Spacing::M) * 2;
    if (icon_.has_value()) {
        width += px(iconBox());
        if (!text().isEmpty()) {
            width += px(Spacing::S);
        }
    }
    if (!text().isEmpty()) {
        width += metrics.horizontalAdvance(text());
    }
    // The focus ring is drawn outside the control rectangle, so the widget reserves room for it on
    // every side and focusing never shifts a layout.
    const auto ringMargin = static_cast<int>(std::lround(kFocusRingWidth)) * 2;
    return {std::max(width, controlExtent()) + ringMargin, controlExtent() + ringMargin};
}

QSize KButton::minimumSizeHint() const {
    const auto ringMargin = static_cast<int>(std::lround(kFocusRingWidth)) * 2;
    return {controlExtent() + ringMargin, controlExtent() + ringMargin};
}

void KButton::enterEvent(QEnterEvent* event) {
    if (isEnabled()) {
        hovered_ = true;
        update();
    }
    QAbstractButton::enterEvent(event);
}

void KButton::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void KButton::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        // Losing enablement while hovered must not leave a stale hover behind.
        hovered_ = false;
    }
    QAbstractButton::changeEvent(event);
}

void KButton::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    const State state = visualState();
    const auto ringMargin = kFocusRingWidth;
    const QRectF bounds = QRectF(rect()).adjusted(ringMargin, ringMargin, -ringMargin, -ringMargin);

    const QColor fill = fillForState(state);
    QColor border = color(borderForState(state));
    if (variant_ == Variant::Primary || paintsAsDanger(state)) {
        border = fill;
    } else if (variant_ == Variant::Danger &&
               (state == State::Normal || state == State::Focused || state == State::Disabled)) {
        border = inkForVisualState(state);
    } else if (variant_ == Variant::Ghost && state == State::Normal) {
        border = {};
    }
    fillRoundedSurface(painter, bounds, fill, border, Radius::Small);

    if (state == State::Focused || (hasFocus() && state != State::Disabled)) {
        // Always visible for keyboard focus, and always outside the control's own rectangle.
        drawFocusRing(painter, bounds, Radius::Small);
    }

    const QColor ink = inkForVisualState(state);
    QRectF content = bounds.adjusted(px(Spacing::M), 0.0, -px(Spacing::M), 0.0);
    if (icon_.has_value()) {
        // The icon takes exactly the ink the label does, so a button reads as one object in every
        // state rather than a glyph and a word fading at different rates.
        const QPixmap pixmap = iconPixmap(*icon_, iconBox(), ink, devicePixelRatioF());
        const auto box = static_cast<qreal>(px(iconBox()));
        const QRectF iconRect(content.left(), content.center().y() - box / 2.0, box, box);
        painter.drawPixmap(iconRect.toRect(), pixmap);
        content.setLeft(iconRect.right() + (text().isEmpty() ? 0.0 : px(Spacing::S)));
    }

    if (!text().isEmpty()) {
        painter.setPen(ink);
        painter.setFont(font());
        const QFontMetrics metrics(font());
        const QString visible =
            metrics.elidedText(text(), Qt::ElideRight, static_cast<int>(content.width()));
        painter.drawText(content, Qt::AlignVCenter | Qt::AlignHCenter, visible);
    }
}

} // namespace bloom::ui::kit
