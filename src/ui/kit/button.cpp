#include <bloom/ui/kit/button.hpp>

#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>

#include <QEnterEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QPainter>
#include <cmath>
#include <QPixmap>
#include <QRectF>

#include <algorithm>
#include <utility>

namespace bloom::ui::kit {

KButton::KButton(QWidget* parent) : QAbstractButton(parent) {
    ensureKeyboardFocusTracking(*this);
    setFocusPolicy(Qt::StrongFocus);
    setFixedHeight(controlExtent());
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
    setFont(kit::font(TypeRole::Ui));
}

ButtonKind KButton::buttonKind() const noexcept {
    switch (variant_) {
    case Variant::Primary:
        return ButtonKind::Primary;
    case Variant::Secondary:
        return ButtonKind::Secondary;
    case Variant::Ghost:
        return dangerOnHover_ ? ButtonKind::GhostDanger : ButtonKind::Ghost;
    case Variant::Danger:
        return ButtonKind::Danger;
    }
    return ButtonKind::Secondary;
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
    setFixedHeight(controlExtent());
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
    if (hasKeyboardFocus(*this)) {
        return State::Focused;
    }
    return State::Normal;
}

Color KButton::borderToken() const {
    return borderForInteraction(isEnabled(), hasKeyboardFocus(*this), hovered_);
}

QColor KButton::fillForState(const State state) const {
    return buttonColors(buttonKind(), state, false).fill;
}

QColor KButton::inkForVisualState(const State state) const {
    return buttonColors(buttonKind(), state, false).ink;
}

int KButton::controlExtent() const {
    switch (controlSize_) {
    case ControlSize::Compact:
        return px(Size::Control);
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
    // task U8, issue #131, fix 7: a button with an icon and no text sizes to controlExtent x
    // controlExtent exactly -- the Spacing::M side padding below is a TEXT button's own margin
    // recipe and does not apply once there is no label to pad.
    if (icon_.has_value() && text().isEmpty()) {
        return {controlExtent(), controlExtent()};
    }

    int width = px(Spacing::M) * 2 + px(Spacing::ControlInset) * 2;
    if (icon_.has_value()) {
        width += px(iconBox());
        if (!text().isEmpty()) {
            width += px(Spacing::S);
        }
    }
    if (!text().isEmpty()) {
        // The painter lays glyphs out at fractional advances, so the label's integer advance can
        // fall a fraction short at a fractional device pixel ratio and elide its last letter.
        width += static_cast<int>(std::ceil(QFontMetricsF(font()).horizontalAdvance(text()))) + 1;
    }
    return {std::max(width, controlExtent()), controlExtent()};
}

QSize KButton::minimumSizeHint() const { return {controlExtent(), controlExtent()}; }

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
    if (event->type() == QEvent::EnabledChange) {
        // Losing enablement while hovered must not leave a stale hover behind, and a disabled
        // button does not invite a click.
        if (!isEnabled())
            hovered_ = false;
        setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
    QAbstractButton::changeEvent(event);
}

void KButton::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    const State state = visualState();
    const QRectF bounds = controlBounds(*this);
    const ButtonColors colors = buttonColors(buttonKind(), state, hasKeyboardFocus(*this));
    fillRoundedSurface(painter, bounds, colors.fill, colors.border, Radius::Small);

    const QColor ink = colors.ink;
    QRectF content = bounds.adjusted(px(Spacing::M), 0.0, -px(Spacing::M), 0.0);
    if (icon_.has_value()) {
        // The icon takes exactly the ink the label does, so a button reads as one object in every
        // state rather than a glyph and a word fading at different rates.
        const QPixmap pixmap =
            iconPixmap(*icon_, iconBox(), ink, devicePixelRatioF(), iconWeight(IconRole::Chrome));
        const auto box = static_cast<qreal>(px(iconBox()));
        // task U8, issue #131, fix 7: an icon-only button (no text) is a controlExtent square with
        // no Spacing::M side padding at all (sizeHint() above matches), so its icon centers in the
        // WHOLE bounds rather than starting from that text button's left inset.
        const QRectF iconRect =
            text().isEmpty()
                ? QRectF(bounds.center().x() - box / 2.0, bounds.center().y() - box / 2.0, box, box)
                : QRectF(content.left(), content.center().y() - box / 2.0, box, box);
        painter.drawPixmap(snappedToDevicePixels(painter, iconRect).topLeft(), pixmap);
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
