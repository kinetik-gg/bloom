#include <bloom/ui/kit/painting.hpp>

#include <QGraphicsDropShadowEffect>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QTransform>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

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

QColor edgeInkFor(const QColor& fill) { return pressedFillFor(pressedFillFor(fill)); }

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

Color borderForInteraction(const bool enabled, const bool active, const bool hovered) {
    if (!enabled) {
        return Color::Border;
    }
    // Focus wins: checked before hover, never after.
    if (active) {
        return Color::Accent;
    }
    if (hovered) {
        return Color::BorderHover;
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
        return disabledInk(color(resting));
    case State::Normal:
    case State::Focused:
        return color(resting);
    }
    return color(resting);
}

ButtonColors buttonColors(const ButtonKind kind, const State state, const bool keyboardFocus) {
    const bool disabled = state == State::Disabled;
    ButtonColors result;
    switch (state == State::Selected ? ButtonKind::Primary : kind) {
    case ButtonKind::Primary: {
        // Selected (a checked toggle of any kind) borrows the accent triple too, with the
        // pressed rung for a checked toggle held down.
        const Color fill = state == State::Hover     ? Color::AccentHover
                           : state == State::Pressed ? Color::AccentPressed
                                                     : Color::Accent;
        result.fill = disabled ? disabledInk(color(Color::Accent)) : color(fill);
        result.border = result.fill;
        result.ink = disabled ? disabledInk(color(Color::OnAccent)) : color(Color::OnAccent);
        break;
    }
    case ButtonKind::Secondary:
        result.fill = color(surfaceForState(Color::SurfaceRaised, state));
        result.border = color(borderForState(state == State::Focused ? State::Normal : state));
        result.ink = inkForState(Color::Foreground, state);
        break;
    case ButtonKind::Ghost:
    case ButtonKind::GhostDanger:
    case ButtonKind::Danger: {
        const bool committed = state == State::Hover || state == State::Pressed;
        if (kind != ButtonKind::Ghost && committed) {
            // The filled Danger recipe: the relation the accent triple states, applied to Error.
            const QColor error = color(Color::Error);
            result.fill = state == State::Pressed ? pressedFillFor(error) : hoverFillFor(error);
            result.border = result.fill;
            result.ink = color(Color::Foreground);
            break;
        }
        if (kind == ButtonKind::Danger) {
            // Outlined at rest: the destructive action is legible, but carries no plate until the
            // pointer commits to it.
            result.border = disabled ? disabledInk(color(Color::Error)) : color(Color::Error);
            result.ink = result.border;
            break;
        }
        if (committed) {
            result.fill = color(surfaceForState(Color::Surface, state));
            result.border = color(Color::BorderHover);
        }
        result.ink = inkForState(Color::Muted, state);
        break;
    }
    case ButtonKind::Recessed:
        // ControlSurface is not a ladder rung: hover and press move the border and the ink.
        result.fill = color(Color::ControlSurface);
        result.border = color(borderForState(state == State::Focused ? State::Normal : state));
        result.ink = inkForState(Color::Muted, state);
        break;
    case ButtonKind::MenuItem:
        if (state == State::Hover || state == State::Pressed) {
            result.fill = color(state == State::Pressed ? Color::AccentPressed : Color::Accent);
            result.border = result.fill;
        }
        result.ink =
            inkForState(Color::Foreground,
                        state == State::Hover || state == State::Pressed ? State::Normal : state);
        break;
    }
    if (keyboardFocus && !disabled) {
        // Focus wins over hover, and is never Accent-on-Accent.
        const bool accentPlate =
            result.fill.isValid() &&
            (result.fill == color(Color::Accent) || result.fill == color(Color::AccentHover) ||
             result.fill == color(Color::AccentPressed));
        result.border = color(accentPlate ? Color::Foreground : Color::Accent);
    }
    return result;
}

void applyHairlinePen(QPainter& painter, const QColor& value) {
    QPen pen(value);
    pen.setWidthF(snappedHairlineWidth(painter.device()->devicePixelRatio()));
    pen.setCosmetic(false);
    painter.setPen(pen);
}

QRectF snappedToDevicePixels(const QPainter& painter, const QRectF& bounds) {
    const QTransform transform = painter.deviceTransform();
    if (transform.type() > QTransform::TxScale || !transform.isInvertible()) {
        return bounds;
    }
    const QRectF device = transform.mapRect(bounds);
    const QRectF snapped(QPointF(std::round(device.left()), std::round(device.top())),
                         QPointF(std::round(device.right()), std::round(device.bottom())));
    return transform.inverted().mapRect(snapped);
}

QRectF controlBounds(const QWidget& widget) {
    const auto inset = static_cast<qreal>(px(Spacing::ControlInset));
    return QRectF(widget.rect()).adjusted(inset, inset, -inset, -inset);
}

void fillRoundedSurface(QPainter& painter, const QRectF& bounds, const QColor& fill,
                        const QColor& border, const Radius radius) {
    const QRectF box = snappedToDevicePixels(painter, bounds);
    const auto extent = static_cast<int>(std::lround(std::min(box.width(), box.height())));
    fillRoundedSurface(painter, bounds, fill, border, static_cast<qreal>(radiusPx(radius, extent)));
}

void fillRoundedSurface(QPainter& painter, const QRectF& bounds, const QColor& fill,
                        const QColor& border, const qreal cornerPx) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = snappedToDevicePixels(painter, bounds);
    const qreal corner = std::max(0.0, cornerPx);

    if (border.isValid() && border.alpha() > 0) {
        applyHairlinePen(painter, border);
        const qreal inset = painter.pen().widthF() / 2.0;
        painter.setBrush(fill.isValid() ? QBrush(fill) : Qt::NoBrush);
        painter.drawRoundedRect(box.adjusted(inset, inset, -inset, -inset), corner, corner);
    } else if (fill.isValid()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(box, corner, corner);
    }
    painter.restore();
}

void paintSelectedRow(QPainter& painter, const QRectF& row, const Color surface,
                      const bool leadingEdge) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(row, selectionWash(surface));
    if (leadingEdge) {
        painter.fillRect(QRectF(row.left(), row.top(), px(Size::SelectionEdge), row.height()),
                         color(Color::Accent));
    }
    painter.restore();
}

void paintRowSeparator(QPainter& painter, const QRectF& row) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    const qreal width = snappedHairlineWidth(painter.device()->devicePixelRatio());
    const QRectF snapped = snappedToDevicePixels(painter, row);
    painter.fillRect(QRectF(snapped.left(), snapped.bottom() - width, snapped.width(), width),
                     color(Color::Background));
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

namespace {

void fillCheckerCells(QPainter& painter, const QRectF& bounds, const int cellPx,
                      const Color lightRole = Color::CheckerLight,
                      const Color darkRole = Color::CheckerDark) {
    painter.setPen(Qt::NoPen);
    painter.fillRect(bounds, color(lightRole));
    const QColor dark = color(darkRole);
    const auto cell = static_cast<qreal>(cellPx);
    const auto rows = static_cast<int>(std::ceil(bounds.height() / cell));
    const auto columns = static_cast<int>(std::ceil(bounds.width() / cell));
    for (int row = 0; row < rows; ++row) {
        const qreal y = bounds.top() + static_cast<qreal>(row) * cell;
        const qreal h = std::min(cell, bounds.bottom() - y);
        for (int column = row % 2; column < columns; column += 2) {
            const qreal x = bounds.left() + static_cast<qreal>(column) * cell;
            const qreal w = std::min(cell, bounds.right() - x);
            painter.fillRect(QRectF(x, y, w, h), dark);
        }
    }
}

} // namespace

namespace {

// A blurred rounded square, alpha only, whose middle row and column are flat: the nine-patch
// source for drawElevationShadow(). Three box-blur passes approximate a gaussian closely enough
// for a shadow.
QImage renderShadowTile(const int blurPx, const int cornerPx, const QColor& tint, const qreal dpr) {
    const int blur = std::max(1, static_cast<int>(std::lround(blurPx * dpr)));
    const int corner = static_cast<int>(std::lround(cornerPx * dpr));
    const int core = 2 * corner + 1;
    const int side = core + 2 * blur;
    std::vector<float> alpha(static_cast<std::size_t>(side * side), 0.0F);
    {
        QImage shape(side, side, QImage::Format_Alpha8);
        shape.fill(0);
        QPainter painter(&shape);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 255));
        painter.drawRoundedRect(QRectF(blur, blur, core, core), corner, corner);
        painter.end();
        for (int y = 0; y < side; ++y) {
            const uchar* line = shape.constScanLine(y);
            for (int x = 0; x < side; ++x)
                alpha[static_cast<std::size_t>(y * side + x)] =
                    static_cast<float>(line[x]) / 255.0F;
        }
    }
    const int radius = std::max(1, blur / 3);
    std::vector<float> scratch(alpha.size());
    std::vector<float> prefix(static_cast<std::size_t>(side + 1));
    const auto at = [side](const bool horizontal, const int line, const int index) {
        return static_cast<std::size_t>(horizontal ? line * side + index : index * side + line);
    };
    const auto pass = [&](const bool horizontal) {
        for (int line = 0; line < side; ++line) {
            prefix[0] = 0.0F;
            for (int index = 0; index < side; ++index)
                prefix[static_cast<std::size_t>(index + 1)] =
                    prefix[static_cast<std::size_t>(index)] + alpha[at(horizontal, line, index)];
            for (int index = 0; index < side; ++index) {
                const int low = std::max(0, index - radius);
                const int high = std::min(side, index + radius + 1);
                scratch[at(horizontal, line, index)] = (prefix[static_cast<std::size_t>(high)] -
                                                        prefix[static_cast<std::size_t>(low)]) /
                                                       static_cast<float>(2 * radius + 1);
            }
        }
        alpha.swap(scratch);
    };
    for (int iteration = 0; iteration < 3; ++iteration) {
        pass(true);
        pass(false);
    }
    QImage tile(side, side, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < side; ++y) {
        auto* line = reinterpret_cast<QRgb*>(tile.scanLine(y));
        for (int x = 0; x < side; ++x) {
            const float value =
                std::clamp(alpha[static_cast<std::size_t>(y * side + x)], 0.0F, 1.0F) *
                static_cast<float>(tint.alphaF());
            const auto a = static_cast<int>(std::lround(value * 255.0F));
            line[x] = qPremultiply(qRgba(tint.red(), tint.green(), tint.blue(), a));
        }
    }
    tile.setDevicePixelRatio(dpr);
    return tile;
}

struct ShadowKey {
    Elevation elevation;
    int corner;
    int ratioMilli;
    [[nodiscard]] bool operator==(const ShadowKey&) const noexcept = default;
};

} // namespace

void drawElevationShadow(QPainter& painter, const QRectF& caster, const Radius radius,
                         const Elevation elevation) {
    const Shadow token = shadow(elevation);
    if (token.isFlat() || caster.isEmpty()) {
        return;
    }
    const qreal dpr = painter.device()->devicePixelRatio();
    const int corner =
        radiusPx(radius, static_cast<int>(std::min(caster.width(), caster.height())));
    static std::vector<std::pair<ShadowKey, QImage>> tiles;
    const ShadowKey key{elevation, corner, static_cast<int>(std::lround(dpr * 1000.0))};
    auto found = std::ranges::find(tiles, key, &std::pair<ShadowKey, QImage>::first);
    if (found == tiles.end()) {
        tiles.emplace_back(key, renderShadowTile(token.blurRadius, corner, token.color, dpr));
        found = std::prev(tiles.end());
    }
    const QImage& tile = found->second;
    const qreal blur = token.blurRadius;
    const qreal edge = blur + corner;
    const qreal middle = (static_cast<qreal>(tile.width()) / dpr) - 2.0 * edge;
    const QRectF outer =
        caster.translated(token.offsetX, token.offsetY).adjusted(-blur, -blur, blur, blur);
    if (outer.width() < 2.0 * edge || outer.height() < 2.0 * edge) {
        return;
    }
    const auto source = [&](const qreal x, const qreal y, const qreal w, const qreal h) {
        return QRectF(x * dpr, y * dpr, w * dpr, h * dpr);
    };
    const qreal innerW = outer.width() - 2.0 * edge;
    const qreal innerH = outer.height() - 2.0 * edge;
    const qreal far = edge + middle;
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const std::array<qreal, 3> sx{0.0, edge, far};
    const std::array<qreal, 3> sw{edge, middle, edge};
    const std::array<qreal, 3> dx{outer.left(), outer.left() + edge, outer.right() - edge};
    const std::array<qreal, 3> dw{edge, innerW, edge};
    const std::array<qreal, 3> dy{outer.top(), outer.top() + edge, outer.bottom() - edge};
    const std::array<qreal, 3> dh{edge, innerH, edge};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            painter.drawImage(QRectF(dx[column], dy[row], dw[column], dh[row]), tile,
                              source(sx[column], sx[row], sw[column], sw[row]));
    painter.restore();
}

void drawAlphaCheckerboard(QPainter& painter, const QRectF& bounds, const int cellPx,
                           const Radius radius) {
    if (bounds.isEmpty() || cellPx <= 0) {
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = snappedToDevicePixels(painter, bounds);
    const auto extent = static_cast<int>(std::lround(std::min(box.width(), box.height())));
    const auto corner = static_cast<qreal>(radiusPx(radius, extent));
    QPainterPath clip;
    clip.addRoundedRect(box, corner, corner);
    painter.setClipPath(clip, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing, false);
    fillCheckerCells(painter, box, cellPx);
    painter.restore();
}

void drawAlphaCheckerboard(QPainter& painter, const QRectF& bounds, const int cellPx,
                           const Color light, const Color dark) {
    if (bounds.isEmpty() || cellPx <= 0) {
        return;
    }
    painter.save();
    painter.setClipRect(bounds, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing, false);
    fillCheckerCells(painter, bounds, cellPx, light, dark);
    painter.restore();
}

} // namespace bloom::ui::kit
