#include <bloom/ui/kit/split_handle.hpp>

#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QEnterEvent>
#include <QMouseEvent>
#include <QPainter>

namespace bloom::ui::kit {

KSplitHandle::KSplitHandle(const Qt::Orientation orientation, QWidget* parent)
    : QWidget(parent), orientation_(orientation) {
    setObjectName(QStringLiteral("kSplitHandle"));
    setAttribute(Qt::WA_Hover, true);
    setCursor(orientation_ == Qt::Horizontal ? Qt::SplitHCursor : Qt::SplitVCursor);
    setToolTip(tr("Drag to resize; double-click to reset"));
    if (orientation_ == Qt::Horizontal) {
        setFixedWidth(px(Size::SplitHandle));
    } else {
        setFixedHeight(px(Size::SplitHandle));
    }
}

QSize KSplitHandle::sizeHint() const {
    return orientation_ == Qt::Horizontal ? QSize(px(Size::SplitHandle), 0)
                                          : QSize(0, px(Size::SplitHandle));
}

void KSplitHandle::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    // The painted line stays hairline-width and centered in the wider hit zone, so the divider
    // reads exactly as it did before this task -- only the draggable area grew.
    const auto ink = color((hovered_ || dragging_) ? Color::BorderHover : Color::Background);
    applyHairlinePen(painter, ink);
    if (orientation_ == Qt::Horizontal) {
        const auto x = static_cast<qreal>(width()) / 2.0;
        painter.drawLine(QPointF(x, 0.0), QPointF(x, static_cast<qreal>(height())));
    } else {
        const auto y = static_cast<qreal>(height()) / 2.0;
        painter.drawLine(QPointF(0.0, y), QPointF(static_cast<qreal>(width()), y));
    }
}

void KSplitHandle::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    dragging_ = true;
    pressPosition_ = event->globalPosition().toPoint();
    Q_EMIT dragStarted();
    update();
    event->accept();
}

void KSplitHandle::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const auto current = event->globalPosition().toPoint();
    const int delta = orientation_ == Qt::Horizontal ? current.x() - pressPosition_.x()
                                                     : current.y() - pressPosition_.y();
    Q_EMIT dragged(delta);
    event->accept();
}

void KSplitHandle::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool wasDragging = dragging_;
    dragging_ = false;
    update();
    if (wasDragging)
        Q_EMIT dragFinished();
    event->accept();
}

void KSplitHandle::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    Q_EMIT resetRequested();
    event->accept();
}

void KSplitHandle::enterEvent(QEnterEvent* event) {
    hovered_ = true;
    update();
    QWidget::enterEvent(event);
}

void KSplitHandle::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

} // namespace bloom::ui::kit
