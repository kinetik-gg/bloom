#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QGraphicsProxyWidget>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>
#include <algorithm>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <cmath>
namespace bloom::ui {
namespace {
constexpr qreal kCanvasHalfExtent = kit::kNodeCanvasHalfExtent;
constexpr int kWheelDetent = 120;
} // namespace
// --- NodeGraphicsView --------------------------------------------------------------------------

NodeGraphicsView::NodeGraphicsView(QWidget* parent) : QGraphicsView(parent) {
    setObjectName("nodeGraphicsView");
    setAccessibleName(tr("Composition node graph"));
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    setDragMode(QGraphicsView::NoDrag);
    setViewportUpdateMode(QGraphicsView::MinimalViewportUpdate);
    setFrameShape(QFrame::NoFrame);
    // Zoom/pan live entirely in the view transform (see the class comment): with scrolling off and
    // a top-left alignment, viewportTransform() has no scroll offset folded into it, so
    // sceneFromViewport() is an exact inverse and a zoom step's cursor invariant is exact.
    setTransformationAnchor(QGraphicsView::NoAnchor);
    setResizeAnchor(QGraphicsView::NoAnchor);
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The view's OWN scene rectangle, which overrides the scene's for scrolling purposes -- see
    // kCanvasHalfExtent for why it is fixed and oversized rather than tracking the graph.
    setSceneRect(-kCanvasHalfExtent, -kCanvasHalfExtent, kCanvasHalfExtent * 2.0,
                 kCanvasHalfExtent * 2.0);
    // StrongFocus so Space and the navigation keys reach the canvas without a prior click -- the
    // Viewer's own rule.
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

double NodeGraphicsView::zoomFactor() const noexcept { return transform().m11(); }

bool NodeGraphicsView::viewAdjusted() const noexcept { return viewAdjusted_; }

QPointF NodeGraphicsView::sceneFromViewport(const QPointF viewportPoint) const {
    return viewportTransform().inverted().map(viewportPoint);
}

void NodeGraphicsView::zoomAboutViewportPoint(const QPointF viewportPoint, const double factor) {
    const double current = zoomFactor();
    if (!(current > 0.0) || !(factor > 0.0)) {
        return;
    }
    const double target =
        std::clamp(current * factor, ViewTransform::kMinZoom, ViewTransform::kMaxZoom);
    const double applied = target / current;
    if (qFuzzyCompare(applied, 1.0)) {
        viewAdjusted_ = true;
        return;
    }
    // Scale about `viewportPoint` in VIEWPORT space, composed after the current transform: shift
    // the point to the origin, scale, shift back. Qt maps row vectors (p * M), so "apply A then B"
    // is A * B -- the composition order below is exactly that, and nothing about it depends on a
    // scroll bar's value or range.
    QTransform about = QTransform::fromTranslate(-viewportPoint.x(), -viewportPoint.y());
    about *= QTransform::fromScale(applied, applied);
    about *= QTransform::fromTranslate(viewportPoint.x(), viewportPoint.y());
    setTransform(transform() * about);
    viewAdjusted_ = true;
}

void NodeGraphicsView::zoomStep(const int notches) {
    if (notches == 0) {
        return;
    }
    zoomAboutViewportPoint(QRectF(viewport()->rect()).center(), std::pow(kZoomStepFactor, notches));
}

QRectF NodeGraphicsView::graphBounds() const {
    return scene() == nullptr ? QRectF() : scene()->itemsBoundingRect();
}

bool NodeGraphicsView::applyCenteredScale(const QRectF& bounds, const double scale) {
    const QRectF view = QRectF(viewport()->rect());
    if (bounds.isEmpty() || view.isEmpty()) {
        return false;
    }
    const double clamped = std::clamp(scale, ViewTransform::kMinZoom, ViewTransform::kMaxZoom);
    QTransform framed = QTransform::fromTranslate(-bounds.center().x(), -bounds.center().y());
    framed *= QTransform::fromScale(clamped, clamped);
    framed *= QTransform::fromTranslate(view.center().x(), view.center().y());
    setTransform(framed);
    return true;
}

void NodeGraphicsView::frameGraph() {
    const QRectF bounds = graphBounds();
    const QRectF view = QRectF(viewport()->rect());
    if (bounds.isEmpty() || view.isEmpty()) {
        return;
    }
    if (!applyCenteredScale(
            bounds, std::min(view.width() / bounds.width(), view.height() / bounds.height()))) {
        return;
    }
    // Fit means "keep framing everything": a later projection rebuild re-frames until the artist
    // moves the view themselves.
    viewAdjusted_ = false;
    framedOnce_ = true;
}

void NodeGraphicsView::zoomToActualSize() {
    // A gesture that could not move the view does not count as the artist having taken the view
    // over. Latching the flag on an empty canvas -- no composition, or a collapsed panel -- would
    // permanently stop the auto-framing that a rebuild and a resize depend on, and the canvas would
    // never frame the content that arrived afterwards.
    if (!applyCenteredScale(graphBounds(), 1.0)) {
        return;
    }
    viewAdjusted_ = true;
    framedOnce_ = true;
}

void NodeGraphicsView::zoomToPercent(const int percent) {
    if (!applyCenteredScale(graphBounds(), static_cast<double>(percent) / 100.0)) {
        return;
    }
    viewAdjusted_ = true;
    framedOnce_ = true;
}

void NodeGraphicsView::frameRect(const QRectF bounds) {
    const QRectF view = QRectF(viewport()->rect());
    if (bounds.isEmpty() || view.isEmpty()) {
        return;
    }
    if (!applyCenteredScale(
            bounds, std::min(view.width() / bounds.width(), view.height() / bounds.height()))) {
        return;
    }
    // Unlike Fit, framing a specific selection is a deliberate placement: it counts as the artist
    // having taken the view over, exactly as 100% and a wheel step do.
    viewAdjusted_ = true;
    framedOnce_ = true;
}

void NodeGraphicsView::showEvent(QShowEvent* event) {
    QGraphicsView::showEvent(event);
    if (!framedOnce_ && !viewAdjusted_) {
        frameGraph();
    }
}

void NodeGraphicsView::resizeEvent(QResizeEvent* event) {
    QGraphicsView::resizeEvent(event);
    // The Viewer's Fit recomputes its rectangle from the available area on every paint, so a
    // resized Viewer stays fitted. This canvas keeps that behavior for the same state: while the
    // artist has not moved the view, a resize re-frames the graph. Once they have, the view is
    // theirs and a resize leaves it exactly where they put it.
    if (!viewAdjusted_) {
        frameGraph();
    }
}

void NodeGraphicsView::contextMenuEvent(QContextMenuEvent* event) {
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        event->accept();
        return;
    }
    emit contextMenuRequested(event->pos());
    event->accept();
}

void NodeGraphicsView::wheelEvent(QWheelEvent* event) {
    if (panActive_) {
        event->ignore();
        return;
    }
    // The wheel ALWAYS zooms the canvas, including over an in-node field. The canvas's
    // zoom-about-cursor rule is decision 1's contract and an artist reaching for the wheel over a
    // dense graph means "zoom" every time; a field that silently ate the gesture because it
    // happened to hold focus would make the canvas feel broken in exactly the places it is densest.
    // The field loses nothing it needs: its own steppers, and Up/Down/PageUp/PageDown while
    // focused (kit/value_field.cpp), still step it.
    const int notches = event->angleDelta().y() / kWheelDetent;
    if (notches == 0) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    zoomAboutViewportPoint(event->position(), std::pow(kZoomStepFactor, notches));
    event->accept();
}

void NodeGraphicsView::mousePressEvent(QMouseEvent* event) {
    if (!panActive_ && (event->button() == Qt::MiddleButton)) {
        panActive_ = true;
        panButton_ = event->button();
        panOrigin_ = event->position();
        panBaseTransform_ = transform();
        setDragMode(QGraphicsView::NoDrag);
        setFocus(Qt::MouseFocusReason);
        updatePanCursor();
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void NodeGraphicsView::mouseMoveEvent(QMouseEvent* event) {
    if (!panActive_) {
        QGraphicsView::mouseMoveEvent(event);
        if (const auto* graph = qobject_cast<NodeGraphicsScene*>(scene());
            graph && graph->gestureActive())
            viewAdjusted_ = true;
        return;
    }
    // TOTAL displacement from the press point applied to the transform frozen there -- never a
    // chain of per-move deltas (the Viewer's own pan rule).
    const QPointF delta = event->position() - panOrigin_;
    setTransform(panBaseTransform_ * QTransform::fromTranslate(delta.x(), delta.y()));
    viewAdjusted_ = true;
    event->accept();
}

void NodeGraphicsView::mouseReleaseEvent(QMouseEvent* event) {
    if (panActive_ && event->button() == panButton_) {
        panActive_ = false;
        panButton_ = Qt::NoButton;
        setDragMode(QGraphicsView::NoDrag);
        updatePanCursor();
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

namespace {
// The canvas's own keys (task S1, item 8; docs/ux/interaction-model.md is the list). Adobe-first:
// Delete/Backspace remove, Ctrl+D duplicates, Ctrl+A selects all, Ctrl+0 fits and Ctrl+1 is actual
// size, Tab opens Add, Enter renames, Ctrl+G groups and Ctrl+Shift+G ungroups.
// Mute, collapse and dissolve are context-menu commands and bind no key at all.
bool canvasShortcut(const QKeyEvent& event) {
    const auto modifiers = event.modifiers();
    const int key = event.key();
    if (modifiers == Qt::NoModifier)
        return key == Qt::Key_Home || key == Qt::Key_Delete || key == Qt::Key_Backspace ||
               key == Qt::Key_Escape || key == Qt::Key_Return || key == Qt::Key_Enter ||
               key == Qt::Key_Tab;
    if (modifiers == (Qt::ControlModifier | Qt::ShiftModifier))
        return key == Qt::Key_G;
    return modifiers == Qt::ControlModifier &&
           (key == Qt::Key_A || key == Qt::Key_D || key == Qt::Key_G || key == Qt::Key_0 ||
            key == Qt::Key_1);
}
bool fieldFocused(const QGraphicsScene* scene) {
    return scene && dynamic_cast<QGraphicsProxyWidget*>(scene->focusItem()) != nullptr;
}

// Whether a TEXT editor is actually active inside the focused field (task FIX1, item F). A
// kit::KValueField keeps its QLineEdit hidden until the artist enters it, and a colour chip, a
// switch and a dropdown have no text entry at all -- so "a field has focus" and "the artist is
// typing" are different questions, and only the second one has any claim on Delete.
bool textEditorActive(const QGraphicsScene* scene) {
    const auto* proxy =
        scene == nullptr ? nullptr : dynamic_cast<QGraphicsProxyWidget*>(scene->focusItem());
    const QWidget* hosted = proxy == nullptr ? nullptr : proxy->widget();
    if (hosted == nullptr)
        return false;
    const QWidget* focused = hosted->focusWidget();
    const auto* line = qobject_cast<const QLineEdit*>(focused == nullptr ? hosted : focused);
    return line != nullptr && line->isVisible() && !line->isReadOnly();
}

// Whether the canvas takes this key press for itself. A focused field keeps every key it can
// actually use; the two the canvas takes BACK are Delete and Backspace, and only where no text
// editor is active in that field. Clicking a card's value row, colour chip, switch or dropdown gave
// the field the scene's focus, and the canvas then declined its own binding -- so "click a card,
// press Delete" did nothing at all, which is exactly what the owner reported.
bool canvasClaimsKey(const QGraphicsScene* scene, const QKeyEvent& event) {
    if (!canvasShortcut(event))
        return false;
    if (!fieldFocused(scene))
        return true;
    const bool removal = event.key() == Qt::Key_Delete || event.key() == Qt::Key_Backspace;
    return removal && !textEditorActive(scene);
}
} // namespace
bool NodeGraphicsView::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride &&
        canvasClaimsKey(scene(), *static_cast<QKeyEvent*>(event))) {
        event->accept();
        return true;
    }
    // Tab is claimed before QWidget::event() can spend it on focus traversal, which is what would
    // otherwise happen to it and is never what Tab means on this canvas. A focused in-node field
    // keeps Tab for committing and travelling, exactly as it did.
    if (event->type() == QEvent::KeyPress && !fieldFocused(scene())) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Tab && key->modifiers() == Qt::NoModifier) {
            keyPressEvent(key);
            return true;
        }
    }
    return QGraphicsView::event(event);
}
void NodeGraphicsView::focusOutEvent(QFocusEvent* event) {
    panActive_ = false;
    updatePanCursor();
    Q_EMIT canvasFocusLost();
    QGraphicsView::focusOutEvent(event);
}
void NodeGraphicsView::keyPressEvent(QKeyEvent* event) {
    if (!panActive_ && canvasClaimsKey(scene(), *event)) {
        event->accept();
        if (event->isAutoRepeat())
            return;
        if (event->modifiers() == Qt::NoModifier) {

            if (event->key() == Qt::Key_Home) {
                frameGraph();
                return;
            }
        }
        if (event->modifiers() == Qt::ControlModifier) {
            // Ctrl+0 fit and Ctrl+1 actual size, the same pair the Viewer now answers to. F and Z
            // are retired in both canvases (task S1, item 8).
            if (event->key() == Qt::Key_0) {
                frameGraph();
                return;
            }
            if (event->key() == Qt::Key_1) {
                zoomToActualSize();
                return;
            }
        }
        Q_EMIT canvasKeyPressed(event->key(), event->modifiers());
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

void NodeGraphicsView::updatePanCursor() {
    if (panActive_) {
        viewport()->setCursor(Qt::ClosedHandCursor);

    } else {
        viewport()->unsetCursor();
    }
}

} // namespace bloom::ui
