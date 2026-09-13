#include <bloom/ui/node_editor.hpp>

#include <bloom/ui/composition_editors.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <bloom/ui/kit/color.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <bloom/core/color.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <QAction>
#include <QBrush>
#include <QColor>
#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsObject>
#include <QGraphicsPathItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointer>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyleOptionGraphicsItem>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "node_editor_interactions.hpp"

namespace bloom::ui {
using namespace node_editor;
namespace {
constexpr qreal kGridSpacing = kit::px(kit::Spacing::XXL);
constexpr qreal kGridMinimumDeviceSpacing = kit::px(kit::Spacing::M);
constexpr qreal kGridDotRadius = kit::kHairlineWidth;
} // namespace

// --- NodeGraphicsScene -------------------------------------------------------------------------

NodeGraphicsScene::NodeGraphicsScene(QObject* parent)
    : QGraphicsScene(parent), interaction_(std::make_unique<NodeInteraction>()) {
    setObjectName("nodeGraphicsScene");
    setBackgroundBrush(kit::color(kit::Color::Background));
    setItemIndexMethod(QGraphicsScene::BspTreeIndex);
}

void NodeGraphicsScene::setSession(CompositionSession* session) { session_ = session; }

void NodeGraphicsScene::drawBackground(QPainter* painter, const QRectF& rect) {
    // The canvas surface itself stays the scene's own Background brush; the grid is drawn on top of
    // it, one step up the surface ladder, so it reads as texture rather than as a second color.
    QGraphicsScene::drawBackground(painter, rect);

    const qreal scale = painter->worldTransform().m11();
    if (!(scale > 0.0) || kGridSpacing * scale < kGridMinimumDeviceSpacing) {
        return;
    }
    // Integer step counts, not a floating-point loop variable: repeatedly adding a pitch to a
    // double accumulates error across a wide exposed rectangle, and the dots would slowly drift off
    // the lattice the far side of the canvas is drawn on.
    const qreal first = std::floor(rect.left() / kGridSpacing) * kGridSpacing;
    const qreal top = std::floor(rect.top() / kGridSpacing) * kGridSpacing;
    const auto columns = static_cast<int>(std::floor((rect.right() - first) / kGridSpacing)) + 1;
    const auto lines = static_cast<int>(std::floor((rect.bottom() - top) / kGridSpacing)) + 1;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(kit::color(kit::surfaceStep(kit::Color::Background, 2)));
    for (int column = 0; column < columns; ++column) {
        const qreal x = first + static_cast<qreal>(column) * kGridSpacing;
        for (int line = 0; line < lines; ++line) {
            const qreal y = top + static_cast<qreal>(line) * kGridSpacing;
            painter->drawEllipse(QPointF(x, y), kGridDotRadius, kGridDotRadius);
        }
    }
    painter->restore();
}

void NodeGraphicsScene::setProjection(const document::Snapshot& snapshot,
                                      const document::CompositionId compositionId) {
    cancelGesture();
    const auto* composition = snapshot.project().findComposition(compositionId);
    if (composition == nullptr) {
        clear();
        setSceneRect({});
        return;
    }

    std::map<std::uint64_t, NodeItem*> existing;
    std::vector<QGraphicsItem*> staleEdges;
    for (auto* item : items()) {
        if (auto* node = dynamic_cast<NodeItem*>(item)) {
            existing.emplace(node->id().value(), node);
        } else if (dynamic_cast<NodeEdgeItem*>(item) != nullptr) {
            staleEdges.push_back(item);
        }
    }
    // Edges carry no widget state and no artist-owned position, so they are always rebuilt; node
    // cards are not, because theirs is exactly the state this reconciliation exists to preserve.
    for (auto* edge : staleEdges) {
        removeItem(edge);
        delete edge;
    }
    for (const auto& [id, item] : existing) {
        item->clearEdges();
    }

    const auto defaults = document::defaultNodeLayout(composition->graph().nodes());
    std::vector<std::uint64_t> present;
    present.reserve(composition->graph().nodes().size());
    for (const auto& node : composition->graph().nodes()) {
        const auto found = existing.find(node.id.value());
        NodeItem* item = nullptr;
        if (found != existing.end()) {
            item = found->second;
        } else {
            item = new NodeItem(node.id, session_);
            addItem(item);
        }
        const auto foundLayout = composition->nodeLayout().find(node.id);
        const auto& layout = foundLayout == composition->nodeLayout().end() ? defaults.at(node.id)
                                                                            : foundLayout->second;
        item->refresh(node, *composition, layout);
        item->setAuthoringEnabled(canSubmit());
        item->setPos(layout.position.x, layout.position.y);
        present.push_back(node.id.value());
    }

    for (const auto& [id, item] : existing) {
        if (std::ranges::find(present, id) == present.end()) {
            removeItem(item);
            // deleteLater(), not delete: a card can be dropped from inside one of its own field
            // widgets' signal emission (an edit that removes the node), and unwinding through a
            // freed widget is not something a projection may risk.
            item->deleteLater();
        }
    }

    rebuildGroups(*composition);
    rebuildEdges(*composition);
    updateGroupGeometry();
    const QRectF bounds = itemsBoundingRect();
    setSceneRect(bounds.isEmpty() ? QRectF(-kNodeSceneMargin, -kNodeSceneMargin,
                                           kNodeSceneMargin * 2.0, kNodeSceneMargin * 2.0)
                                  : bounds.adjusted(-kNodeSceneMargin, -kNodeSceneMargin,
                                                    kNodeSceneMargin, kNodeSceneMargin));
}

QGraphicsItem* NodeGraphicsScene::findNodeItem(const document::NodeId nodeId) const {
    const auto matching = items();
    const auto found = std::ranges::find_if(matching, [nodeId](const auto* item) {
        return item->data(kNodeItemKindRole).toString() == QStringLiteral("node") &&
               item->data(kNodeStableIdRole).toULongLong() == nodeId.value();
    });
    return found == matching.end() ? nullptr : *found;
}

QGraphicsItem* NodeGraphicsScene::findNodeGroupItem(const document::NodeGroupId groupId) const {
    const auto matching = items();
    const auto found = std::ranges::find_if(matching, [groupId](const auto* item) {
        return item->data(kNodeItemKindRole).toString() == QStringLiteral("node-group") &&
               item->data(kNodeStableIdRole).toULongLong() == groupId.value();
    });
    return found == matching.end() ? nullptr : *found;
}

// Frames are reconciled by their stable NodeGroupId exactly as cards are, because an inline title
// edit in flight has to survive the snapshot change its own rename produced.
void NodeGraphicsScene::rebuildGroups(const document::Composition& composition) {
    std::map<std::uint64_t, NodeGroupItem*> existing;
    for (auto* item : items())
        if (auto* group = dynamic_cast<NodeGroupItem*>(item))
            existing.emplace(group->id().value(), group);
    for (const auto& [id, record] : composition.nodeGroups()) {
        const auto found = existing.find(id.value());
        auto* item = found == existing.end() ? nullptr : found->second;
        if (item == nullptr) {
            item = new NodeGroupItem(id, session_);
            addItem(item);
        }
        item->refresh(record);
        item->setAuthoringEnabled(canSubmit());
    }
    for (const auto& [id, item] : existing) {
        if (composition.nodeGroups().contains(document::NodeGroupId::fromRaw(id)))
            continue;
        removeItem(item);
        // deleteLater(), not delete: a frame can be dropped from inside its own title editor's
        // signal emission -- the rename that ungrouped it -- and unwinding through a freed widget
        // is not something a projection may risk.
        item->deleteLater();
    }
}

// The frame is the bounding rectangle of its member cards plus the record's own padding, with room
// for the title strip above. A member the gesture in flight is dragging OUT is excluded, so the
// frame holds still and the artist can see where the card is landing; a gesture that carries the
// whole frame excludes nothing and the frame travels with its members.
void NodeGraphicsScene::updateGroupGeometry() {
    const bool frameDrag = interaction_->movedGroup.has_value();
    for (auto* item : items()) {
        auto* group = dynamic_cast<NodeGroupItem*>(item);
        if (group == nullptr)
            continue;
        std::optional<QRectF> settled;
        std::optional<QRectF> every;
        for (const auto id : group->members()) {
            const auto* card = dynamic_cast<NodeItem*>(findNodeItem(id));
            if (card == nullptr)
                continue;
            const QRectF rect = card->mapRectToScene(card->cardRect());
            every = every ? every->united(rect) : rect;
            if (!frameDrag && interaction_->positions.contains(id))
                continue;
            settled = settled ? settled->united(rect) : rect;
        }
        const auto bounds = settled ? settled : every;
        group->setVisible(bounds.has_value());
        if (!bounds)
            continue;
        const auto padding = group->padding();
        group->setFrameRect(
            bounds->adjusted(-padding.x, -padding.y - kGroupTitleHeight, padding.x, padding.y));
    }
}

QWidget* NodeGraphicsScene::nodeFieldForTest(const document::NodeId nodeId,
                                             const QString& fieldObjectName) const {
    auto* item = dynamic_cast<NodeItem*>(findNodeItem(nodeId));
    return item == nullptr ? nullptr : item->fieldWidget(fieldObjectName);
}

NodeSockets NodeGraphicsScene::nodeSocketsForTest(const document::NodeId nodeId) const {
    const auto* item = dynamic_cast<const NodeItem*>(findNodeItem(nodeId));
    return item == nullptr ? NodeSockets{}
                           : NodeSockets{item->hasInputSocket(), item->hasOutputSocket()};
}

void NodeGraphicsScene::rebuildEdges(const document::Composition& composition) {
    // Task FIX1, item A: a driven operand's link is recorded as its parameter's driver binding
    // rather than as an edge, and until now nothing drew it -- an artist who plugged a Scalar into
    // an opacity socket saw the socket's widget disappear and no wire at all, which is most of
    // "nodes exist but not usable". The canvas's one link list is therefore the graph's edges
    // FOLLOWED BY every driver binding, rendered as the same NodeEdgeItem so hover, selection
    // emphasis, cutting and the pick-up gesture all reach them without a second code path. A driver
    // link carries no EdgeId (there is no edge to carry one), so it is addressed by its
    // destination, which is what DisconnectInput already takes.
    std::vector<document::EdgeRecord> links(composition.graph().edges().begin(),
                                            composition.graph().edges().end());
    for (const auto& node : composition.graph().nodes()) {
        for (const auto& binding : node.parameters) {
            const auto* parameter = composition.parameters().find(binding.parameterId);
            const auto* driver =
                parameter == nullptr
                    ? nullptr
                    : std::get_if<document::DriverBindingSource>(&parameter->source);
            if (driver == nullptr)
                continue;
            links.push_back({document::EdgeId{},
                             document::OutputPortRef{driver->sourceNodeId, driver->outputPort},
                             document::NodeInputRef{node.id, binding.role}});
        }
    }
    for (const auto& edge : links) {
        auto* source = dynamic_cast<NodeItem*>(findNodeItem(edge.source.nodeId));
        auto* destination =
            dynamic_cast<NodeItem*>(findNodeItem(destinationNodeId(edge.destination)));
        if (source != nullptr && destination != nullptr) {
            SocketItem* output = nullptr;
            SocketItem* input = nullptr;
            for (auto* socket : source->sockets())
                if (socket->output == edge.source)
                    output = socket;
            for (auto* socket : destination->sockets())
                // accepts(), not an equality test: the Merge node's one ordered multi-input stands
                // for every stack slot, so every edge terminating on any of them terminates here.
                if (socket->accepts(edge.destination))
                    input = socket;
            if (output && input)
                // Task FIX1, item C: no link is structural any more. Every one of them can be
                // picked up, cut, or disconnected from its own context menu, because the two that
                // could not be -- a Layer's boundary output and Merge's stack slot -- are now
                // created and removed by connecting and disconnecting them.
                addItem(new NodeEdgeItem(*source, *destination, *output, *input, edge, false));
        }
    }
}

// --- NodeGraphEditor ---------------------------------------------------------------------------

NodeGraphEditor::NodeGraphEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("nodeGraphEditor");
    setAccessibleName(tr("Nodes editor"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    scene_ = new NodeGraphicsScene(this);
    scene_->setSession(&session_);
    // Production submission (task N3): every gesture transaction goes through the session's one
    // public node-transaction seam, so publication and refusal reporting match Properties and the
    // Timeline. Tests may replace this with an adapter through setSubmit().
    scene_->setSubmit([this](commands::Transaction&& transaction) {
        return session_.executeNodeTransaction(std::move(transaction));
    });
    view_ = new NodeGraphicsView(this);
    view_->setScene(scene_);
    layout->addWidget(view_);

    connect(&session_, &CompositionSession::snapshotChanged, this, &NodeGraphEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this, &NodeGraphEditor::rebuild);
    connect(&session_, &CompositionSession::selectionChanged, this,
            &NodeGraphEditor::updateSelection);
    connect(scene_, &QGraphicsScene::selectionChanged, this,
            &NodeGraphEditor::sceneSelectionChanged);
    connect(view_, &NodeGraphicsView::contextMenuRequested, this,
            &NodeGraphEditor::showContextMenu);

    connect(view_, &NodeGraphicsView::canvasKeyPressed, this, &NodeGraphEditor::handleCanvasKey);
    connect(view_, &NodeGraphicsView::canvasFocusLost, scene_, &NodeGraphicsScene::cancelGesture);
    connect(scene_, &NodeGraphicsScene::addSearchRequested, this, &NodeGraphEditor::openAddSearch);
    rebuild();
}

NodeGraphEditor::~NodeGraphEditor() { scene_->blockSignals(true); }

NodeGraphicsScene* NodeGraphEditor::graphScene() const noexcept { return scene_; }

NodeGraphicsView* NodeGraphEditor::graphView() const noexcept { return view_; }

void NodeGraphEditor::rebuild() {
    rebuilding_ = true;
    scene_->setProjection(session_.snapshot(), session_.compositionId());
    updateSelection();
    rebuilding_ = false;
    if (!view_->viewAdjusted()) {
        view_->frameGraph();
    }
}

void NodeGraphEditor::updateSelection() {
    const bool wasRebuilding = rebuilding_;
    rebuilding_ = true;
    for (auto* candidate : scene_->items()) {
        auto* item = dynamic_cast<NodeItem*>(candidate);
        if (!item)
            continue;
        item->setSelected(session_.selectedNodes().contains(item->id()));
        const auto* selected = session_.selectedNode();
        item->setPrimary(selected && selected->id == item->id());
    }
    rebuilding_ = wasRebuilding;
}

void NodeGraphEditor::sceneSelectionChanged() {
    if (rebuilding_) {
        return;
    }
    std::set<document::NodeId> nodes;
    for (auto* candidate : scene_->selectedItems())
        if (auto* item = dynamic_cast<NodeItem*>(candidate))
            nodes.insert(item->id());
    if (nodes.empty()) {
        session_.clearSelection();
        return;
    }
    const auto* primary = session_.selectedNode();
    session_.selectNodes(nodes,
                         primary && nodes.contains(primary->id) ? primary->id : *nodes.begin());
}

} // namespace bloom::ui
