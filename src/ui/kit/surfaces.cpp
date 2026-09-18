#include <QEvent>
#include <QPen>
#include <bloom/ui/kit/surfaces.hpp>
#include <cmath>
namespace bloom::ui::kit {
void KDiamond::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const auto tint =
        animated_ ? color(Color::Keyframe) : withOpacity(color(Color::Muted), kDisabledOpacity);
    // Include the scene transform as well as the paint device DPR. Resolve vertices and stroke in
    // device pixels: no cached pixmap is resampled when the canvas zoom changes.
    const auto transform = painter.deviceTransform();
    const auto center = transform.map(QPointF(width() / 2.0, height() / 2.0));
    const auto scale = std::hypot(transform.m11(), transform.m12());
    const auto radius = std::max(1.0, std::round(kKeyDiamondRadius * scale));
    const auto stroke = std::max(1.0, std::round(kDiamondStroke * scale));
    const QPointF snapped(std::round(center.x()), std::round(center.y()));
    QPolygonF diamond{snapped + QPointF(0, -radius), snapped + QPointF(radius, 0),
                      snapped + QPointF(0, radius), snapped + QPointF(-radius, 0)};
    const auto inverse = transform.inverted();
    const auto localDiamond = inverse.map(diamond);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(tint, stroke / scale, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    painter.setBrush(fill_ == Fill::Full ? QBrush(tint) : Qt::NoBrush);
    painter.drawPolygon(localDiamond);
    if (animated_ && fill_ == Fill::Half) {
        painter.setClipRect(
            inverse.mapRect(QRectF(snapped.x() - radius - stroke, snapped.y() - radius - stroke,
                                   radius + stroke, 2 * (radius + stroke))));
        painter.setBrush(tint);
        painter.drawPolygon(localDiamond);
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
KPanelFrame::KPanelFrame(QWidget* panel) : QWidget(panel), panel_(panel) {
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::NoFocus);
    setObjectName(QStringLiteral("panelFrame"));
    panel_->installEventFilter(this);
    fit();
}
int KPanelFrame::radiusPx() noexcept { return kit::radiusPx(Radius::Panel, 0); }
void KPanelFrame::setActive(const bool active) {
    if (active_ == active)
        return;
    active_ = active;
    update();
}
void KPanelFrame::fit() {
    setGeometry(panel_->rect());
    raise();
}
bool KPanelFrame::eventFilter(QObject* watched, QEvent* event) {
    if (watched == panel_) {
        if (event->type() == QEvent::Resize)
            fit();
        else if (event->type() == QEvent::ChildAdded)
            // ChildAdded arrives after the child is already in the parent's child list, so
            // raising this overlay now moves it above the newcomer. A queued functor call was
            // used first; Qt 6.8's invokeMethod template trips the CI analyzer's leak check.
            raise();
    }
    return QWidget::eventFilter(watched, event);
}
void KPanelFrame::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal r = radiusPx();
    const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath frame;
    frame.addRoundedRect(bounds, r, r);
    QPainterPath outside;
    outside.addRect(QRectF(rect()));
    painter.fillPath(outside.subtracted(frame), color(Color::Background));
    painter.setPen(QPen(color(active_ ? Color::BorderActive : Color::Border), kHairlineWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(frame);
}
KSurface::KSurface(QWidget* parent) : QWidget(parent) {
    setAutoFillBackground(true);
    auto colors = palette();
    colors.setColor(QPalette::Window, color(Color::Surface));
    setPalette(colors);
}
} // namespace bloom::ui::kit
