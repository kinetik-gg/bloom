#include <bloom/ui/kit/painting.hpp>

#include <QGraphicsDropShadowEffect>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cmath>

namespace bloom::ui::kit {
namespace {

[[nodiscard]] qreal channelBlendTowards(const int from, const int to, const int target) {
    const int span = target - from;
    if (span == 0) {
        return 0.0;
    }
    return static_cast<qreal>(to - from) / static_cast<qreal>(span);
}

[[nodiscard]] int blendChannel(const int from, const int target, const qreal amount) {
    const auto blended =
        static_cast<qreal>(from) + amount * (static_cast<qreal>(target) - static_cast<qreal>(from));
    return std::clamp(static_cast<int>(std::lround(blended)), 0, 255);
}

[[nodiscard]] int scaleChannel(const int value, const qreal scale) {
    return std::clamp(static_cast<int>(std::lround(static_cast<qreal>(value) * scale)), 0, 255);
}

[[nodiscard]] qreal meanOfChannels(const std::array<qreal, 3>& values) {
    return (values[0] + values[1] + values[2]) / 3.0;
}

} // namespace

qreal filledHoverBlend() {
    const QColor accent = color(Color::Accent);
    const QColor hovered = color(Color::AccentHover);
    const QColor ink = color(Color::Foreground);
    return meanOfChannels({channelBlendTowards(accent.red(), hovered.red(), ink.red()),
                           channelBlendTowards(accent.green(), hovered.green(), ink.green()),
                           channelBlendTowards(accent.blue(), hovered.blue(), ink.blue())});
}

qreal filledPressedScale() {
    const QColor accent = color(Color::Accent);
    const QColor pressed = color(Color::AccentPressed);
    const auto ratio = [](const int from, const int to) {
        return from == 0 ? 1.0 : static_cast<qreal>(to) / static_cast<qreal>(from);
    };
    return meanOfChannels({ratio(accent.red(), pressed.red()),
                           ratio(accent.green(), pressed.green()),
                           ratio(accent.blue(), pressed.blue())});
}

QColor hoverFillFor(const QColor& resting) {
    const QColor ink = color(Color::Foreground);
    const qreal amount = filledHoverBlend();
    QColor result(blendChannel(resting.red(), ink.red(), amount),
                  blendChannel(resting.green(), ink.green(), amount),
                  blendChannel(resting.blue(), ink.blue(), amount));
    result.setAlpha(resting.alpha());
    return result;
}

QColor pressedFillFor(const QColor& resting) {
    const qreal scale = filledPressedScale();
    QColor result(scaleChannel(resting.red(), scale), scaleChannel(resting.green(), scale),
                  scaleChannel(resting.blue(), scale));
    result.setAlpha(resting.alpha());
    return result;
}

Color surfaceForState(const Color resting, const State state) {
    switch (state) {
    case State::Hover:
        return surfaceStep(resting, 1);
    case State::Pressed:
        return surfaceStep(resting, -1);
    case State::Normal:
    case State::Focused:
    case State::Selected:
    case State::Disabled:
        // A disabled control does not move: it has no hover response at all.
        return resting;
    }
    return resting;
}

Color borderForState(const State state) {
    switch (state) {
    case State::Hover:
    case State::Pressed:
        return Color::BorderHover;
    case State::Selected:
    case State::Focused:
        return Color::Accent;
    case State::Normal:
    case State::Disabled:
        return Color::Border;
    }
    return Color::Border;
}

QColor inkForState(const Color resting, const State state) {
    switch (state) {
    case State::Hover:
    case State::Pressed:
    case State::Selected:
        return color(Color::Foreground);
    case State::Disabled:
        return withOpacity(color(resting), kDisabledOpacity);
    case State::Normal:
    case State::Focused:
        return color(resting);
    }
    return color(resting);
}

void applyHairlinePen(QPainter& painter, const QColor& value) {
    QPen pen(value);
    pen.setWidthF(snappedHairlineWidth(painter.device()->devicePixelRatio()));
    pen.setCosmetic(false);
    painter.setPen(pen);
}

void fillRoundedSurface(QPainter& painter, const QRectF& bounds, const QColor& fill,
                        const QColor& border, const Radius radius) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto extent = static_cast<int>(std::lround(std::min(bounds.width(), bounds.height())));
    const auto corner = static_cast<qreal>(radiusPx(radius, extent));

    if (border.isValid() && border.alpha() > 0) {
        applyHairlinePen(painter, border);
        const qreal inset = painter.pen().widthF() / 2.0;
        painter.setBrush(fill.isValid() ? QBrush(fill) : Qt::NoBrush);
        painter.drawRoundedRect(bounds.adjusted(inset, inset, -inset, -inset), corner, corner);
    } else if (fill.isValid()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(bounds, corner, corner);
    }
    painter.restore();
}

void drawFocusRing(QPainter& painter, const QRectF& bounds, const Radius radius) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color(Color::Accent));
    pen.setWidthF(kFocusRingWidth);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    // Outside the control's own rectangle: the ring is drawn in the margin the widget already
    // reserved, so focus never resizes anything.
    const qreal offset = kFocusRingWidth;
    const QRectF ring = bounds.adjusted(-offset, -offset, offset, offset);
    const auto extent = static_cast<int>(std::lround(std::min(ring.width(), ring.height())));
    const auto corner = static_cast<qreal>(radiusPx(radius, extent)) + offset;
    painter.drawRoundedRect(ring, corner, corner);
    painter.restore();
}

void applyElevation(QWidget& widget, const Elevation elevation) {
    const Shadow token = shadow(elevation);
    if (token.isFlat()) {
        widget.setGraphicsEffect(nullptr);
        return;
    }
    auto* effect = new QGraphicsDropShadowEffect(&widget);
    effect->setOffset(token.offsetX, token.offsetY);
    effect->setBlurRadius(token.blurRadius);
    effect->setColor(token.color);
    widget.setGraphicsEffect(effect);
}

} // namespace bloom::ui::kit
