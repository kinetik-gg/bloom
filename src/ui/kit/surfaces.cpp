#include <bloom/ui/kit/surfaces.hpp>
#include <cmath>
namespace bloom::ui::kit {
void KDiamond::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const auto tint = animated_ || underMouse()
                          ? color(Color::Keyframe)
                          : withOpacity(color(Color::Muted), kDisabledOpacity);
    // Include the scene transform as well as the paint device DPR. Reset to device pixels:
    // no cached pixmap is resampled when the canvas zoom changes.
    const auto transform = painter.deviceTransform();
    const auto center = transform.map(QPointF(width() / 2.0, height() / 2.0));
    const auto scale = std::hypot(transform.m11(), transform.m12());
    const auto radius = std::max(1.0, std::round(kKeyDiamondRadius * scale));
    const auto stroke = std::max(1.0, std::round(kDiamondStroke * scale));
    painter.resetTransform();
    painter.scale(1.0 / painter.device()->devicePixelRatioF(),
                  1.0 / painter.device()->devicePixelRatioF());
    const QPointF snapped(std::round(center.x()), std::round(center.y()));
    QPolygonF diamond{snapped + QPointF(0, -radius), snapped + QPointF(radius, 0),
                      snapped + QPointF(0, radius), snapped + QPointF(-radius, 0)};
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(tint, stroke, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    painter.setBrush(keyed_ ? QBrush(tint) : Qt::NoBrush);
    painter.drawPolygon(diamond);
    if (animated_ && !keyed_) {
        painter.setClipRect(QRectF(snapped.x() - radius - stroke, snapped.y() - radius - stroke,
                                   radius + stroke, 2 * (radius + stroke)));
        painter.setBrush(tint);
        painter.drawPolygon(diamond);
    }
}
KAnchorGrid::KAnchorGrid(QWidget* parent) : QWidget(parent) {
    const int pitch = px(Size::PropertiesAnchorDot) + px(Spacing::XS);
    setFixedSize(pitch * 3, pitch * 3);
}
QRect KAnchorGrid::pointRect(int index) const {
    const int dot = px(Size::PropertiesAnchorDot), pitch = dot + px(Spacing::XS);
    return {index % 3 * pitch + px(Spacing::XXS), index / 3 * pitch + px(Spacing::XXS), dot, dot};
}
void KAnchorGrid::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    for (int index = 0; index < 9; ++index)
        fillRoundedSurface(painter, pointRect(index),
                           color(index == selectedPoint() ? Color::Keyframe : Color::BorderHover),
                           hasFocus() && index == selectedPoint() ? color(Color::Accent) : QColor{},
                           Radius::Small);
}
void KListSurface::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const int pitch = px(Size::ListRow);
    const int first = offset_ / pitch;
    for (int row = first; row <= (offset_ + height()) / pitch; ++row) {
        const int y = row * pitch - offset_;
        painter.fillRect(QRect(0, y, width(), pitch), color(Color::Surface));
        painter.setPen(color(Color::Background));
        painter.drawLine(0, y + pitch - 1, width(), y + pitch - 1);
    }
}
void KSurface::clipPanelChildren(QWidget& panel) {
    QPainterPath clip;
    const auto radius = radiusPx(Radius::Panel, 0);
    clip.addRoundedRect(QRectF(panel.rect()), radius, radius);
    panel.setMask(QRegion(clip.toFillPolygon().toPolygon()));
}
KSurface::KSurface(QWidget* parent) : QWidget(parent) {
    setAutoFillBackground(true);
    auto colors = palette();
    colors.setColor(QPalette::Window, color(Color::Surface));
    setPalette(colors);
}
} // namespace bloom::ui::kit
