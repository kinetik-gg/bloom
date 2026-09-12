#pragma once

#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <functional>
#include <memory>
#include <set>

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
class QResizeEvent;
class QShowEvent;
class QWheelEvent;

namespace bloom::ui {

class CompositionSession;
struct NodeInteraction;
namespace kit {
class KSearchPopup;
}

inline constexpr int kNodeItemKindRole = Qt::UserRole + 1;
inline constexpr int kNodeStableIdRole = Qt::UserRole + 2;

inline constexpr int kNodeSocketNameRole = Qt::UserRole + 3;
inline constexpr int kNodeSocketInputRole = Qt::UserRole + 4;
inline constexpr int kNodeHoveredRole = Qt::UserRole + 5;
inline constexpr int kNodeMutedRole = Qt::UserRole + 6;
inline constexpr int kNodeCollapsedRole = Qt::UserRole + 7;
inline constexpr int kNodeStructuralRole = Qt::UserRole + 8;

// Socket schema kinds use the N1 palette mapping, including presently nonlinkable kinds.
[[nodiscard]] kit::Color socketColorToken(runtime::SocketValueKind kind) noexcept;

// Which declared socket directions a card exposes, including unconnected ports. Input items live
// on its left edge and output items on its right edge.
struct NodeSockets final {
    bool hasInput = false;
    bool hasOutput = false;

    friend bool operator==(const NodeSockets&, const NodeSockets&) = default;
};

class NodeGraphicsScene final : public QGraphicsScene {
    Q_OBJECT

  public:
    explicit NodeGraphicsScene(QObject* parent = nullptr);
    ~NodeGraphicsScene() override;
    using Submit = std::function<commands::CommandResult(commands::Transaction&&)>;
    void setSubmit(Submit submit);
    [[nodiscard]] bool canSubmit() const { return static_cast<bool>(submit_); }
    void cancelGesture();
    void startDuplicateMove(QPointF scenePosition);
    [[nodiscard]] bool gestureActive() const;
    [[nodiscard]] commands::CommandResult submit(commands::Transaction&& transaction);
    void selectAllNodes();

  Q_SIGNALS:
    void addSearchRequested(QPointF scenePosition, QPoint screenPosition,
                            std::optional<document::InputPortRef> input,
                            std::optional<document::OutputPortRef> output);

  public:
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
    // Direction summary of the actual per-port child items, retained as a diagnostic contract.
    [[nodiscard]] NodeSockets nodeSocketsForTest(document::NodeId nodeId) const;

  protected:
    void drawBackground(QPainter* painter, const QRectF& rect) override;
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;
    // Task S1, item 8: a double-click on a layer card renames it, the second way into the rename
    // besides Enter. A card that is not a layer boundary has no name of its own to edit, and
    // NodeItem::startRename() is what says so.
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;

  private:
    void rebuildEdges(const document::Composition& composition);

    CompositionSession* session_ = nullptr;
    Submit submit_;
    std::unique_ptr<NodeInteraction> interaction_;
};

// The graph canvas. Its navigation conventions are the Viewer's, deliberately and structurally:
// the wheel step factor and the zoom bounds are the SAME named constants viewer_editor.hpp
// publishes (kZoomStepFactor, ViewTransform::kMinZoom/kMaxZoom), so the two canvases cannot drift
// apart by someone re-spelling a number. Wheel zooms about the cursor, Space-hold + left drag or a
// middle drag pans, Ctrl+0 frames the graph and Ctrl+1 returns to 100% -- the same pair the Viewer
// answers to, one app, one feel (decision 1; task S1, item 8 retired F and Z in both).
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
    // positioned view is never yanked out from under the artist. Fit (Ctrl+0) puts it back to false
    // -- "keep framing everything" -- while 100% (Ctrl+1), a wheel step, and a pan set it.
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
    // Ctrl+0: scales and centers so every item fits, clamped into the shared zoom bounds.
    void frameGraph();
    // Ctrl+1: exactly 100%, with the graph's bounding rectangle centered -- the same "actual size,
    // centered" the Viewer's 100% means.
    void zoomToActualSize();

  Q_SIGNALS:
    // Emitted instead of QGraphicsView's default "forward a context menu event into the scene"
    // behavior, which no item here consumes. `viewportPosition` is in viewport coordinates, so it
    // feeds itemAt()/mapToGlobal() directly.
    void contextMenuRequested(const QPoint& viewportPosition);
    void canvasKeyPressed(int key, Qt::KeyboardModifiers modifiers);
    void canvasFocusLost();

  protected:
    bool event(QEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void updatePanCursor();
    [[nodiscard]] QRectF graphBounds() const;
    // False when there is nothing to frame -- an empty graph, or a viewport with no area --
    // in which case the caller must not latch any "the artist moved the view" state.
    [[nodiscard]] bool applyCenteredScale(double scale);

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
    [[nodiscard]] QMenu* contextMenuForTest(bool nodeMenu = false);
    void openAddSearch(QPointF scenePosition, QPoint screenPosition,
                       std::optional<document::InputPortRef> input = {},
                       std::optional<document::OutputPortRef> output = {});

  private:
    void rebuild();
    void updateSelection();
    void sceneSelectionChanged();
    void showContextMenu(const QPoint& viewportPosition);
    [[nodiscard]] QMenu* buildContextMenu(QWidget* parent, bool nodeMenu = false);
    void handleCanvasKey(int key, Qt::KeyboardModifiers modifiers);
    // The node commands, each named for what it does (task S1, item 8). The keyboard and the
    // context menu call these; neither synthesizes a key press at the other, so a command can exist
    // in the menu without owning a key -- which is what mute, collapse and dissolve now are.
    [[nodiscard]] std::set<document::NodeId> commandTargets();
    void removeSelectedNodes();
    void duplicateSelectedNodes();
    void dissolveSelectedNode();
    // `muted` selects which layout flag is toggled: true for mute, false for collapse.
    void toggleSelectedMuted(bool muted);
    void renameSelectedLayer();
    void addNode(const QString& type);
    void showStatus(const QString& message);

    CompositionSession& session_;
    NodeGraphicsScene* scene_ = nullptr;
    NodeGraphicsView* view_ = nullptr;
    bool rebuilding_ = false;
    kit::KSearchPopup* search_ = nullptr;
    QPointF addPosition_;
    std::optional<document::InputPortRef> addInput_;
    std::optional<document::OutputPortRef> addOutput_;
    document::Revision addRevision_;
};

} // namespace bloom::ui
