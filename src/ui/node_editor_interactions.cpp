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
    for (auto* item : items())
        if (auto* card = dynamic_cast<NodeItem*>(item))
            card->setAuthoringEnabled(canSubmit());
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
        gesture.line = addPath({}, QPen(kit::color(socketColorToken(socket->kind)), 2));
        gesture.line->setZValue(10);
        gesture.line->setData(kNodeItemKindRole, QStringLiteral("link-preview"));
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
        if (fieldAt(*this, event->scenePos())) {
            QGraphicsScene::mousePressEvent(event);
            return;
        }
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
        break;
    case NodeInteraction::Mode::Resize:
        if (auto* card = dynamic_cast<NodeItem*>(findNodeItem(gesture.resized)))
            card->setPreviewWidth(std::max(128.0, gesture.width + delta.x()));
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
        gesture.line->setPen(
            QPen(kit::color(incompatible ? kit::Color::Error : kit::Color::DataImage), 2));
        gesture.line->setPath(gesture.output ? linkPath(gesture.origin, event->scenePos())
                                             : linkPath(event->scenePos(), gesture.origin));
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
        if (!moved.empty())
            transaction.emplace<commands::MoveNodes>(compositionId, moved);
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
