#include "node_editor_interactions.hpp"
#include <QCoreApplication>
#include <QGraphicsView>
#include <QPainterPathStroker>
#include <cmath>

namespace bloom::ui {
using namespace node_editor;
namespace {
bool fieldAt(QGraphicsScene& scene, QPointF point);
// Task NODES-1, deliverable 3: the grid-snap primitive the Move gesture's live preview and its
// final commit both round through. Alt-bypass and the "is snapping even on" check are the caller's
// job -- this is pure geometry.
QPointF snappedToGrid(const QPointF point, const qreal gridSize) {
    return {std::round(point.x() / gridSize) * gridSize,
            std::round(point.y() / gridSize) * gridSize};
}
// The pointer slop a socket gets, measured where the artist actually aims: on SCREEN. The socket's
// own hit shape is fixed in scene units (SocketItem::shape()), so at a zoomed-out canvas -- which
// is what Fit leaves the artist looking at -- kSocketHitSlop's 12 scene px shrink to three or four
// device px and a socket becomes something to aim at rather than something to grab. This converts
// the same slop back into scene units through the view's own scale, so the grab radius is constant
// in the artist's hand at every zoom.
[[nodiscard]] qreal sceneGrabRadius(const QGraphicsScene& scene) {
    qreal scale = 1.0;
    for (const auto* view : scene.views())
        if (view != nullptr && view->transform().m11() > 0.0)
            scale = view->transform().m11();
    // Never TIGHTER than the painted hit shape: zooming in does not make a socket harder to hit.
    // Pointer coordinates are integer viewport pixels. Include half a pixel so a point
    // exactly on the radius remains inside after viewport rounding at fractional zoom.
    return std::max(kSocketHitSlop, (kSocketHitSlop + kit::px(kit::Size::Hairline) / 2.0) / scale);
}
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
    if (closest != nullptr)
        return closest;
    // No socket's own shape held the point. Widen to the zoom-compensated radius, but only where a
    // hosted field is not already under the pointer -- a field keeps its own clicks, exactly as it
    // does when a socket shape does contain the point.
    if (fieldAt(scene, point)) {
        return nullptr;
    }
    const qreal radius = sceneGrabRadius(scene);
    const QRectF region(point.x() - radius, point.y() - radius, radius * 2.0, radius * 2.0);
    for (auto* item : scene.items(region, Qt::IntersectsItemBoundingRect)) {
        auto* socket = dynamic_cast<SocketItem*>(item);
        if (socket == nullptr || !socket->isVisible())
            continue;
        // Measured against the socket's own BODY, so a long multi-input pill is grabbable along its
        // whole length rather than only near its centre.
        const QPointF origin = socket->scenePos();
        const qreal half = socket->pillLength() / 2.0;
        const qreal dy = std::max(0.0, std::abs(point.y() - origin.y()) - half);
        const qreal reach = std::hypot(point.x() - origin.x(), dy);
        if (reach <= radius && reach < distance) {
            closest = socket;
            distance = reach;
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
            if (auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(parent)) {
                auto* root = proxy->widget();
                if (!root)
                    continue;
                auto* child = root->childAt(proxy->mapFromScene(point).toPoint());
                // Property labels and gutters belong to the card. Only a live control owns
                // the click, so its row proxy does not consume the socket's screen-space slop.
                for (auto* target = child ? child : root; target; target = target->parentWidget()) {
                    if (target->focusPolicy() != Qt::NoFocus ||
                        qobject_cast<QAbstractButton*>(target) ||
                        dynamic_cast<kit::KDiamond*>(target))
                        return true;
                    if (target == root)
                        break;
                }
            }
    return false;
}
const document::EdgeRecord* inputEdge(const document::Composition& composition,
                                      const document::InputPortRef& input) {
    const auto edges = composition.graph().edges();
    const auto found = std::ranges::find(edges, input, &document::EdgeRecord::destination);
    return found == edges.end() ? nullptr : &*found;
}
// Where an input's value comes from, whichever durable record carries it: an edge for image
// transport, the parameter's own driver binding for an operand (task FIX1, item A). The pick-up
// gesture asks this one question rather than only looking for an edge, which is why an operand's
// link can now be dragged off and dropped the way an image link always could.
std::optional<document::OutputPortRef> incomingSource(const document::Composition& composition,
                                                      const document::InputPortRef& input) {
    if (const auto* edge = inputEdge(composition, input))
        return edge->source;
    const auto* fixed = std::get_if<document::NodeInputRef>(&input);
    if (fixed == nullptr)
        return std::nullopt;
    const auto* node = composition.graph().findNode(fixed->nodeId);
    if (node == nullptr)
        return std::nullopt;
    const auto binding =
        std::ranges::find(node->parameters, fixed->port, &document::ParameterBinding::role);
    if (binding == node->parameters.end())
        return std::nullopt;
    const auto* parameter = composition.parameters().find(binding->parameterId);
    const auto* driver = parameter == nullptr
                             ? nullptr
                             : std::get_if<document::DriverBindingSource>(&parameter->source);
    return driver == nullptr
               ? std::nullopt
               : std::optional(document::OutputPortRef{driver->sourceNodeId, driver->outputPort});
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
        if (socket->kind != document::SocketValueKind::Image || socket->multiInput())
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
// The one line a dimmed socket carries while a drag it cannot take is in flight (task FIX1,
// item A.3): which way round the link would have to go, or which two kinds do not meet.
[[nodiscard]] QString refusalFor(const NodeInteraction& gesture, const SocketItem& socket,
                                 const bool opposite) {
    if (!socket.draggable())
        return QCoreApplication::translate(
            "node_editor", "Cannot be unlinked here; remove the layer to remove this connection");
    if (!opposite)
        return gesture.output.has_value()
                   ? QCoreApplication::translate("node_editor",
                                                 "A link from an output must land on an input")
                   : QCoreApplication::translate("node_editor",
                                                 "A link from an input must land on an output");
    const auto from = gesture.output.has_value() ? gesture.linkKind : socket.kind;
    const auto to = gesture.output.has_value() ? socket.kind : gesture.linkKind;
    return QCoreApplication::translate("node_editor", "%1 does not connect to %2")
        .arg(socketKindName(from), socketKindName(to));
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
        // The shared promotion whitelist, not kind equality: a socket an Integer output can widen
        // into is a socket the drag can land on, so the affinity an artist SEES is the rule
        // ConnectPorts will actually apply.
        const bool compatible =
            socket->draggable() && opposite &&
            (socket->acceptsAnyKind() ||
             (fromOutput ? document::isAcceptedSocketConnection(gesture.linkKind, socket->kind)
                         : document::isAcceptedSocketConnection(socket->kind, gesture.linkKind)));
        socket->setDragAffinity(compatible ? SocketItem::DragAffinity::Compatible
                                           : SocketItem::DragAffinity::Incompatible,
                                compatible ? QString{} : refusalFor(gesture, *socket, opposite));
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
    // Shift + right drag draws a stroke that puts a REROUTE on every link it crosses (task FIX1,
    // item I), the same shape of gesture as the Ctrl + right drag cut beside it and the same stroke
    // arithmetic on release -- one adds a bend where the stroke crossed, the other takes the wire
    // away.
    if (submit_ && event->button() == Qt::RightButton && event->modifiers() == Qt::ShiftModifier) {
        cancelGesture();
        gesture.mode = NodeInteraction::Mode::Reroute;
        gesture.origin = event->scenePos();
        gesture.revision = session_->snapshot().revision();
        gesture.line =
            addPath(QPainterPath(gesture.origin), QPen(kit::color(kit::Color::Accent), 2));
        gesture.line->setZValue(10);
        gesture.line->setData(kNodeItemKindRole, QStringLiteral("reroute-preview"));
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
        gesture.output = socket->output;
        // Merge's pill stands for every stack slot, so a press on it is a press on the slot under
        // the pointer (task FIX1, items B and C). With no slot there -- an empty stack, or the
        // pointer past the last one -- the gesture starts from the "new slot" sentinel the socket
        // carries, and a release on a Layer output creates the slot.
        std::optional<document::InputPortRef> pressedInput = socket->input;
        if (socket->multiInput()) {
            if (const auto index = socket->slotIndexAt(socket->mapFromScene(event->scenePos()));
                index.has_value() && *index < socket->orderedInputs().size())
                pressedInput = socket->orderedInputs()[*index];
        }
        gesture.input = pressedInput;
        if (pressedInput) {
            if (const auto existing = incomingSource(*session_->composition(), *pressedInput)) {
                auto* source = findOutput(*this, *existing);
                if (!source) {
                    cancelGesture();
                    return;
                }
                gesture.pickedInput = pressedInput;
                gesture.input.reset();
                gesture.output = *existing;
                gesture.origin = source->scenePos();
                // Addressed by DESTINATION, not by edge id: a driver link has no edge and therefore
                // no id, and one input has exactly one incoming link either way.
                for (auto* item : items())
                    if (auto* link = dynamic_cast<NodeEdgeItem*>(item);
                        link && link->edge.destination == *pressedInput)
                        link->hide();
            }
        }
        gesture.linkKind = socket->kind;
        gesture.line =
            addPath({}, QPen(kit::color(socketColorToken(gesture.linkKind)), kit::kNodeLinkWidth));
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
    if (event->button() == Qt::LeftButton && items(event->scenePos()).isEmpty()) {
        cancelGesture();
        Q_EMIT addSearchRequested(event->scenePos(), event->screenPos(), std::nullopt,
                                  std::nullopt);
        event->accept();
        return;
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
    case NodeInteraction::Mode::Move: {
        // Task NODES-1, deliverable 3: Alt bypasses snapping for exactly this move, without
        // touching the persisted snapEnabled_ setting itself.
        const bool snap = snapEnabled_ && !event->modifiers().testFlag(Qt::AltModifier);
        for (const auto& [id, position] : gesture.positions)
            if (auto* card = dynamic_cast<NodeItem*>(findNodeItem(id))) {
                card->setDragging(true);
                const QPointF target = position + delta;
                card->setPos(snap ? snappedToGrid(target, gridSize_) : target);
            }
        previewInsertion(*this, gesture, *session_->composition(), event->scenePos());
        updateGroupGeometry();
        break;
    }
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
    case NodeInteraction::Mode::Reroute:
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
        // Task S7: the preview's refusal is the KIND rule now, not "this socket is not Image".
        // Every socket is draggable, so what makes a landing site wrong is the same shared
        // isAcceptedSocketConnection() predicate ConnectPorts will apply on release -- which is
        // what keeps the red wire an honest preview of the refusal rather than a separate opinion.
        const bool orientationWrong = target != nullptr && ((gesture.output && !target->input) ||
                                                            (gesture.input && !target->output));
        const bool kindWrong =
            target != nullptr && !orientationWrong && !target->acceptsAnyKind() &&
            !(gesture.output
                  ? document::isAcceptedSocketConnection(gesture.linkKind, target->kind)
                  : document::isAcceptedSocketConnection(target->kind, gesture.linkKind));
        const bool incompatible =
            target != nullptr && (!target->draggable() || orientationWrong || kindWrong);
        gesture.line->setPen(
            QPen(kit::color(incompatible ? kit::Color::Error : socketColorToken(gesture.linkKind)),
                 kit::kNodeLinkWidth));
        gesture.line->setPath(gesture.output
                                  ? linkPath(gesture.origin, event->scenePos(), linkStyle_)
                                  : linkPath(event->scenePos(), gesture.origin, linkStyle_));
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
        // Task NODES-1, deliverable 3: snapped again here, explicitly, rather than trusting that
        // the last mouseMoveEvent already left every card on the lattice -- a release is its own
        // event, with its own modifiers, and is what MoveNodes actually reads. Skipped entirely for
        // a card no mouseMoveEvent ever touched (still exactly at `original`): a plain click is a
        // press and a release at the same point with no Move in between, and must stay a no-op --
        // snapping it here would silently teleport an untouched, off-lattice card onto the grid on
        // nothing more than a selection click.
        const bool snap = snapEnabled_ && !event->modifiers().testFlag(Qt::AltModifier);
        std::map<document::NodeId, document::Vec2d> moved;
        for (const auto& [id, original] : gesture.positions) {
            auto* card = findNodeItem(id);
            if (card == nullptr || card->pos() == original) {
                continue;
            }
            const QPointF finalPosition =
                snap ? snappedToGrid(card->pos(), gridSize_) : card->pos();
            if (finalPosition != card->pos()) {
                card->setPos(finalPosition);
            }
            if (finalPosition != original) {
                moved.emplace(id, document::Vec2d{finalPosition.x(), finalPosition.y()});
            }
        }
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
    } else if (gesture.mode == NodeInteraction::Mode::Reroute) {
        QPainterPathStroker stroke;
        stroke.setWidth(2);
        const auto crossing = stroke.createStroke(gesture.line->path());
        for (auto* item : items()) {
            const auto* edge = dynamic_cast<NodeEdgeItem*>(item);
            if (edge == nullptr || !crossing.intersects(edge->shape()))
                continue;
            // At the crossing itself, so the dot lands where the artist drew through the wire.
            const auto crossingPoint = edge->path().pointAtPercent(0.5);
            Q_EMIT rerouteRequested(edge->edge.destination, crossingPoint);
            break;
        }
        cancelGesture();
        return;
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
        bool bodyDrop = false;
        if (!target && gesture.output) {
            if (const auto* card = cardAt(*this, event->scenePos()))
                for (const auto* socket : card->sockets())
                    // Merge now carries two stack pills (content, audio): a body drop lands on
                    // whichever one the dragged output's kind actually feeds, not just the first
                    // multi-input pill the card happens to list.
                    if (socket->multiInput() &&
                        (socket->acceptsAnyKind() ||
                         document::isAcceptedSocketConnection(gesture.linkKind, socket->kind))) {
                        target = socket;
                        bodyDrop = true;
                        break;
                    }
        }
        // Task FOLLOW-1: release never used to re-check the kind rule against the exact pill it
        // landed on, so dropping an Audio output on the content pill (or an Image output on the
        // audio pill) committed a ConnectPorts transaction that graph.addEdge could only reject
        // with a generic message. Refuse it HERE, before a doomed transaction is even built.
        const bool kindWrong =
            target != nullptr && target->multiInput() && !target->acceptsAnyKind() &&
            !(gesture.output
                  ? document::isAcceptedSocketConnection(gesture.linkKind, target->kind)
                  : document::isAcceptedSocketConnection(target->kind, gesture.linkKind));
        if (kindWrong) {
            Q_EMIT session_->commandRejected(
                target->kind == document::SocketValueKind::Audio
                    ? QCoreApplication::translate("node_editor",
                                                  "An Image output cannot feed the audio stack")
                    : QCoreApplication::translate("node_editor",
                                                  "An Audio output cannot feed the image stack"));
        } else if (target && target->draggable() &&
                   ((gesture.output && target->input) || (gesture.input && target->output))) {
            const auto input =
                gesture.input.value_or(target->input.value_or(document::NodeInputRef{}));
            const auto output =
                gesture.output.value_or(target->output.value_or(document::OutputPortRef{}));
            // Where in the stack order a drop on Merge's pill lands. Read off the same caret the
            // artist watched during the drag, so the order they saw is the order written.
            std::optional<document::LayerSlotId> insertBefore;
            if (gesture.output && target->multiInput() && !bodyDrop)
                insertBefore = target->slotInsertionAt(target->mapFromScene(event->scenePos()));
            const auto* picked =
                gesture.pickedInput
                    ? std::get_if<document::LayerStackInputRef>(&*gesture.pickedInput)
                    : nullptr;
            const auto* mergeInput = std::get_if<document::LayerStackInputRef>(&input);
            if (picked && mergeInput && picked->stackNodeId == mergeInput->stackNodeId &&
                target->multiInput()) {
                const auto entries =
                    session_->composition()->graph().merge(picked->stackNodeId)->entries();
                std::size_t index = entries.size();
                if (insertBefore)
                    index = static_cast<std::size_t>(std::distance(
                        entries.begin(), std::ranges::find(entries, *insertBefore,
                                                           &document::LayerStackEntry::slotId)));
                const auto old = static_cast<std::size_t>(std::distance(
                    entries.begin(), std::ranges::find(entries, picked->slotId,
                                                       &document::LayerStackEntry::slotId)));
                if (index > old)
                    --index;
                transaction.emplace<commands::ReorderMergeInput>(compositionId, picked->stackNodeId,
                                                                 picked->slotId, index);
            } else {
                if (gesture.pickedInput && *gesture.pickedInput != input)
                    transaction.emplace<commands::DisconnectInput>(compositionId,
                                                                   *gesture.pickedInput);
                transaction.emplace<commands::ConnectPorts>(
                    compositionId, output, input, document::builtInNodeDefinitions(), insertBefore);
            }
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
