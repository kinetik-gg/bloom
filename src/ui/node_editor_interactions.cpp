#include "node_editor_interactions.hpp"
#include <QPainterPathStroker>

namespace bloom::ui {
using namespace node_editor;
namespace {
SocketItem* socketAt(QGraphicsScene& scene, const QPointF point) {
    SocketItem* closest = nullptr;
    qreal distance = 1e30;
    for (auto* item : scene.items(point)) {
        auto* socket = dynamic_cast<SocketItem*>(item);
        if (!socket)
            continue;
        const qreal candidate = QLineF(point, socket->scenePos()).length();
        if (candidate < distance) {
            closest = socket;
            distance = candidate;
        }
    }
    return closest;
}
NodeItem* cardAt(QGraphicsScene& scene, const QPointF point) {
    for (auto* item : scene.items(point))
        if (auto* card = nodeItemAncestor(item))
            return card;
    return nullptr;
}
// The innermost frame under `point`: with nested frames the smallest one wins, so a drop has
// exactly one answer instead of depending on scene item order.
NodeGroupItem* groupAt(QGraphicsScene& scene, const QPointF point) {
    NodeGroupItem* innermost = nullptr;
    for (auto* item : scene.items(point)) {
        auto* group = groupItemAncestor(item);
        if (group == nullptr || !group->isVisible())
            continue;
        const auto area = group->frameRect().width() * group->frameRect().height();
        if (innermost == nullptr ||
            area < innermost->frameRect().width() * innermost->frameRect().height())
            innermost = group;
    }
    return innermost;
}
// Which frame a dropped card's CENTER lands in, by the same innermost rule. Membership is judged on
// the center rather than on any overlap, so one card has one landing.
std::optional<document::NodeGroupId> groupForDrop(QGraphicsScene& scene, const QPointF center) {
    auto* group = groupAt(scene, center);
    return group == nullptr ? std::nullopt : std::optional(group->id());
}
bool fieldAt(QGraphicsScene& scene, const QPointF point) {
    for (auto* item : scene.items(point))
        for (auto* parent = item; parent; parent = parent->parentItem())
            if (qgraphicsitem_cast<QGraphicsProxyWidget*>(parent))
                return true;
    return false;
}
const document::EdgeRecord* inputEdge(const document::Composition& composition,
                                      const document::InputPortRef& input) {
    const auto edges = composition.graph().edges();
    const auto found = std::ranges::find(edges, input, &document::EdgeRecord::destination);
    return found == edges.end() ? nullptr : &*found;
}
SocketItem* findOutput(QGraphicsScene& scene, const document::OutputPortRef& output) {
    for (auto* item : scene.items())
        if (auto* socket = dynamic_cast<SocketItem*>(item); socket && socket->output == output)
            return socket;
    return nullptr;
}
std::pair<SocketItem*, SocketItem*> insertionSockets(NodeItem& card,
                                                     const document::Composition& composition) {
    SocketItem* input = nullptr;
    SocketItem* output = nullptr;
    for (auto* socket : card.sockets()) {
        if (socket->kind != document::SocketValueKind::Image)
            continue;
        if (socket->input && !input)
            input = socket;
        if (socket->output && !output)
            output = socket;
    }
    if (!input || !output || !input->draggable() || !output->draggable() || !input->input ||
        !output->output || inputEdge(composition, *input->input))
        return {};
    return {input, output};
}
// Tells every socket on the canvas whether the link now being dragged could land on it (task S1,
// item 6). The socket the drag STARTED from keeps its resting ink: it is the thing in the artist's
// hand, not a candidate to aim at.
void markLinkAffinity(QGraphicsScene& scene, const NodeInteraction& gesture,
                      const SocketItem* origin) {
    const bool fromOutput = gesture.output.has_value();
    for (auto* item : scene.items()) {
        auto* socket = dynamic_cast<SocketItem*>(item);
        if (socket == nullptr)
            continue;
        if (socket == origin) {
            socket->setDragAffinity(SocketItem::DragAffinity::Idle);
            continue;
        }
        const bool opposite = fromOutput ? socket->input.has_value() : socket->output.has_value();
        const bool compatible = socket->draggable() && opposite && socket->kind == gesture.linkKind;
        socket->setDragAffinity(compatible ? SocketItem::DragAffinity::Compatible
                                           : SocketItem::DragAffinity::Incompatible);
    }
}

void clearLinkAffinity(QGraphicsScene& scene) {
    for (auto* item : scene.items()) {
        if (auto* socket = dynamic_cast<SocketItem*>(item)) {
            socket->setDragAffinity(SocketItem::DragAffinity::Idle);
            socket->setDropIndicator(std::nullopt);
        }
    }
}

// Marks, on the Merge node's ordered multi-input, which position in the stack order the pointer is
// currently at (task S1, item 7). The pill is simultaneously dimmed as incompatible by
// markLinkAffinity() above -- a stack slot is structural and accepts no drop -- so the caret
// reports the pointer's position in the order and never promises a landing.
void updateOrderedDropIndicator(QGraphicsScene& scene, const QPointF cursor) {
    for (auto* item : scene.items()) {
        auto* socket = dynamic_cast<SocketItem*>(item);
        if (socket == nullptr || !socket->multiInput())
            continue;
        socket->setDropIndicator(socket->slotIndexAt(socket->mapFromScene(cursor)));
    }
}

void previewInsertion(QGraphicsScene& scene, NodeInteraction& gesture,
                      const document::Composition& composition, const QPointF cursor) {
    if (gesture.insertEdge)
        gesture.insertEdge->emphasize(false);
    gesture.insertEdge = nullptr;
    if (gesture.positions.size() != 1)
        return;
    auto* card = cardAt(scene, cursor);
    if (!card || !gesture.positions.contains(card->id()))
        return;
    if (!insertionSockets(*card, composition).first)
        return;
    for (auto* item : scene.items(cursor)) {
        auto* edge = dynamic_cast<NodeEdgeItem*>(item);
        if (!edge || edge->structural || edge->edge.source.nodeId == card->id() ||
            destinationNodeId(edge->edge.destination) == card->id())
            continue;
        gesture.insertEdge = edge;
        edge->emphasize(true);
        break;
    }
}
} // namespace

NodeGraphicsScene::~NodeGraphicsScene() { cancelGesture(); }
void NodeGraphicsScene::setSubmit(Submit submit) {
    submit_ = std::move(submit);
    for (auto* item : items()) {
        if (auto* card = dynamic_cast<NodeItem*>(item))
            card->setAuthoringEnabled(canSubmit());
        else if (auto* group = dynamic_cast<NodeGroupItem*>(item))
            group->setAuthoringEnabled(canSubmit());
    }
}
commands::CommandResult NodeGraphicsScene::submit(commands::Transaction&& transaction) {
    if (submit_)
        return submit_(std::move(transaction));
    commands::CommandResult result;
    result.status = commands::CommandStatus::Rejected;
    return result;
}
bool NodeGraphicsScene::gestureActive() const {
    return interaction_->mode != NodeInteraction::Mode::Idle;
}
void NodeGraphicsScene::cancelGesture() {
    auto& gesture = *interaction_;
    clearLinkAffinity(*this);
    if (gesture.insertEdge)
        gesture.insertEdge->emphasize(false);
    delete gesture.line;
    delete gesture.box;
    for (const auto& [id, point] : gesture.positions) {
        if (auto* card = dynamic_cast<NodeItem*>(findNodeItem(id))) {
            card->setDragging(false);
            card->setPos(point);
        }
    }
    if (gesture.mode == NodeInteraction::Mode::Resize)
        if (auto* card = dynamic_cast<NodeItem*>(findNodeItem(gesture.resized)))
            card->setPreviewWidth(gesture.width);
    for (auto* item : items())
        if (dynamic_cast<NodeEdgeItem*>(item))
            item->show();
    gesture = {};
    updateGroupGeometry();
}
void NodeGraphicsScene::selectAllNodes() {
    if (!session_ || !session_->composition())
        return;
    std::set<document::NodeId> nodes;
    for (const auto& node : session_->composition()->graph().nodes())
        nodes.insert(node.id);
    if (!nodes.empty())
        session_->selectNodes(nodes, *nodes.begin());
}
void NodeGraphicsScene::startDuplicateMove(const QPointF scenePosition) {
    if (!session_ || !submit_)
        return;
    cancelGesture();
    auto& gesture = *interaction_;
    gesture.mode = NodeInteraction::Mode::Move;
    gesture.revision = session_->snapshot().revision();
    gesture.origin = scenePosition;
    gesture.floating = true;
    for (const auto id : session_->selectedNodes())
        if (auto* item = findNodeItem(id))
            gesture.positions.emplace(id, item->pos());
}

void NodeGraphicsScene::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    if (!session_ || !session_->composition()) {
        QGraphicsScene::mousePressEvent(event);
        return;
    }
    auto& gesture = *interaction_;
    if (gesture.floating && event->button() == Qt::LeftButton) {
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton && gestureActive()) {
        cancelGesture();
        event->accept();
        return;
    }
    if (submit_ && event->button() == Qt::RightButton &&
        event->modifiers() == Qt::ControlModifier) {
        cancelGesture();
        gesture.mode = NodeInteraction::Mode::Cut;
        gesture.origin = event->scenePos();
        gesture.revision = session_->snapshot().revision();
        gesture.line =
            addPath(QPainterPath(gesture.origin), QPen(kit::color(kit::Color::Error), 2));
        gesture.line->setZValue(10);
        gesture.line->setData(kNodeItemKindRole, QStringLiteral("cut-preview"));
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QGraphicsScene::mousePressEvent(event);
        return;
    }
    if (!fieldAt(*this, event->scenePos()))
        clearFocus();
    if (auto* socket = socketAt(*this, event->scenePos())) {
        event->accept();
        if (!submit_ || !socket->draggable())
            return;
        cancelGesture();
        gesture.mode = NodeInteraction::Mode::Link;
        gesture.revision = session_->snapshot().revision();
        gesture.origin = socket->scenePos();
        gesture.input = socket->input;
        gesture.output = socket->output;
        if (socket->input) {
            if (const auto* edge = inputEdge(*session_->composition(), *socket->input)) {
                auto* source = findOutput(*this, edge->source);
                if (!source || !source->draggable()) {
                    cancelGesture();
                    return;
                }
                gesture.pickedInput = socket->input;
                gesture.input.reset();
                gesture.output = edge->source;
                gesture.origin = source->scenePos();
                for (auto* item : items())
                    if (auto* link = dynamic_cast<NodeEdgeItem*>(item);
                        link && link->edge.id == edge->id)
                        link->hide();
            }
        }
        gesture.linkKind = socket->kind;
        gesture.line = addPath({}, QPen(kit::color(socketColorToken(gesture.linkKind)), 2));
        gesture.line->setZValue(10);
        gesture.line->setData(kNodeItemKindRole, QStringLiteral("link-preview"));
        markLinkAffinity(*this, gesture, socket);
        return;
    }
    if (auto* card = cardAt(*this, event->scenePos())) {
        if (event->modifiers().testFlag(Qt::ShiftModifier)) {
            session_->toggleNodeSelection(card->id());
            event->accept();
            return;
        }
        auto selection = session_->selectedNodes();
        if (!selection.contains(card->id()))
            selection = {card->id()};
        session_->selectNodes(selection, card->id());
        event->accept();
        if (!submit_)
            return;
        gesture.revision = session_->snapshot().revision();
        gesture.origin = event->scenePos();
        const QPointF local = card->mapFromScene(event->scenePos());
        if (std::abs(local.x() - card->cardWidth()) <= 6) {
            gesture.mode = NodeInteraction::Mode::Resize;
            gesture.resized = card->id();
            gesture.width = card->cardWidth();
        } else {
            gesture.mode = NodeInteraction::Mode::Move;
            for (const auto id : selection)
                if (auto* item = findNodeItem(id))
                    gesture.positions.emplace(id, item->pos());
        }
        return;
    }
    if (auto* frame = groupAt(*this, event->scenePos()); frame != nullptr) {
        // The frame's own title editor keeps its clicks, exactly as an in-card field does: without
        // this the press would fall through to a box selection and take the caret with it.
        if (fieldAt(*this, event->scenePos())) {
            QGraphicsScene::mousePressEvent(event);
            return;
        }
        // Clicking a frame selects what it frames. The frame itself is not a document selection:
        // CompositionSession owns one selection truth and it is made of NodeIds.
        std::set<document::NodeId> members;
        for (const auto id : frame->members())
            if (findNodeItem(id) != nullptr)
                members.insert(id);
        if (!members.empty())
            session_->selectNodes(members, *members.begin());
        event->accept();
        if (!submit_ || members.empty())
            return;
        cancelGesture();
        gesture.mode = NodeInteraction::Mode::Move;
        gesture.movedGroup = frame->id();
        gesture.revision = session_->snapshot().revision();
        gesture.origin = event->scenePos();
        for (const auto id : members)
            if (auto* item = findNodeItem(id))
                gesture.positions.emplace(id, item->pos());
        return;
    }
    cancelGesture();
    gesture.mode = NodeInteraction::Mode::Box;
    gesture.origin = event->scenePos();
    if (event->modifiers().testFlag(Qt::ShiftModifier))
        gesture.baseSelection = session_->selectedNodes();
    else
        session_->clearSelection();
    gesture.box = addRect({}, QPen(kit::color(kit::Color::Accent), 1));
    gesture.box->setZValue(10);
    gesture.box->setData(kNodeItemKindRole, QStringLiteral("selection-preview"));
    event->accept();
}

void NodeGraphicsScene::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) {
    // A double-click inside a hosted field belongs to the field (it selects a word); only a click
    // on the card itself is a rename gesture.
    if (!fieldAt(*this, event->scenePos())) {
        if (auto* card = cardAt(*this, event->scenePos())) {
            cancelGesture();
            card->startRename();
            event->accept();
            return;
        }
        if (auto* frame = groupAt(*this, event->scenePos())) {
            cancelGesture();
            frame->startRename();
            event->accept();
            return;
        }
    }
    QGraphicsScene::mouseDoubleClickEvent(event);
}

void NodeGraphicsScene::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
    auto& gesture = *interaction_;
    if (!gestureActive()) {
        QGraphicsScene::mouseMoveEvent(event);
        return;
    }
    event->accept();
    const QPointF delta = event->scenePos() - gesture.origin;
    switch (gesture.mode) {
    case NodeInteraction::Mode::Move:
        for (const auto& [id, position] : gesture.positions)
            if (auto* card = dynamic_cast<NodeItem*>(findNodeItem(id))) {
                card->setDragging(true);
                card->setPos(position + delta);
            }
        previewInsertion(*this, gesture, *session_->composition(), event->scenePos());
        updateGroupGeometry();
        break;
    case NodeInteraction::Mode::Resize:
        if (auto* card = dynamic_cast<NodeItem*>(findNodeItem(gesture.resized)))
            // Clamped against the card's OWN content floor, not a spelled 128: a card whose label
            // column and narrowest field need more than that cannot be dragged narrower than the
            // content it carries (task S1, item 2).
            card->setPreviewWidth(std::max(card->minimumCardWidth(), gesture.width + delta.x()));
        break;
    case NodeInteraction::Mode::Box:
        gesture.box->setRect(QRectF(gesture.origin, event->scenePos()).normalized());
        break;
    case NodeInteraction::Mode::Cut: {
        auto path = gesture.line->path();
        // QGraphicsPathItem may coalesce a move-only path with its empty default. Restore the
        // press point before the first segment so a cut never starts at the scene origin.
        if (path.isEmpty())
            path.moveTo(gesture.origin);
        path.lineTo(event->scenePos());
        gesture.line->setPath(path);
        break;
    }
    case NodeInteraction::Mode::Link: {
        const auto* target = socketAt(*this, event->scenePos());
        const bool incompatible =
            target && (!target->draggable() || (gesture.output && !target->input) ||
                       (gesture.input && !target->output));
        gesture.line->setPen(QPen(
            kit::color(incompatible ? kit::Color::Error : socketColorToken(gesture.linkKind)), 2));
        gesture.line->setPath(gesture.output ? linkPath(gesture.origin, event->scenePos())
                                             : linkPath(event->scenePos(), gesture.origin));
        updateOrderedDropIndicator(*this, event->scenePos());
        break;
    }
    case NodeInteraction::Mode::Idle:
        break;
    }
}

void NodeGraphicsScene::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    auto& gesture = *interaction_;
    if (!gestureActive()) {
        QGraphicsScene::mouseReleaseEvent(event);
        return;
    }
    event->accept();
    if (gesture.mode == NodeInteraction::Mode::Box) {
        auto selected = gesture.baseSelection;
        for (auto* item : items(gesture.box->rect(), Qt::IntersectsItemShape))
            if (auto* card = dynamic_cast<NodeItem*>(item))
                selected.insert(card->id());
        cancelGesture();
        if (selected.empty())
            session_->clearSelection();
        else
            session_->selectNodes(selected, *selected.begin());
        return;
    }
    commands::Transaction transaction("Edit Nodes", gesture.revision);
    const auto compositionId = session_->compositionId();
    if (gesture.mode == NodeInteraction::Mode::Move) {
        std::map<document::NodeId, document::Vec2d> moved;
        for (const auto& [id, original] : gesture.positions)
            if (auto* card = findNodeItem(id); card && card->pos() != original)
                moved.emplace(id, document::Vec2d{card->pos().x(), card->pos().y()});
        // Where each dragged card landed relative to the frames that held still during the drag. A
        // frame drag carries every member at once and changes no membership, so it asks nothing.
        commands::NodeGroupMembershipDelta membership;
        if (!gesture.movedGroup) {
            const auto& groups = session_->composition()->nodeGroups();
            for (const auto& [id, original] : gesture.positions) {
                const auto* card = dynamic_cast<NodeItem*>(findNodeItem(id));
                if (card == nullptr)
                    continue;
                const auto landing =
                    groupForDrop(*this, card->mapToScene(card->cardRect().center()));
                const auto* current = document::findNodeGroupOf(groups, id);
                const auto currentId =
                    current == nullptr ? std::nullopt : std::optional(current->id);
                if (landing != currentId)
                    membership.emplace(id, landing);
            }
        }
        if (!moved.empty() || !membership.empty())
            transaction.emplace<commands::MoveNodes>(compositionId, moved, membership);
        if (gesture.insertEdge && !moved.empty()) {
            auto* card = dynamic_cast<NodeItem*>(findNodeItem(moved.begin()->first));
            const auto [input, output] = insertionSockets(*card, *session_->composition());
            if (input && output && input->input && output->output) {
                transaction.emplace<commands::ConnectPorts>(
                    compositionId, gesture.insertEdge->edge.source, *input->input);
                transaction.emplace<commands::ConnectPorts>(compositionId, *output->output,
                                                            gesture.insertEdge->edge.destination);
            }
        }
    } else if (gesture.mode == NodeInteraction::Mode::Resize) {
        if (const auto* card = dynamic_cast<NodeItem*>(findNodeItem(gesture.resized));
            card && card->cardWidth() != gesture.width)
            transaction.emplace<commands::SetNodeWidth>(compositionId, gesture.resized,
                                                        card->cardWidth());
    } else if (gesture.mode == NodeInteraction::Mode::Cut) {
        QPainterPathStroker stroke;
        stroke.setWidth(2);
        const auto cut = stroke.createStroke(gesture.line->path());
        for (auto* item : items())
            if (const auto* edge = dynamic_cast<NodeEdgeItem*>(item);
                edge && !edge->structural && cut.intersects(edge->shape()))
                transaction.emplace<commands::DisconnectInput>(compositionId,
                                                               edge->edge.destination);
    } else if (gesture.mode == NodeInteraction::Mode::Link) {
        const auto* target = socketAt(*this, event->scenePos());
        if (target && target->draggable() &&
            ((gesture.output && target->input) || (gesture.input && target->output))) {
            const auto input =
                gesture.input.value_or(target->input.value_or(document::NodeInputRef{}));
            const auto output =
                gesture.output.value_or(target->output.value_or(document::OutputPortRef{}));
            if (gesture.pickedInput && *gesture.pickedInput != input)
                transaction.emplace<commands::DisconnectInput>(compositionId, *gesture.pickedInput);
            transaction.emplace<commands::ConnectPorts>(compositionId, output, input);
        } else if (!target && !cardAt(*this, event->scenePos())) {
            if (gesture.pickedInput)
                transaction.emplace<commands::DisconnectInput>(compositionId, *gesture.pickedInput);
            else {
                const auto input = gesture.input;
                const auto output = gesture.output;
                cancelGesture();
                Q_EMIT addSearchRequested(event->scenePos(), event->screenPos(), input, output);
                return;
            }
        }
    }
    cancelGesture();
    if (!transaction.empty())
        (void)submit(std::move(transaction));
}
} // namespace bloom::ui
