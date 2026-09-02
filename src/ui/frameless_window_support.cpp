#include <bloom/ui/frameless_window_support.hpp>

#include <QEvent>
#include <QMainWindow>
#include <QMouseEvent>
#include <QWidget>
#include <QWindow>

namespace bloom::ui {

FramelessEdgeResizer::FramelessEdgeResizer(QMainWindow& window, const int marginPx, QObject* parent)
    : QObject(parent), window_(window), marginPx_(marginPx) {}

bool FramelessEdgeResizer::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::MouseButtonPress) {
        return false;
    }
    auto* mouseEvent = static_cast<QMouseEvent*>(event);
    if (mouseEvent->button() != Qt::LeftButton) {
        return false;
    }
    auto* target = qobject_cast<QWidget*>(watched);
    if (target == nullptr || target->window() != &window_) {
        return false;
    }
    if (window_.isMaximized() || window_.isFullScreen()) {
        return false;
    }
    auto* handle = window_.windowHandle();
    if (handle == nullptr) {
        return false;
    }

    const QPoint local = window_.mapFromGlobal(mouseEvent->globalPosition().toPoint());
    const QRect bounds = window_.rect();
    if (!bounds.marginsAdded(QMargins(marginPx_, marginPx_, marginPx_, marginPx_))
             .contains(local)) {
        return false;
    }

    Qt::Edges edges;
    if (local.x() <= bounds.left() + marginPx_) {
        edges |= Qt::LeftEdge;
    } else if (local.x() >= bounds.right() - marginPx_) {
        edges |= Qt::RightEdge;
    }
    if (local.y() <= bounds.top() + marginPx_) {
        edges |= Qt::TopEdge;
    } else if (local.y() >= bounds.bottom() - marginPx_) {
        edges |= Qt::BottomEdge;
    }
    if (edges == Qt::Edges{}) {
        return false;
    }

    handle->startSystemResize(edges);
    return true;
}

} // namespace bloom::ui
