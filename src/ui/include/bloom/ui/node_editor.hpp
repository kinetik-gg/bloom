#pragma once

#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QGraphicsScene>
#include <QGraphicsView>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QTransform>
#include <QWidget>

class QContextMenuEvent;
class QKeyEvent;
class QMenu;
class QMouseEvent;
class QPainter;
class QShowEvent;
class QWheelEvent;

namespace bloom::ui {

class CompositionSession;

inline constexpr int kNodeItemKindRole = Qt::UserRole + 1;
inline constexpr int kNodeStableIdRole = Qt::UserRole + 2;

// The typed-connector color mapping (task U4, issue #123, decision 3), keyed by the runtime's own
// socket kind rather than by a node-editor-local guess at what a wire carries.
//
// `runtime::SocketValueKind` has exactly ONE enumerator today -- `Image` -- and
// docs/architecture/evaluation-primitives.md says so in as many words: "The current `Image` socket
// is only the first transport kind; semantic role constraints must exist before masks, depth,
// normals, motion, UV, ID, or arbitrary data images can use it safely." Every port constant in
// src/document is literally named "image" (kSolidSourceOutputPort, kTextSourceOutputPort,
// kLayerOutputContentInputPort, kLayerOutputOutputPort, kLayerStackOutputPort,
// kCompositionOutputInputPort, kCompositionOutputOutputPort), and every edge in a
// document::CanonicalGraph -- whether its destination is a NodeInputRef or a LayerStackInputRef
// slot -- carries that one transport.
//
// So this is deliberately a ONE-ENTRY mapping, not a speculative palette. Image transport takes
// `Color::DataImage`, the data-type palette's own image role (docs/ux/visual-language.md). The
// second transport kind gets its own token the day `SocketValueKind` gains its second enumerator,
// and this switch is what will refuse to compile until someone makes that decision explicitly.
//
// Parameter/object links are NOT colored here because they do not exist as connectors: a
// document::ParameterRecord binds to a node through a ParameterBinding (a role plus a stable
// ParameterId), never through a graph edge, and the one "graph-driven value" source the model
// declares -- document::DriverBindingSource -- is rejected outright by both the project format
// (project::CanonicalDocumentError::UnsupportedDriverBindingSource) and the runtime snapshot
// compiler. Drawing a second connector color for a link kind no document can currently contain
// would be exactly the speculative palette this decision forbids.
[[nodiscard]] kit::Color socketColorToken(runtime::SocketValueKind kind) noexcept;

class NodeGraphicsScene final : public QGraphicsScene {
    Q_OBJECT

  public:
    explicit NodeGraphicsScene(QObject* parent = nullptr);

    // The session in-node field rows read and commit through (decision 5). Null leaves the scene a
    // pure read-only projection with no editable rows at all -- the shape
    // kinetik_baseline_tests.cpp constructs directly to assert the canvas background token.
    void setSession(CompositionSession* session);

    // Reconciles the scene against `snapshot`'s composition IN PLACE: node cards are matched by
    // their stable NodeId and updated, never dropped and rebuilt, so an in-node kit field keeps its
    // identity (and its keyboard focus) across the snapshot change that its own edit produced.
    // Cards for nodes that left the graph are removed; edges, which carry no widget state, are
    // rebuilt outright each time.
    void setProjection(const document::Snapshot& snapshot, document::CompositionId compositionId);
    [[nodiscard]] QGraphicsItem* findNodeItem(document::NodeId nodeId) const;

    // Test/diagnostic surface only, mirroring ViewerEditor's own *ForTest precedent: an in-node kit
    // field lives inside a QGraphicsProxyWidget, so it is not a QWidget child of the view and
    // findChild() cannot reach it.
    [[nodiscard]] QWidget* nodeFieldForTest(document::NodeId nodeId,
                                            const QString& fieldObjectName) const;

  protected:
    void drawBackground(QPainter* painter, const QRectF& rect) override;

  private:
    void rebuildEdges(const document::Composition& composition);

    CompositionSession* session_ = nullptr;
};

// The graph canvas. Its navigation conventions are the Viewer's, deliberately and structurally:
// the wheel step factor and the zoom bounds are the SAME named constants viewer_editor.hpp
// publishes (kZoomStepFactor, ViewTransform::kMinZoom/kMaxZoom), so the two canvases cannot drift
// apart by someone re-spelling a number. Wheel zooms about the cursor, Space-hold + left drag or a
// middle drag pans, F frames the graph, Z returns to 100% -- one app, one feel (decision 1).
//
// QGraphicsView's own transform is what carries zoom and pan underneath, but its SCROLLING is
// switched off entirely -- both scroll bar policies AlwaysOff, top-left alignment, and a fixed
// oversized view scene rectangle (see kCanvasHalfExtent in node_editor.cpp) that keeps
// QGraphicsView's alignment indents and scroll values pinned at zero. The view transform is then
// the whole mapping: viewportTransform() is exactly it, which is what makes the zoom-about-cursor
// invariant exact and testable rather than dependent on a scroll bar's clamping behavior.
class NodeGraphicsView final : public QGraphicsView {
    Q_OBJECT

  public:
    explicit NodeGraphicsView(QWidget* parent = nullptr);

    // The current uniform scale: 1.0 is 100%, clamped by construction into the Viewer's own
    // [ViewTransform::kMinZoom, ViewTransform::kMaxZoom].
    [[nodiscard]] double zoomFactor() const noexcept;

    // False until the artist zooms or pans; the editor re-frames the graph on a projection rebuild
    // only while it is false, so a freshly opened panel frames its content and a deliberately
    // positioned view is never yanked out from under the artist. Fit (F) puts it back to false --
    // "keep framing everything" -- while 100% (Z), a wheel step, and a pan set it.
    [[nodiscard]] bool viewAdjusted() const noexcept;

    // The scene point under a viewport point, exactly (no integer rounding): the mapping the
    // zoom-about-cursor invariant is stated in terms of.
    [[nodiscard]] QPointF sceneFromViewport(QPointF viewportPoint) const;

    // Scales by `factor` (clamped into the shared zoom bounds) while holding the scene point under
    // `viewportPoint` fixed.
    void zoomAboutViewportPoint(QPointF viewportPoint, double factor);
    // `notches` wheel detents' worth of zoom about the viewport's own center -- the menu's Zoom
    // In/Out and the keyboard both land here.
    void zoomStep(int notches);
    // F: scales and centers so every item fits, clamped into the shared zoom bounds.
    void frameGraph();
    // Z: exactly 100%, with the graph's bounding rectangle centered -- the same "actual size,
    // centered" the Viewer's 100% means.
    void zoomToActualSize();

  Q_SIGNALS:
    // Emitted instead of QGraphicsView's default "forward a context menu event into the scene"
    // behavior, which no item here consumes. `viewportPosition` is in viewport coordinates, so it
    // feeds itemAt()/mapToGlobal() directly.
    void contextMenuRequested(const QPoint& viewportPosition);

  protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void updatePanCursor();
    [[nodiscard]] QRectF graphBounds() const;
    void applyCenteredScale(double scale);

    bool spaceHeld_ = false;
    bool panActive_ = false;
    Qt::MouseButton panButton_ = Qt::NoButton;
    QPointF panOrigin_;
    // The transform frozen at pan begin: every move applies the TOTAL displacement from the press
    // point to it, never a chain of already-rounded per-move deltas -- the Viewer's own pan rule.
    QTransform panBaseTransform_;
    bool viewAdjusted_ = false;
    bool framedOnce_ = false;
};

class NodeGraphEditor final : public QWidget {
    Q_OBJECT

  public:
    explicit NodeGraphEditor(CompositionSession& session, QWidget* parent = nullptr);
    ~NodeGraphEditor() override;

    [[nodiscard]] NodeGraphicsScene* graphScene() const noexcept;
    [[nodiscard]] NodeGraphicsView* graphView() const noexcept;

    // Test/diagnostic surface only: the exact menu a right-click builds, parented to this widget
    // and never shown. Lets a test enumerate what the canvas offers -- and, just as importantly,
    // what it honestly does not.
    [[nodiscard]] QMenu* contextMenuForTest();

  private:
    void rebuild();
    void updateSelection();
    void sceneSelectionChanged();
    void showContextMenu(const QPoint& viewportPosition);
    [[nodiscard]] QMenu* buildContextMenu(QWidget* parent);

    CompositionSession& session_;
    NodeGraphicsScene* scene_ = nullptr;
    NodeGraphicsView* view_ = nullptr;
    bool rebuilding_ = false;
};

} // namespace bloom::ui
