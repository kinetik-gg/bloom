#pragma once

#include <array>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/preferences_aware.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <QGraphicsScene>
#include <QGraphicsView>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QTransform>
#include <QWidget>

class QAction;
class QContextMenuEvent;
class QKeyEvent;
class QLabel;
class QMenu;
class QMouseEvent;
class QPainter;
class QResizeEvent;
class QShowEvent;
class QWheelEvent;

namespace bloom::ui::kit {
class KDropdown;
class KSwitch;
} // namespace bloom::ui::kit

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

// The wire's own drawing convention (task NODES-1). Spline is the original cubic bezier; Straight
// is a direct line socket-to-socket; Angled is an orthogonal horizontal-vertical-horizontal path.
// Persisted per node editor instance (QSettings "nodes/link-style") and shared by every
// NodeEdgeItem's own path and the drag preview link, so a style change repaints both alike.
// Declared in bloom::ui rather than bloom::ui::node_editor (node_editor_items.hpp's own
// namespace) so NodeGraphicsScene, defined in this header, can hold and expose it without this
// header needing node_editor_items.hpp's forward declarations; node_editor_items.hpp's code sees
// it unqualified through ordinary enclosing-namespace lookup.
enum class LinkStyle : std::uint8_t { Spline, Straight, Angled };

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
    // Shift + right drag crossed a link and asks for a reroute at that point (task FIX1, item I).
    void rerouteRequested(document::InputPortRef input, QPointF scenePosition);

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
    // Re-reads every card's VALUES and keyframe diamonds from the current snapshot, leaving the
    // projection's structure -- cards, sockets, links, frames, the scene rectangle -- exactly where
    // it is. The session TIME is not a document change, but it IS what an animated parameter's
    // shown value and its diamond are read at, so a playhead move has to reach the cards; doing it
    // through setProjection() would rebuild every edge and re-frame the canvas once per frame of
    // playback for a question only the rows ask.
    void refreshValues();
    [[nodiscard]] QGraphicsItem* findNodeItem(document::NodeId nodeId) const;
    [[nodiscard]] QGraphicsItem* findNodeGroupItem(document::NodeGroupId groupId) const;
    // Recomputes every group frame from the live member cards. Called by the projection and by
    // every move in flight, which is what makes a frame follow its members rather than a saved
    // rectangle.
    void updateGroupGeometry();

    // Test/diagnostic surface only, mirroring ViewerEditor's own *ForTest precedent: an in-node kit
    // field lives inside a QGraphicsProxyWidget, so it is not a QWidget child of the view and
    // findChild() cannot reach it.
    [[nodiscard]] QWidget* nodeFieldForTest(document::NodeId nodeId,
                                            const QString& fieldObjectName) const;
    // Direction summary of the actual per-port child items, retained as a diagnostic contract.
    [[nodiscard]] NodeSockets nodeSocketsForTest(document::NodeId nodeId) const;

    // Task NODES-1, deliverable 2: the link style every edge draws with. Changing it walks every
    // NodeEdgeItem currently in the scene and repaints it in place -- a fresh edge (rebuildEdges())
    // reads the same member at construction, so the two can never disagree.
    void setLinkStyle(LinkStyle style);
    [[nodiscard]] LinkStyle linkStyle() const noexcept { return linkStyle_; }

    // Task NODES-1, deliverable 3: grid snapping. Applies to a node drag's live position and to the
    // position MoveNodes commits, in both cases unless Alt is held at the moment. Disabled by
    // default -- see node_interaction_tests.cpp's drag fixture, which is pinned to exact,
    // deliberately non-grid-aligned pixel positions and would otherwise silently start failing the
    // moment this feature landed.
    void setGridSnapEnabled(bool enabled) noexcept { snapEnabled_ = enabled; }
    [[nodiscard]] bool gridSnapEnabled() const noexcept { return snapEnabled_; }
    // The dot grid drawBackground() paints steps by this same size, in scene units. Clamped to a
    // sane positive floor so a bad persisted setting cannot divide by zero or invert the lattice.
    void setGridSize(qreal size);
    [[nodiscard]] qreal gridSize() const noexcept { return gridSize_; }

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
    void rebuildGroups(const document::Composition& composition);

    CompositionSession* session_ = nullptr;
    Submit submit_;
    std::unique_ptr<NodeInteraction> interaction_;
    LinkStyle linkStyle_ = LinkStyle::Spline;
    bool snapEnabled_ = false;
    qreal gridSize_ = kit::px(kit::Size::NodeGrid);
};

// The graph canvas. Its navigation conventions are the Viewer's, deliberately and structurally:
// the wheel step factor and the zoom bounds are the SAME named constants viewer_editor.hpp
// publishes (kZoomStepFactor, ViewTransform::kMinZoom/kMaxZoom), so the two canvases cannot drift
// apart by someone re-spelling a number. Wheel zooms about the cursor, a
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
    // The footer's zoom dropdown (task NODES-1, deliverable 4): `percent` is one of the Viewer's
    // own fixed presets (25/50/100/200/400 -- viewer_editor.cpp's kZoomPresets). Same
    // centered-scale behavior as zoomToActualSize(), generalized to an arbitrary factor.
    void zoomToPercent(int percent);
    // View > Frame Selected (task NODES-1): scales and centers so exactly `bounds` (already in
    // scene coordinates -- the caller's job to compute, since only it knows which cards are
    // selected) fits, clamped into the shared zoom bounds. Unlike Fit, this counts as the artist
    // having taken the view over: framing one selection is a deliberate placement, not "keep
    // following the whole graph".
    void frameRect(QRectF bounds);

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
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void updatePanCursor();
    [[nodiscard]] QRectF graphBounds() const;
    // False when there is nothing to frame -- an empty `bounds`, or a viewport with no area --
    // in which case the caller must not latch any "the artist moved the view" state.
    [[nodiscard]] bool applyCenteredScale(const QRectF& bounds, double scale);

    bool panActive_ = false;
    Qt::MouseButton panButton_ = Qt::NoButton;
    QPointF panOrigin_;
    // The transform frozen at pan begin: every move applies the TOTAL displacement from the press
    // point to it, never a chain of already-rounded per-move deltas -- the Viewer's own pan rule.
    QTransform panBaseTransform_;
    bool viewAdjusted_ = false;
    bool framedOnce_ = false;
};

// Task NODES-1: the node editor is the first to offer BOTH the header menus and the footer, on the
// same terms EditorArea already gives ViewerEditor for the footer alone -- each interface is taken
// at most once, the moment EditorArea creates this widget, and either can legitimately be absent
// (a test that constructs a NodeGraphEditor directly, outside an EditorArea, never calls either).
class NodeGraphEditor final : public QWidget, public EditorChromeProvider, public PreferencesAware {
    Q_OBJECT

  public:
    [[nodiscard]] EditorChromeSpec& editorChrome() override { return chrome_; }
    explicit NodeGraphEditor(CompositionSession& session, QWidget* parent = nullptr);
    ~NodeGraphEditor() override;

    [[nodiscard]] NodeGraphicsScene* graphScene() const noexcept;
    [[nodiscard]] NodeGraphicsView* graphView() const noexcept;

    // Applies the Settings window's committed node-graph preferences through the scene's own
    // setters.
    void applyApplicationPreferences(const ApplicationPreferences& preferences) override;

    // Test/diagnostic surface only: the exact menu a right-click builds, parented to this widget
    // and never shown. Lets a test enumerate what the canvas offers -- and, just as importantly,
    // what it honestly does not.
    // `group` asks for the menu a right-click on that group's own frame offers.
    [[nodiscard]] QMenu* contextMenuForTest(bool nodeMenu = false,
                                            std::optional<document::NodeGroupId> group = {});
    // The menu a right-click on the LINK under `viewportPosition` offers, or null if no link is
    // there. Same surface rule as contextMenuForTest(): built, never shown.
    [[nodiscard]] QMenu* linkContextMenuForTest(QPoint viewportPosition);
    void openAddSearch(QPointF scenePosition, QPoint screenPosition,
                       std::optional<document::InputPortRef> input = {},
                       std::optional<document::OutputPortRef> output = {});

    // idempotent: a second call, on either, returns nullptr -- the FORMAL AMENDMENT 1 contract

    // Test/diagnostic surface only, mirroring contextMenuForTest(): the persistent Add/View/
    // Select/Node menus this editor builds once and keeps live, regardless of whether

    // "select", "node".
    [[nodiscard]] QMenu* headerMenuForTest(std::string_view which) const;
    // Test/diagnostic surface only: the footer widget's own child controls, by objectName, without
    // requiring an EditorArea to host them first.
    [[nodiscard]] QWidget* footerWidgetForTest();

    // The QSettings "nodes/link-style" string <-> LinkStyle mapping (task NODES-1): static so both
    // this translation unit's constructor (the initial read) and node_editor_menus.cpp's
    // applyLinkStyle() (every write thereafter) share the one definition.
    [[nodiscard]] static LinkStyle linkStyleFromSettingsValue(const QString& value) noexcept;
    [[nodiscard]] static QString linkStyleSettingsValue(LinkStyle style) noexcept;

  private:
    EditorChromeSpec chrome_;
    void rebuild();
    void updateSelection();
    void sceneSelectionChanged();
    void showContextMenu(const QPoint& viewportPosition);
    [[nodiscard]] QMenu* buildContextMenu(QWidget* parent, bool nodeMenu = false,
                                          std::optional<document::NodeGroupId> group = {});
    // The menu for one link (task FIX1, item C). Null when nothing under the point is a link, which
    // is what tells showContextMenu() to fall through to the canvas or card menu.
    [[nodiscard]] QMenu* buildLinkContextMenu(QWidget* parent, QPoint viewportPosition);
    void disconnectLink(document::InputPortRef input);
    // Task FIX1, item I: inserts a reroute into the link that ends at `input`, at `scenePosition`.
    // Both creation gestures -- the link's own menu and Shift + right drag across it -- call this.
    void insertReroute(document::InputPortRef input, QPointF scenePosition);
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
    void arrangeAllNodes();
    void arrangeSelectedNodes();
    // Ctrl+G / Ctrl+Shift+G, and the same two commands the context menu reaches. Ungrouping with no
    // explicit frame removes every frame the selection sits in.
    void groupSelectedNodes();
    void ungroupSelection(std::optional<document::NodeGroupId> group = {});
    void addNode(const QString& type);
    void showStatus(const QString& message);

    // --- Task NODES-1: View/Select commands the header menus reach that the canvas did not have a
    // command for before (Fit, Actual Size, Zoom In/Out, Group, Ungroup, Mute, Collapse, Rename,
    // Dissolve, Delete, Select All all already existed as commands or inline lambdas above). ---
    void frameSelectedNodes();
    void selectNoneNodes();
    void selectInvertNodes();
    void selectLinkedUpstream();
    void selectLinkedDownstream();
    // Shared BFS the two linked-selection commands above both walk, over the SAME link set
    // rebuildEdges() draws (graph edges followed by every driver binding) so "linked" here can
    // never disagree with what the canvas actually shows as a wire. Returns `seeds` itself united
    // with whatever following edges (upstream: toward sources; downstream: toward destinations)
    // reaches transitively.
    [[nodiscard]] std::set<document::NodeId> linkedNodes(const std::set<document::NodeId>& seeds,
                                                         bool upstream) const;

    // --- Task NODES-1: the persistent header menus (deliverable 1) and their live state. ---
    void buildHeaderMenus();
    void buildFooter();
    // The categorized Add submenu's actual contents: one call from buildContextMenu() (transient,
    // rebuilt fresh per popup, exactly as before this task) and one from buildHeaderMenus() (the
    // persistent header Add menu, built ONCE -- `recordInto`, when given, collects each leaf
    // action alongside its type id so refreshAddMenuState() can update enabled/tooltip state in
    // place afterward without rebuilding the tree, which would otherwise leak a QMenu per category
    // every time the menu is reopened: QMenu::clear() does not delete a submenu it did not itself
    // parent, and addMenu(QString) parents the submenu to ITSELF, not to the menu clear() is called
    // on).
    void populateAddMenu(QMenu* menu,
                         std::vector<std::pair<QAction*, std::string>>* recordInto = nullptr);
    void refreshAddMenuState();
    // The one place Add's own refusal text is computed, from a private draft exactly like a dry
    // run of the command itself -- shared by populateAddMenu() (a fresh menu's initial state) and
    // refreshAddMenuState() (the persistent header menu's state, refreshed on every aboutToShow).
    [[nodiscard]] QString addNodeRefusal(const std::string& typeId) const;
    void refreshViewMenuState();
    void refreshSelectMenuState();
    void refreshNodeMenuState();
    void applyLinkStyle(LinkStyle style);
    void applyGridSnap(bool enabled);
    void refreshSelectionReadout();

    CompositionSession& session_;
    NodeGraphicsScene* scene_ = nullptr;
    NodeGraphicsView* view_ = nullptr;
    bool rebuilding_ = false;
    kit::KSearchPopup* search_ = nullptr;
    QPointF addPosition_;
    std::optional<document::InputPortRef> addInput_;
    std::optional<document::OutputPortRef> addOutput_;
    document::Revision addRevision_;

    // Header menus (task NODES-1, deliverable 1): built once in buildHeaderMenus(), lived in for
    // the editor's whole lifetime regardless of whether EditorArea ever calls

    // action below reuses one of the objectNames the canvas context menu already established
    // wherever the same command applies; the exceptions (Select None/Invert/Linked Upstream/Linked
    // Downstream, Frame Selected, Grid Snapping, Link Style) are new and documented in this task's
    // report.
    QMenu* headerAddMenu_ = nullptr;
    QMenu* headerViewMenu_ = nullptr;
    QMenu* headerSelectMenu_ = nullptr;
    QMenu* headerNodeMenu_ = nullptr;
    QWidget* headerMenuWidget_ = nullptr;
    QAction* gridSnapAction_ = nullptr;
    std::array<QAction*, 3> linkStyleActions_{};
    QAction* groupAction_ = nullptr;
    QAction* ungroupAction_ = nullptr;
    QAction* muteAction_ = nullptr;
    QAction* collapseAction_ = nullptr;
    QAction* renameAction_ = nullptr;
    QAction* dissolveAction_ = nullptr;
    QAction* deleteAction_ = nullptr;
    QAction* arrangeAllAction_ = nullptr;
    QAction* arrangeSelectionAction_ = nullptr;
    // Recorded by populateAddMenu() when it builds headerAddMenu_, so refreshAddMenuState() can
    // update enabled/tooltip state per item without rebuilding the (structurally static, since the
    // node definition registry is frozen at startup) category tree.
    std::vector<std::pair<QAction*, std::string>> addMenuItems_;

    // Footer (task NODES-1, deliverable 4): built once in the constructor, handed away by

    // are: the zoom dropdown and selection readout both need to react to session/view state that
    // outlives any one popup.
    QWidget* footerWidget_ = nullptr;
    kit::KDropdown* footerZoomDropdown_ = nullptr;
    kit::KSwitch* footerSnapSwitch_ = nullptr;
    kit::KDropdown* footerLinkStyleDropdown_ = nullptr;
    QLabel* footerSelectionLabel_ = nullptr;
};

} // namespace bloom::ui
