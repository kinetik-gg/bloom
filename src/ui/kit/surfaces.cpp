#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPen>
#include <bloom/ui/kit/mnemonic_style.hpp>
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
    ensureKeyboardFocusTracking(*this);
    const int pitch = px(Size::PropertiesAnchorDot) + px(Spacing::XS);
    setFixedSize(pitch * 3, pitch * 3);
}
QRect KAnchorGrid::pointRect(int index) const {
    const int dot = px(Size::PropertiesAnchorDot), pitch = dot + px(Spacing::XS);
    return {index % 3 * pitch + px(Spacing::XXS), index / 3 * pitch + px(Spacing::XXS), dot, dot};
}
void KAnchorGrid::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    // Round dots (Radius::Full), the selected one in Keyframe; keyboard focus rings the selected
    // dot in Accent, a click does not.
    const bool focused = hasKeyboardFocus(*this);
    for (int index = 0; index < 9; ++index)
        fillRoundedSurface(painter, pointRect(index),
                           color(index == selectedPoint() ? Color::Keyframe : Color::BorderHover),
                           focused && index == selectedPoint() ? color(Color::Accent) : QColor{},
                           Radius::Full);
}
void KAnchorGrid::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        for (int index = 0; index < 9; ++index)
            if (pointRect(index)
                    .adjusted(-px(Spacing::XXS), -px(Spacing::XXS), px(Spacing::XXS),
                              px(Spacing::XXS))
                    .contains(event->pos())) {
                choosePoint(index);
                event->accept();
                return;
            }
    }
    QWidget::mousePressEvent(event);
}
void KAnchorGrid::keyPressEvent(QKeyEvent* event) {
    const int current = selectedPoint() < 0 ? 4 : selectedPoint();
    switch (event->key()) {
    case Qt::Key_Left:
        choosePoint(current % 3 > 0 ? current - 1 : current);
        break;
    case Qt::Key_Right:
        choosePoint(current % 3 < 2 ? current + 1 : current);
        break;
    case Qt::Key_Up:
        choosePoint(current >= 3 ? current - 3 : current);
        break;
    case Qt::Key_Down:
        choosePoint(current < 6 ? current + 3 : current);
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}
void KListSurface::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const int pitch = px(Size::ListRow);
    const int first = offset_ / pitch;
    for (int row = first; row <= (offset_ + height()) / pitch; ++row) {
        const QRectF band(0, row * pitch - offset_, width(), pitch);
        painter.fillRect(band, color(Color::Surface));
        paintRowSeparator(painter, band);
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
    const QColor ink = color(active_ ? Color::Accent : Color::Border);
    const qreal r = radiusPx();
    if (r <= 0.0) {
        // Square panels: no corner wedges to fill, just the one snapped hairline.
        fillRoundedSurface(painter, QRectF(rect()), {}, ink, Radius::Panel);
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    applyHairlinePen(painter, ink);
    const qreal inset = painter.pen().widthF() / 2.0;
    const QRectF bounds = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
    QPainterPath frame;
    frame.addRoundedRect(bounds, r, r);
    QPainterPath outside;
    outside.addRect(QRectF(rect()));
    painter.fillPath(outside.subtracted(frame), color(Color::Background));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(frame);
}
KSurface::KSurface(QWidget* parent, const Color fill) : QWidget(parent) {
    setAutoFillBackground(true);
    setFill(fill);
}
void KSurface::setFill(const Color fill) {
    auto colors = palette();
    colors.setColor(QPalette::Window, color(fill));
    setPalette(colors);
}
} // namespace bloom::ui::kit
