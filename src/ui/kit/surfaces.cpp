#include <bloom/ui/kit/surfaces.hpp>
namespace bloom::ui::kit {
void KDiamond::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const auto tint = animated_ || underMouse()
                          ? color(Color::Keyframe)
                          : withOpacity(color(Color::Muted), kDisabledOpacity);
    const auto glyph = iconPixmap(IconId::Keyframe, Size::IconSmall, tint, devicePixelRatioF(),
                                  keyed_ ? IconWeight::Fill : IconWeight::Regular);
    const auto extent = glyph.deviceIndependentSize();
    const QPointF origin((width() - extent.width()) / 2, (height() - extent.height()) / 2);
    painter.drawPixmap(origin, glyph);
    if (animated_ && !keyed_) {
        painter.setClipRect(QRectF(origin, QSizeF(extent.width() / 2, extent.height())));
        painter.drawPixmap(origin, iconPixmap(IconId::Keyframe, Size::IconSmall, tint,
                                              devicePixelRatioF(), IconWeight::Fill));
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
KSurface::KSurface(QWidget* parent) : QWidget(parent) {
    setAutoFillBackground(true);
    auto colors = palette();
    colors.setColor(QPalette::Window, color(Color::Surface));
    setPalette(colors);
}
} // namespace bloom::ui::kit
