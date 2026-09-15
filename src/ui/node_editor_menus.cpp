#include "node_editor_add.hpp"
#include "node_editor_items.hpp"
#include <QActionGroup>
#include <QCursor>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QPalette>
#include <QResizeEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QToolButton>
#include <algorithm>
#include <array>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/search_popup.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <iterator>
#include <tuple>
#include <vector>

namespace bloom::ui {
using namespace node_editor;
namespace {
document::NodeLayoutRecord layoutFor(const document::Composition& composition,
                                     const document::NodeId id) {
    const auto found = composition.nodeLayout().find(id);
    return found == composition.nodeLayout().end()
               ? document::defaultNodeLayout(composition.graph().nodes()).at(id)
               : found->second;
}
QString addActionName(const std::string_view type) {
    if (type == document::kSolidSourceNodeType)
        return QStringLiteral("nodeAddSolidLayerAction");
    if (type == document::kTextSourceNodeType)
        return QStringLiteral("nodeAddTextLayerAction");
    if (type == document::kLayerOutputNodeType)
        return QStringLiteral("nodeAddLayerOutputAction");
    if (type == document::kLayerStackNodeType)
        return QStringLiteral("nodeAddLayerStackAction");
    if (type == document::kCompositionOutputNodeType)
        return QStringLiteral("nodeAddCompositionOutputAction");
    // Every other registered type gets its own name derived from its type id. Before task FIX1 this
    // function fell through to the composition-output name, so all forty value nodes shared one
    // object name -- which made the menu unaddressable from a test and said nothing about which row
    // was which.
    return QStringLiteral("nodeAddAction.") +
           QString::fromUtf8(type.data(), static_cast<qsizetype>(type.size()));
}

// Task NODES-1, deliverable 1: the header-hosted menu strip NodeGraphEditor hands EditorArea

// too narrow to hold all of them side by side, at which point every one of them folds into a
// single "..." overflow button holding the same menus as submenus -- "never wrap": the header
// row's height is fixed (Size::EditorHeader), so there is nowhere for a second row to go.

} // namespace

void NodeGraphEditor::showStatus(const QString& message) {
    Q_EMIT session_.commandRejected(message);
}

// The canvas's key bindings (task S1, item 8). Every node command below is a named method rather
// than a key this function re-dispatches to itself, because the context menu invokes the same
// commands and a menu item that worked by synthesizing a key press could only offer what the
// keyboard happened to bind -- which is exactly what mute, collapse and dissolve no longer are.
void NodeGraphEditor::handleCanvasKey(const int key, const Qt::KeyboardModifiers modifiers) {
    if (key == Qt::Key_Escape) {
        scene_->cancelGesture();
        return;
    }
    if (modifiers == (Qt::ControlModifier | Qt::ShiftModifier)) {
        if (key == Qt::Key_G)
            ungroupSelection();
        return;
    }
    if (modifiers == Qt::ControlModifier) {
        if (key == Qt::Key_A)
            scene_->selectAllNodes();
        else if (key == Qt::Key_D)
            duplicateSelectedNodes();
        else if (key == Qt::Key_G)
            groupSelectedNodes();
        return;
    }
    if (modifiers != Qt::NoModifier)
        return;
    if (key == Qt::Key_Tab) {
        const QPoint global = QCursor::pos();
        openAddSearch(view_->sceneFromViewport(view_->viewport()->mapFromGlobal(global)), global);
        return;
    }
    if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
        removeSelectedNodes();
        return;
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter)
        renameSelectedLayer();
}

// The guard every node command shares: an available submission path, a live composition, a
// non-empty selection, and no transient gesture left in flight. An empty result means nothing will
// happen and the reason has already been reported.
std::set<document::NodeId> NodeGraphEditor::commandTargets() {
    if (!scene_->canSubmit()) {
        showStatus(tr("Node command submission is unavailable"));
        return {};
    }
    auto nodes = session_.selectedNodes();
    if (nodes.empty() || !session_.composition()) {
        showStatus(tr("Select a node first"));
        return {};
    }
    if (scene_->gestureActive())
        scene_->cancelGesture();
    return nodes;
}

void NodeGraphEditor::removeSelectedNodes() {
    const auto nodes = commandTargets();
    if (nodes.empty())
        return;
    commands::Transaction transaction("Edit Nodes", session_.snapshot().revision());
    transaction.emplace<commands::RemoveNodes>(session_.compositionId(), nodes);
    (void)scene_->submit(std::move(transaction));
}

void NodeGraphEditor::duplicateSelectedNodes() {
    const auto nodes = commandTargets();
    if (nodes.empty())
        return;
    commands::Transaction transaction("Edit Nodes", session_.snapshot().revision());
    transaction.emplace<commands::DuplicateNodes>(session_.compositionId(), nodes,
                                                  document::Vec2d{24, 24});
    const auto result = scene_->submit(std::move(transaction));
    if (!result.succeeded())
        return;
    std::set<document::NodeId> copies;
    for (const auto id : nodes) {
        const auto copy = result.outputId<document::NodeId>("node." + std::to_string(id.value()));
        if (copy)
            copies.insert(*copy);
    }
    if (copies.empty())
        return;
    session_.selectNodes(copies, *copies.begin());
    scene_->startDuplicateMove(
        view_->sceneFromViewport(view_->viewport()->mapFromGlobal(QCursor::pos())));
}

void NodeGraphEditor::dissolveSelectedNode() {
    const auto nodes = commandTargets();
    if (nodes.empty())
        return;
    if (nodes.size() != 1) {
        showStatus(tr("Dissolve requires exactly one selected node"));
        return;
    }
    commands::Transaction transaction("Edit Nodes", session_.snapshot().revision());
    transaction.emplace<commands::DissolveNode>(session_.compositionId(), *nodes.begin());
    (void)scene_->submit(std::move(transaction));
}

void NodeGraphEditor::toggleSelectedMuted(const bool muted) {
    const auto nodes = commandTargets();
    if (nodes.empty())
        return;
    // A mixed selection becomes uniformly enabled for the state, not individually flipped.
    const bool target = std::ranges::any_of(nodes, [&](const auto id) {
        const auto layout = layoutFor(*session_.composition(), id);
        return !(muted ? layout.muted : layout.collapsed);
    });
    commands::Transaction transaction("Edit Nodes", session_.snapshot().revision());
    for (const auto id : nodes) {
        if (muted)
            transaction.emplace<commands::SetNodeMuted>(session_.compositionId(), id, target);
        else
            transaction.emplace<commands::SetNodeCollapsed>(session_.compositionId(), id, target);
    }
    (void)scene_->submit(std::move(transaction));
}

void NodeGraphEditor::renameSelectedLayer() {
    const auto nodes = commandTargets();
    if (nodes.empty())
        return;
    if (nodes.size() != 1) {
        showStatus(tr("Rename requires exactly one selected layer"));
        return;
    }
    const auto id = *nodes.begin();
    for (const auto& boundary : session_.composition()->graph().layerOutputs()) {
        if (boundary.nodeId != id)
            continue;
        if (auto* card = dynamic_cast<NodeItem*>(scene_->findNodeItem(id)))
            card->startRename();
        return;
    }
    showStatus(tr("Only a layer node can be renamed"));
}

void NodeGraphEditor::groupSelectedNodes() {
    const auto nodes = commandTargets();
    if (nodes.empty())
        return;
    commands::Transaction transaction("Group Nodes", session_.snapshot().revision());
    transaction.emplace<commands::GroupNodes>(session_.compositionId(), nodes,
                                              std::string(commands::kDefaultNodeGroupName));
    (void)scene_->submit(std::move(transaction));
}

void NodeGraphEditor::ungroupSelection(const std::optional<document::NodeGroupId> group) {
    if (!scene_->canSubmit()) {
        showStatus(tr("Node command submission is unavailable"));
        return;
    }
    if (!session_.composition())
        return;
    if (scene_->gestureActive())
        scene_->cancelGesture();
    std::set<document::NodeGroupId> targets;
    if (group) {
        targets.insert(*group);
    } else {
        // No frame was named, so the selection says which frames to take apart -- the frames its
        // own nodes are sitting in.
        for (const auto id : session_.selectedNodes())
            if (const auto* owner =
                    document::findNodeGroupOf(session_.composition()->nodeGroups(), id))
                targets.insert(owner->id);
    }
    if (targets.empty()) {
        showStatus(tr("Select a grouped node first"));
        return;
    }
    commands::Transaction transaction("Ungroup Nodes", session_.snapshot().revision());
    for (const auto id : targets)
        transaction.emplace<commands::UngroupNodes>(session_.compositionId(), id);
    (void)scene_->submit(std::move(transaction));
}

// --- Task NODES-1: View and Select commands the header menus reach ------------------------------

void NodeGraphEditor::frameSelectedNodes() {
    QRectF bounds;
    for (auto* item : scene_->selectedItems())
        if (auto* card = dynamic_cast<NodeItem*>(item)) {
            const auto rect = card->mapRectToScene(card->cardRect());
            bounds = bounds.isNull() ? rect : bounds.united(rect);
        }
    if (bounds.isEmpty()) {
        // Nothing selected: framing nothing would silently do nothing an artist asked for, so this
        // falls back to framing the whole graph instead of being a no-op.
        view_->frameGraph();
        return;
    }
    view_->frameRect(
        bounds.adjusted(-kNodeSceneMargin, -kNodeSceneMargin, kNodeSceneMargin, kNodeSceneMargin));
}

void NodeGraphEditor::selectNoneNodes() { session_.clearSelection(); }

void NodeGraphEditor::selectInvertNodes() {
    const auto* composition = session_.composition();
    if (composition == nullptr)
        return;
    std::set<document::NodeId> all;
    for (const auto& node : composition->graph().nodes())
        all.insert(node.id);
    const auto current = session_.selectedNodes();
    std::set<document::NodeId> inverted;
    std::ranges::set_difference(all, current, std::inserter(inverted, inverted.end()));
    if (inverted.empty())
        session_.clearSelection();
    else
        session_.selectNodes(inverted, *inverted.begin());
}

std::set<document::NodeId> NodeGraphEditor::linkedNodes(const std::set<document::NodeId>& seeds,
                                                        const bool upstream) const {
    const auto* composition = session_.composition();
    if (composition == nullptr)
        return seeds;
    // The SAME link set rebuildEdges() draws: graph edges followed by every driver binding, read as
    // (source, destination) pairs, so "linked" here can never disagree with what the canvas
    // actually shows as a wire.
    std::vector<std::pair<document::NodeId, document::NodeId>> links;
    for (const auto& edge : composition->graph().edges())
        links.emplace_back(edge.source.nodeId, destinationNodeId(edge.destination));
    for (const auto& node : composition->graph().nodes())
        for (const auto& binding : node.parameters) {
            const auto* parameter = composition->parameters().find(binding.parameterId);
            const auto* driver =
                parameter == nullptr
                    ? nullptr
                    : std::get_if<document::DriverBindingSource>(&parameter->source);
            if (driver != nullptr)
                links.emplace_back(driver->sourceNodeId, node.id);
        }
    std::set<document::NodeId> visited = seeds;
    std::vector<document::NodeId> frontier(seeds.begin(), seeds.end());
    while (!frontier.empty()) {
        std::vector<document::NodeId> next;
        for (const auto id : frontier)
            for (const auto& [from, to] : links) {
                const auto neighbor = upstream ? (to == id ? std::optional(from) : std::nullopt)
                                               : (from == id ? std::optional(to) : std::nullopt);
                if (neighbor && visited.insert(*neighbor).second)
                    next.push_back(*neighbor);
            }
        frontier = std::move(next);
    }
    return visited;
}

void NodeGraphEditor::selectLinkedUpstream() {
    const auto seeds = session_.selectedNodes();
    if (seeds.empty()) {
        showStatus(tr("Select a node first"));
        return;
    }
    const auto extended = linkedNodes(seeds, true);
    session_.selectNodes(extended, *seeds.begin());
}

void NodeGraphEditor::selectLinkedDownstream() {
    const auto seeds = session_.selectedNodes();
    if (seeds.empty()) {
        showStatus(tr("Select a node first"));
        return;
    }
    const auto extended = linkedNodes(seeds, false);
    session_.selectNodes(extended, *seeds.begin());
}

// Task NODES-1: the categorized Add submenu's actual contents, factored out of buildContextMenu()
// so the header's own persistent Add menu (buildHeaderMenus()) builds the SAME structure -- same
// category order, same sort, same objectNames -- rather than a second opinion about what is
// addable. `recordInto`, given only by the header's call, collects each leaf action with its type
// id for refreshAddMenuState() to re-read afterward.
QString NodeGraphEditor::addNodeRefusal(const std::string& typeId) const {
    if (session_.composition() == nullptr)
        return tr("No active composition");
    document::Document isolated(session_.snapshot().project(),
                                session_.snapshot().ids().highWater());
    auto draft = isolated.draft(isolated.snapshot());
    const auto result =
        AddEditorNode(session_.compositionId(), typeId, {addPosition_.x(), addPosition_.y()})
            .apply(draft);
    return result.issues.empty() ? QString{}
                                 : QString::fromStdString(result.issues.front().message);
}

void NodeGraphEditor::populateAddMenu(QMenu* menu,
                                      std::vector<std::pair<QAction*, std::string>>* recordInto) {
    for (const auto& category : nodeCategoryOrder()) {
        std::vector<const document::NodeDefinition*> section;
        for (const auto& definition : document::builtInNodeDefinitions().definitions())
            // Task FIX1, item I: a reroute is a point on a LINK, made by right-clicking the link or
            // dragging across it. It is not something to pick out of a menu and then find a use
            // for, so it is listed in neither Add surface.
            if (nodeCategoryName(definition) == category &&
                !document::isRerouteNodeType(definition.key.typeId))
                section.push_back(&definition);
        if (section.empty())
            continue;
        std::ranges::sort(section, [](const auto* left, const auto* right) {
            return nodeTypeDisplayName(left->key.typeId) < nodeTypeDisplayName(right->key.typeId);
        });
        auto* sectionMenu = kit::makeMenu(nodeCategoryName(category), menu);
        sectionMenu->setProperty("columnFlow", true);
        menu->addMenu(sectionMenu);
        sectionMenu->setObjectName(QStringLiteral("nodeAddCategoryMenu.") +
                                   nodeCategoryName(category));
        for (const auto* candidate : section) {
            auto* item = sectionMenu->addAction(nodeTypeDisplayName(candidate->key.typeId));
            item->setObjectName(addActionName(candidate->key.typeId));
            if (scene_->canSubmit()) {
                // Cardinality and every other refusal, read back from the command itself rather
                // than restated here: a singleton already in the composition is listed and
                // disabled, with the command's own words in its tooltip.
                const auto refusal = addNodeRefusal(candidate->key.typeId);
                item->setToolTip(refusal);
                item->setEnabled(refusal.isEmpty());
            } else {
                // Without a submission adapter only the two session-level Add paths exist.
                const bool solid = candidate->key.typeId == document::kSolidSourceNodeType;
                const bool text = candidate->key.typeId == document::kTextSourceNodeType;
                item->setVisible(solid || text);
                item->setEnabled((solid || text) && session_.composition() != nullptr);
            }
            connect(
                item, &QAction::triggered, this,
                [this, type = QString::fromStdString(candidate->key.typeId)] { addNode(type); });
            if (recordInto != nullptr)
                recordInto->emplace_back(item, candidate->key.typeId);
        }
    }
}

// Refreshes every action populateAddMenu() recorded into addMenuItems_ in place -- no rebuild, so
// the persistent header Add menu never leaks a submenu (see populateAddMenu()'s own header
// comment). Connected to headerAddMenu_'s aboutToShow.
void NodeGraphEditor::refreshAddMenuState() {
    addRevision_ = session_.snapshot().revision();
    for (const auto& [item, typeId] : addMenuItems_) {
        if (scene_->canSubmit()) {
            const auto refusal = addNodeRefusal(typeId);
            item->setToolTip(refusal);
            item->setEnabled(refusal.isEmpty());
        } else {
            const bool solid = typeId == document::kSolidSourceNodeType;
            const bool text = typeId == document::kTextSourceNodeType;
            item->setVisible(solid || text);
            item->setEnabled((solid || text) && session_.composition() != nullptr);
        }
    }
}

QMenu* NodeGraphEditor::buildContextMenu(QWidget* parent, const bool nodeMenu,
                                         const std::optional<document::NodeGroupId> group) {
    addRevision_ = session_.snapshot().revision();
    auto* menu = kit::makeMenu(parent);
    menu->setObjectName(nodeMenu ? QStringLiteral("nodeContextMenu")
                                 : QStringLiteral("nodeCanvasMenu"));
    menu->setAccessibleName(tr("Node graph menu"));
    const auto action = [&](const QString& label, const QString& name, auto callback) {
        auto* item = menu->addAction(label);
        item->setObjectName(name);
        connect(item, &QAction::triggered, this, callback);
        return item;
    };
    if (group) {
        if (!scene_->canSubmit() || !session_.composition() ||
            !session_.composition()->nodeGroups().contains(*group))
            return menu;
        action(tr("Ungroup"), QStringLiteral("nodeUngroupAction"),
               [this, id = *group] { ungroupSelection(id); });
        action(tr("Rename"), QStringLiteral("nodeGroupRenameAction"), [this, id = *group] {
            if (auto* frame = dynamic_cast<NodeGroupItem*>(scene_->findNodeGroupItem(id)))
                frame->startRename();
        });
        return menu;
    }
    if (nodeMenu) {
        const auto nodes = session_.selectedNodes();
        if (nodes.empty() || !scene_->canSubmit() || !session_.composition())
            return menu;
        const auto composition = session_.compositionId();
        const auto accepts = [&](const commands::Operation& operation) {
            return commands::canApplyNodeOperation(session_.snapshot(), operation);
        };
        if (accepts(commands::DuplicateNodes(composition, nodes, {24, 24})))
            action(tr("Duplicate"), QStringLiteral("nodeDuplicateAction"),
                   [this] { duplicateSelectedNodes(); });
        if (nodes.size() == 1 && accepts(commands::DissolveNode(composition, *nodes.begin())))
            action(tr("Dissolve"), QStringLiteral("nodeDissolveAction"),
                   [this] { dissolveSelectedNode(); });
        const bool allMuted = std::ranges::all_of(
            nodes, [&](const auto id) { return layoutFor(*session_.composition(), id).muted; });
        const bool allCollapsed = std::ranges::all_of(
            nodes, [&](const auto id) { return layoutFor(*session_.composition(), id).collapsed; });
        if (std::ranges::all_of(nodes, [&](const auto id) {
                return accepts(commands::SetNodeMuted(composition, id, !allMuted));
            }))
            action(allMuted ? tr("Unmute") : tr("Mute"), QStringLiteral("nodeMuteAction"),
                   [this] { toggleSelectedMuted(true); });
        if (std::ranges::all_of(nodes, [&](const auto id) {
                return accepts(commands::SetNodeCollapsed(composition, id, !allCollapsed));
            }))
            action(allCollapsed ? tr("Expand") : tr("Collapse"),
                   QStringLiteral("nodeCollapseAction"), [this] { toggleSelectedMuted(false); });
        if (nodes.size() == 1) {
            for (const auto& boundary : session_.composition()->graph().layerOutputs()) {
                if (boundary.nodeId == *nodes.begin() &&
                    accepts(commands::RenameLayer(composition, boundary.layerId, boundary.name)))
                    action(tr("Rename"), QStringLiteral("nodeRenameAction"),
                           [this, id = boundary.nodeId] {
                               if (auto* card = dynamic_cast<NodeItem*>(scene_->findNodeItem(id)))
                                   card->startRename();
                           });
            }
        }
        if (accepts(commands::GroupNodes(composition, nodes,
                                         std::string(commands::kDefaultNodeGroupName))))
            action(tr("Group"), QStringLiteral("nodeGroupAction"),
                   [this] { groupSelectedNodes(); });
        if (std::ranges::any_of(nodes, [&](const auto id) {
                return document::findNodeGroupOf(session_.composition()->nodeGroups(), id) !=
                       nullptr;
            }))
            action(tr("Ungroup"), QStringLiteral("nodeUngroupAction"),
                   [this] { ungroupSelection(); });
        if (accepts(commands::RemoveNodes(composition, nodes)))
            action(tr("Delete"), QStringLiteral("nodeDeleteAction"),
                   [this] { removeSelectedNodes(); });
        return menu;
    }
    // Task FIX1, item D: "Add Node" is a CASCADING SUBMENU grouped by the registry's own
    // categories, in the same order the search popup's sections are read in. It is not the search
    // popup: right- clicking to add a known node should not make the artist type its name, and Tab
    // is where the search lives. Both surfaces call addNode() with the same click position and read
    // their refusals from the same dry run of AddEditorNode, so neither can disagree with the other
    // about what is addable. Kit menu styling is the application-wide proxy style
    // (kit/mnemonic_style.hpp)
    // -- rows, the submenu caret and Size::MenuMinWidth come from it, so an ordinary QMenu is
    // already a Kinetik menu and a second opinion here would be the drift that file exists to
    // prevent.
    auto* addMenu = menu->addMenu(tr("Add Node"));
    addMenu->setObjectName(QStringLiteral("nodeAddMenu"));
    addMenu->setEnabled(session_.composition() != nullptr);
    populateAddMenu(addMenu);
    menu->addSeparator();
    action(tr("Fit"), QStringLiteral("nodeFitAction"), [this] { view_->frameGraph(); });
    action(tr("100%"), QStringLiteral("nodeActualSizeAction"),
           [this] { view_->zoomToActualSize(); });
    menu->addSeparator();
    action(tr("Zoom In"), QStringLiteral("nodeZoomInAction"), [this] { view_->zoomStep(1); });
    action(tr("Zoom Out"), QStringLiteral("nodeZoomOutAction"), [this] { view_->zoomStep(-1); });
    action(tr("Select All"), QStringLiteral("nodeSelectAllAction"),
           [this] { scene_->selectAllNodes(); });
    return menu;
}

QMenu* NodeGraphEditor::contextMenuForTest(const bool nodeMenu,
                                           const std::optional<document::NodeGroupId> group) {
    return buildContextMenu(this, nodeMenu, group);
}

QMenu* NodeGraphEditor::linkContextMenuForTest(const QPoint viewportPosition) {
    return buildLinkContextMenu(this, viewportPosition);
}

// Task FIX1, item C: a link is a thing an artist can act on, not only a thing to look at. One
// command, offered under the two names an artist might look for it by -- "Disconnect" says what
// happens to the connection, "Delete Link" says what happens to the wire, and they are the same
// DisconnectInput on the same destination. Which durable record that destination is addressed
// through -- an edge, a driver binding, or a stack slot -- is DisconnectInput's business, not this
// menu's, which is why every link kind works here.
QMenu* NodeGraphEditor::buildLinkContextMenu(QWidget* parent, const QPoint viewportPosition) {
    if (!scene_->canSubmit() || session_.composition() == nullptr)
        return nullptr;
    const node_editor::NodeEdgeItem* link = nullptr;
    for (auto* item : view_->items(viewportPosition))
        if (const auto* candidate = dynamic_cast<node_editor::NodeEdgeItem*>(item);
            candidate != nullptr) {
            link = candidate;
            break;
        }
    if (link == nullptr)
        return nullptr;
    auto* menu = kit::makeMenu(parent);
    menu->setObjectName(QStringLiteral("nodeLinkMenu"));
    menu->setAccessibleName(tr("Link menu"));
    const auto input = link->edge.destination;
    auto* disconnect = menu->addAction(tr("Disconnect"));
    disconnect->setObjectName(QStringLiteral("nodeLinkDisconnectAction"));
    connect(disconnect, &QAction::triggered, this, [this, input] { disconnectLink(input); });
    auto* remove = menu->addAction(tr("Delete Link"));
    remove->setObjectName(QStringLiteral("nodeLinkDeleteAction"));
    connect(remove, &QAction::triggered, this, [this, input] { disconnectLink(input); });
    menu->addSeparator();
    // Task FIX1, item I: a reroute is made ON a link, at the point the artist clicked, because that
    // is the only place a bend in a wire means anything.
    auto* reroute = menu->addAction(tr("Add Reroute"));
    reroute->setObjectName(QStringLiteral("nodeLinkAddRerouteAction"));
    const auto scenePosition = view_->sceneFromViewport(viewportPosition);
    connect(reroute, &QAction::triggered, this,
            [this, input, scenePosition] { insertReroute(input, scenePosition); });
    return menu;
}

void NodeGraphEditor::insertReroute(document::InputPortRef input, const QPointF scenePosition) {
    if (!scene_->canSubmit()) {
        showStatus(tr("Node command submission is unavailable"));
        return;
    }
    if (scene_->gestureActive())
        scene_->cancelGesture();
    commands::Transaction insert("Add Reroute", session_.snapshot().revision());
    insert.emplace<InsertReroute>(session_.compositionId(), std::move(input),
                                  document::Vec2d{scenePosition.x(), scenePosition.y()});
    const auto result = scene_->submit(std::move(insert));
    if (const auto id = result.outputId<document::NodeId>("editorNode"); result.succeeded() && id)
        session_.selectNode(*id);
}

void NodeGraphEditor::disconnectLink(document::InputPortRef input) {
    if (!scene_->canSubmit()) {
        showStatus(tr("Node command submission is unavailable"));
        return;
    }
    if (scene_->gestureActive())
        scene_->cancelGesture();
    commands::Transaction transaction("Disconnect Link", session_.snapshot().revision());
    transaction.emplace<commands::DisconnectInput>(session_.compositionId(), std::move(input));
    (void)scene_->submit(std::move(transaction));
}
void NodeGraphEditor::showContextMenu(const QPoint& viewportPosition) {
    auto* card = nodeItemAncestor(view_->itemAt(viewportPosition));
    // A link under the pointer owns the click, and only where there is no card there: a wire
    // passing behind a card is the card's business.
    if (card == nullptr) {
        if (const QPointer<QMenu> linkMenu = buildLinkContextMenu(view_, viewportPosition)) {
            linkMenu->setAttribute(Qt::WA_DeleteOnClose);
            linkMenu->popup(view_->viewport()->mapToGlobal(viewportPosition));
            return;
        }
    }
    if (card && !session_.selectedNodes().contains(card->id()))
        session_.selectNode(card->id());
    addPosition_ = view_->sceneFromViewport(viewportPosition);
    addInput_.reset();
    addOutput_.reset();
    addRevision_ = session_.snapshot().revision();
    // A right-click that is not on a card but is inside a frame is about that frame.
    std::optional<document::NodeGroupId> group;
    if (card == nullptr)
        for (auto* item : view_->items(viewportPosition))
            if (auto* frame = groupItemAncestor(item); frame != nullptr && frame->isVisible()) {
                group = frame->id();
                break;
            }
    const QPointer<QMenu> menu =
        buildContextMenu(view_, card != nullptr && scene_->canSubmit(), group);
    // Nonblocking popup: selecting Duplicate must let the canvas receive the following move.
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(view_->viewport()->mapToGlobal(viewportPosition));
}

void NodeGraphEditor::openAddSearch(const QPointF scenePosition, const QPoint screenPosition,
                                    std::optional<document::InputPortRef> input,
                                    std::optional<document::OutputPortRef> output) {
    if (!session_.composition())
        return;
    if (!scene_->canSubmit()) {
        showStatus(
            tr("Node command submission is unavailable; Add > Solid can still create a layer"));
        return;
    }
    addPosition_ = scenePosition;
    addInput_ = std::move(input);
    addOutput_ = std::move(output);
    addRevision_ = session_.snapshot().revision();
    if (!search_) {
        search_ = new kit::KSearchPopup(this);
        connect(search_, &kit::KSearchPopup::entryChosen, this, &NodeGraphEditor::addNode);
    }
    // Listed in the category order the sections are read in, and alphabetically inside each one.
    // The registry's own order is by type id, which is neither.
    std::vector<const document::NodeDefinition*> ordered;
    for (const auto& category : nodeCategoryOrder()) {
        std::vector<const document::NodeDefinition*> section;
        for (const auto& definition : document::builtInNodeDefinitions().definitions())
            // The reroute is hidden here for the same reason it is hidden from the Add submenu.
            if (nodeCategoryName(definition) == category &&
                !document::isRerouteNodeType(definition.key.typeId))
                section.push_back(&definition);
        std::ranges::sort(section, [](const auto* left, const auto* right) {
            return nodeTypeDisplayName(left->key.typeId) < nodeTypeDisplayName(right->key.typeId);
        });
        ordered.insert(ordered.end(), section.begin(), section.end());
    }

    std::vector<kit::SearchEntry> entries;
    for (const auto* candidate : ordered) {
        const auto& definition = *candidate;
        QString keywords = QString::fromStdString(definition.key.typeId);
        for (const auto& port : definition.inputs)
            keywords += ' ' + socketKindName(port.valueKind);
        for (const auto& port : definition.outputs)
            keywords += ' ' + socketKindName(port.valueKind);
        QString refusal;
        // Read the actual refusal, including text's portable-font message, from a private draft.
        document::Document isolated(session_.snapshot().project(),
                                    session_.snapshot().ids().highWater());
        auto draft = isolated.draft(isolated.snapshot());
        const auto result =
            AddEditorNode(session_.compositionId(), definition.key.typeId,
                          {scenePosition.x(), scenePosition.y()}, addInput_, addOutput_)
                .apply(draft);
        if (!result.issues.empty())
            refusal = QString::fromStdString(result.issues.front().message);
        else if (!scene_->canSubmit() && definition.key.typeId != document::kSolidSourceNodeType &&
                 definition.key.typeId != document::kTextSourceNodeType)
            refusal = tr("Node command submission is unavailable");
        entries.push_back({QString::fromStdString(definition.key.typeId),
                           nodeTypeDisplayName(definition.key.typeId), keywords, refusal,
                           nodeCategoryName(definition)});
    }
    search_->setEntries(std::move(entries));
    search_->openAt(screenPosition);
}

void NodeGraphEditor::addNode(const QString& type) {
    if (!scene_->canSubmit()) {
        if (type.toStdString() == document::kSolidSourceNodeType)
            (void)addDefaultSolidLayer(session_);
        else if (type.toStdString() == document::kTextSourceNodeType)
            (void)addDefaultTextLayer(session_);
        return;
    }
    // One label, because there is one thing the canvas's Add does now: it adds the node that was
    // asked for (task FIX1, item B). "Add Solid Layer" would have been a promise about structure
    // the artist makes themselves.
    commands::Transaction transaction("Add Node", addRevision_);
    transaction.emplace<AddEditorNode>(session_.compositionId(), type.toStdString(),
                                       document::Vec2d{addPosition_.x(), addPosition_.y()},
                                       addInput_, addOutput_);
    const auto result = scene_->submit(std::move(transaction));
    if (const auto id = result.outputId<document::NodeId>("editorNode"); result.succeeded() && id)
        session_.selectNode(*id);
    view_->setFocus(Qt::PopupFocusReason);
}

// --- Task NODES-1, deliverable 1: the persistent header menus -----------------------------------

LinkStyle NodeGraphEditor::linkStyleFromSettingsValue(const QString& value) noexcept {
    if (value == QStringLiteral("straight"))
        return LinkStyle::Straight;
    if (value == QStringLiteral("angled"))
        return LinkStyle::Angled;
    return LinkStyle::Spline;
}

QString NodeGraphEditor::linkStyleSettingsValue(const LinkStyle style) noexcept {
    switch (style) {
    case LinkStyle::Straight:
        return QStringLiteral("straight");
    case LinkStyle::Angled:
        return QStringLiteral("angled");
    case LinkStyle::Spline:
        break;
    }
    return QStringLiteral("spline");
}

void NodeGraphEditor::buildHeaderMenus() {
    const auto action = [this](QMenu* menu, const QString& label, const QString& name,
                               auto callback) {
        auto* item = menu->addAction(label);
        item->setObjectName(name);
        connect(item, &QAction::triggered, this, callback);
        return item;
    };
    // A shortcut set here is DISPLAY ONLY (WidgetShortcut context, scoped to the action's own
    // menu): it shows the same accelerator docs/ux/interaction-model.md already documents for the
    // canvas, without registering a second, window-wide live shortcut that could fire while some
    // other panel has focus -- NodeGraphicsView already owns the live key for every one of these
    // through ShortcutOverride (docs/ux/interaction-model.md's "Ownership Boundary").
    const auto withDisplayShortcut = [](QAction* item, const QKeySequence& sequence) {
        item->setShortcut(sequence);
        item->setShortcutContext(Qt::WidgetShortcut);
        return item;
    };

    // hands it to EditorArea (which reparents it into the header, exactly like the footer). Every
    // menu below is parented to `bar` rather than to `this`, so the whole header-menu subtree --
    // buttons, menus, and actions alike -- travels together on that reparent and is torn down
    // together, regardless of which of NodeGraphEditor or the bar happens to be destroyed first.
    auto* bar = &chrome_.header;
    bar->owner = this;
    bar->objectName = "nodeHeaderMenuBar";
    bar->overflowButtonName = "nodeHeaderOverflowButton";
    bar->overflowMenuName = "nodeHeaderOverflowMenu";

    // --- Add: the same categorized submenu the canvas's own right-click menu offers (deliverable
    // 1's "the existing categorized submenu"), built once and kept live via refreshAddMenuState()
    // rather than rebuilt -- see populateAddMenu()'s own comment for why. ---
    headerAddMenu_ = kit::makeMenu(this);
    headerAddMenu_->setTitle(tr("Add"));
    headerAddMenu_->setObjectName(QStringLiteral("nodeAddMenu"));
    populateAddMenu(headerAddMenu_, &addMenuItems_);
    connect(headerAddMenu_, &QMenu::aboutToShow, this, &NodeGraphEditor::refreshAddMenuState);

    // --- View ---
    headerViewMenu_ = kit::makeMenu(this);
    headerViewMenu_->setTitle(tr("View"));
    headerViewMenu_->setObjectName(QStringLiteral("nodeViewMenu"));
    withDisplayShortcut(action(headerViewMenu_, tr("Fit"), QStringLiteral("nodeFitAction"),
                               [this] { view_->frameGraph(); }),
                        QKeySequence(Qt::CTRL | Qt::Key_0));
    action(headerViewMenu_, tr("Frame Selected"), QStringLiteral("nodeFrameSelectedAction"),
           [this] { frameSelectedNodes(); });
    withDisplayShortcut(action(headerViewMenu_, tr("Actual Size"),
                               QStringLiteral("nodeActualSizeAction"),
                               [this] { view_->zoomToActualSize(); }),
                        QKeySequence(Qt::CTRL | Qt::Key_1));
    headerViewMenu_->addSeparator();
    action(headerViewMenu_, tr("Zoom In"), QStringLiteral("nodeZoomInAction"),
           [this] { view_->zoomStep(1); });
    action(headerViewMenu_, tr("Zoom Out"), QStringLiteral("nodeZoomOutAction"),
           [this] { view_->zoomStep(-1); });
    headerViewMenu_->addSeparator();
    gridSnapAction_ =
        action(headerViewMenu_, tr("Grid Snapping"), QStringLiteral("nodeGridSnapAction"),
               [this] { applyGridSnap(!scene_->gridSnapEnabled()); });
    gridSnapAction_->setCheckable(true);
    gridSnapAction_->setChecked(scene_->gridSnapEnabled());
    headerViewMenu_->addSeparator();
    auto* linkStyleMenu = headerViewMenu_->addMenu(tr("Link Style"));
    linkStyleMenu->setObjectName(QStringLiteral("nodeLinkStyleMenu"));
    auto* linkStyleGroup = new QActionGroup(linkStyleMenu);
    linkStyleGroup->setExclusive(true);
    const std::array<std::tuple<QString, QString, LinkStyle>, 3> linkStyleRows{{
        {tr("Spline"), QStringLiteral("nodeLinkStyleSplineAction"), LinkStyle::Spline},
        {tr("Straight"), QStringLiteral("nodeLinkStyleStraightAction"), LinkStyle::Straight},
        {tr("Angled"), QStringLiteral("nodeLinkStyleAngledAction"), LinkStyle::Angled},
    }};
    for (std::size_t i = 0; i < linkStyleRows.size(); ++i) {
        const auto& [label, name, style] = linkStyleRows[i];
        auto* item = action(linkStyleMenu, label, name, [this, style] { applyLinkStyle(style); });
        item->setCheckable(true);
        item->setChecked(style == scene_->linkStyle());
        linkStyleGroup->addAction(item);
        linkStyleActions_[i] = item;
    }
    connect(headerViewMenu_, &QMenu::aboutToShow, this, &NodeGraphEditor::refreshViewMenuState);

    // --- Select ---
    headerSelectMenu_ = kit::makeMenu(this);
    headerSelectMenu_->setTitle(tr("Select"));
    headerSelectMenu_->setObjectName(QStringLiteral("nodeSelectMenu"));
    withDisplayShortcut(action(headerSelectMenu_, tr("All"), QStringLiteral("nodeSelectAllAction"),
                               [this] { scene_->selectAllNodes(); }),
                        QKeySequence(Qt::CTRL | Qt::Key_A));
    action(headerSelectMenu_, tr("None"), QStringLiteral("nodeSelectNoneAction"),
           [this] { selectNoneNodes(); });
    action(headerSelectMenu_, tr("Invert"), QStringLiteral("nodeSelectInvertAction"),
           [this] { selectInvertNodes(); });
    headerSelectMenu_->addSeparator();
    action(headerSelectMenu_, tr("Linked Upstream"),
           QStringLiteral("nodeSelectLinkedUpstreamAction"), [this] { selectLinkedUpstream(); });
    action(headerSelectMenu_, tr("Linked Downstream"),
           QStringLiteral("nodeSelectLinkedDownstreamAction"),
           [this] { selectLinkedDownstream(); });
    connect(headerSelectMenu_, &QMenu::aboutToShow, this, &NodeGraphEditor::refreshSelectMenuState);

    // --- Node: every one of these reuses the exact objectName the canvas context menu's own node
    // commands already established (deliverable 1's "reuse the existing QActions"); a persistent
    // menu cannot literally share the SAME QAction instance the transient context menu builds fresh
    // per popup (buildContextMenu() depends on that popup-time freshness for its own honesty rule
    // -- an item that cannot apply is not merely disabled, it is absent), so this menu instead
    // disables an applicable-but-currently-inapplicable command rather than hiding it, refreshed on
    // every aboutToShow by refreshNodeMenuState(). ---
    headerNodeMenu_ = kit::makeMenu(this);
    headerNodeMenu_->setTitle(tr("Node"));
    headerNodeMenu_->setObjectName(QStringLiteral("nodeNodeMenu"));
    groupAction_ =
        withDisplayShortcut(action(headerNodeMenu_, tr("Group"), QStringLiteral("nodeGroupAction"),
                                   [this] { groupSelectedNodes(); }),
                            QKeySequence(Qt::CTRL | Qt::Key_G));
    ungroupAction_ = withDisplayShortcut(action(headerNodeMenu_, tr("Ungroup"),
                                                QStringLiteral("nodeUngroupAction"),
                                                [this] { ungroupSelection(); }),
                                         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));
    headerNodeMenu_->addSeparator();
    muteAction_ = action(headerNodeMenu_, tr("Mute"), QStringLiteral("nodeMuteAction"),
                         [this] { toggleSelectedMuted(true); });
    collapseAction_ = action(headerNodeMenu_, tr("Collapse"), QStringLiteral("nodeCollapseAction"),
                             [this] { toggleSelectedMuted(false); });
    headerNodeMenu_->addSeparator();
    renameAction_ = action(headerNodeMenu_, tr("Rename"), QStringLiteral("nodeRenameAction"),
                           [this] { renameSelectedLayer(); });
    dissolveAction_ = action(headerNodeMenu_, tr("Dissolve"), QStringLiteral("nodeDissolveAction"),
                             [this] { dissolveSelectedNode(); });
    headerNodeMenu_->addSeparator();
    deleteAction_ = withDisplayShortcut(action(headerNodeMenu_, tr("Delete"),
                                               QStringLiteral("nodeDeleteAction"),
                                               [this] { removeSelectedNodes(); }),
                                        QKeySequence(Qt::Key_Delete));
    connect(headerNodeMenu_, &QMenu::aboutToShow, this, &NodeGraphEditor::refreshNodeMenuState);

    // `bar` (created above) stays alive and usable through headerMenuForTest() even before

    bar->addTopLevelMenu(tr("Add"), headerAddMenu_);
    bar->addTopLevelMenu(tr("View"), headerViewMenu_);
    bar->addTopLevelMenu(tr("Select"), headerSelectMenu_);
    bar->addTopLevelMenu(tr("Node"), headerNodeMenu_);
    headerMenuWidget_ = EditorArea::buildChromeRow(chrome_.header, this);
}

void NodeGraphEditor::refreshViewMenuState() {
    if (gridSnapAction_ != nullptr) {
        gridSnapAction_->setChecked(scene_->gridSnapEnabled());
    }
    const auto style = scene_->linkStyle();
    for (std::size_t i = 0; i < linkStyleActions_.size(); ++i) {
        if (linkStyleActions_[i] != nullptr) {
            linkStyleActions_[i]->setChecked(static_cast<LinkStyle>(i) == style);
        }
    }
}

void NodeGraphEditor::refreshSelectMenuState() {
    const bool hasComposition = session_.composition() != nullptr;
    for (auto* item : headerSelectMenu_->actions()) {
        item->setEnabled(hasComposition);
    }
}

void NodeGraphEditor::refreshNodeMenuState() {
    const std::array<QAction*, 7> all{groupAction_,  ungroupAction_,  muteAction_,  collapseAction_,
                                      renameAction_, dissolveAction_, deleteAction_};
    const auto nodes = session_.selectedNodes();
    const bool hasSelection =
        !nodes.empty() && scene_->canSubmit() && session_.composition() != nullptr;
    if (!hasSelection) {
        for (auto* item : all) {
            if (item != nullptr) {
                item->setEnabled(false);
            }
        }
        muteAction_->setText(tr("Mute"));
        collapseAction_->setText(tr("Collapse"));
        return;
    }
    const auto composition = session_.compositionId();
    const auto accepts = [this](const commands::Operation& operation) {
        return commands::canApplyNodeOperation(session_.snapshot(), operation);
    };
    dissolveAction_->setEnabled(nodes.size() == 1 &&
                                accepts(commands::DissolveNode(composition, *nodes.begin())));
    const bool allMuted = std::ranges::all_of(
        nodes, [&](const auto id) { return layoutFor(*session_.composition(), id).muted; });
    const bool allCollapsed = std::ranges::all_of(
        nodes, [&](const auto id) { return layoutFor(*session_.composition(), id).collapsed; });
    muteAction_->setText(allMuted ? tr("Unmute") : tr("Mute"));
    muteAction_->setEnabled(std::ranges::all_of(nodes, [&](const auto id) {
        return accepts(commands::SetNodeMuted(composition, id, !allMuted));
    }));
    collapseAction_->setText(allCollapsed ? tr("Expand") : tr("Collapse"));
    collapseAction_->setEnabled(std::ranges::all_of(nodes, [&](const auto id) {
        return accepts(commands::SetNodeCollapsed(composition, id, !allCollapsed));
    }));
    bool canRename = false;
    if (nodes.size() == 1) {
        for (const auto& boundary : session_.composition()->graph().layerOutputs()) {
            if (boundary.nodeId == *nodes.begin() &&
                accepts(commands::RenameLayer(composition, boundary.layerId, boundary.name))) {
                canRename = true;
            }
        }
    }
    renameAction_->setEnabled(canRename);
    groupAction_->setEnabled(accepts(
        commands::GroupNodes(composition, nodes, std::string(commands::kDefaultNodeGroupName))));
    ungroupAction_->setEnabled(std::ranges::any_of(nodes, [&](const auto id) {
        return document::findNodeGroupOf(session_.composition()->nodeGroups(), id) != nullptr;
    }));
    deleteAction_->setEnabled(accepts(commands::RemoveNodes(composition, nodes)));
}

void NodeGraphEditor::applyLinkStyle(const LinkStyle style) {
    scene_->setLinkStyle(style);
    QSettings settings;
    settings.setValue(QStringLiteral("nodes/link-style"), linkStyleSettingsValue(style));
    for (std::size_t i = 0; i < linkStyleActions_.size(); ++i) {
        if (linkStyleActions_[i] != nullptr) {
            linkStyleActions_[i]->setChecked(static_cast<LinkStyle>(i) == style);
        }
    }
    if (footerLinkStyleDropdown_ != nullptr) {
        const QSignalBlocker blocker(footerLinkStyleDropdown_);
        footerLinkStyleDropdown_->setCurrentIndex(static_cast<int>(style));
    }
}

void NodeGraphEditor::applyGridSnap(const bool enabled) {
    scene_->setGridSnapEnabled(enabled);
    QSettings settings;
    settings.setValue(QStringLiteral("nodes/snap"), enabled);
    if (gridSnapAction_ != nullptr) {
        gridSnapAction_->setChecked(enabled);
    }
    if (footerSnapSwitch_ != nullptr) {
        const QSignalBlocker blocker(footerSnapSwitch_);
        footerSnapSwitch_->setChecked(enabled);
    }
}

QMenu* NodeGraphEditor::headerMenuForTest(const std::string_view which) const {
    if (which == "add")
        return headerAddMenu_;
    if (which == "view")
        return headerViewMenu_;
    if (which == "select")
        return headerSelectMenu_;
    if (which == "node")
        return headerNodeMenu_;
    return nullptr;
}

// --- Task NODES-1, deliverable 4: the footer
// ------------------------------------------------------

void NodeGraphEditor::refreshSelectionReadout() {
    if (footerSelectionLabel_ == nullptr) {
        return;
    }
    footerSelectionLabel_->setText(session_.selectedNodes().empty()
                                       ? QString{}
                                       : tr("%1 nodes").arg(session_.selectedNodes().size()));
    footerSelectionLabel_->setProperty("chromeSuppressed", session_.selectedNodes().empty());
    footerSelectionLabel_->setVisible(!session_.selectedNodes().empty());
}

void NodeGraphEditor::buildFooter() {
    // The same fixed presets the Viewer's own zoom dropdown offers (viewer_editor.cpp's
    // kZoomPresets) -- deliverable 4's "same items as the viewer's".
    constexpr std::array<int, 5> kZoomPresets{25, 50, 100, 200, 400};

    auto* layout = &chrome_.footer;
    layout->objectName = "nodeFooter";
    footerZoomDropdown_ = new kit::KDropdown(this);
    footerZoomDropdown_->setObjectName(QStringLiteral("nodeZoomDropdown"));
    footerZoomDropdown_->setAccessibleName(tr("Zoom"));
    footerZoomDropdown_->setControlSize(kit::KDropdown::ControlSize::Compact);
    footerZoomDropdown_->addItem(tr("Fit"), 0);
    for (const int percent : kZoomPresets) {
        footerZoomDropdown_->addItem(tr("%1%").arg(percent), percent);
    }
    connect(footerZoomDropdown_, &kit::KDropdown::currentIndexChanged, this, [this](int index) {
        if (index <= 0) {
            view_->frameGraph();
            return;
        }
        view_->zoomToPercent(footerZoomDropdown_->itemData(index).toInt());
    });
    layout->addWidget(footerZoomDropdown_);

    auto* snapSwitch = new kit::KSwitch(this);
    snapSwitch->setObjectName(QStringLiteral("nodeSnapSwitch"));
    snapSwitch->setAccessibleName(tr("Grid Snapping"));
    snapSwitch->setToolTip(tr("Grid Snapping"));
    {
        // No signal yet -- applyGridSnap() writes QSettings, and this initial state merely mirrors
        // what the scene already loaded FROM QSettings a moment ago; reasserting it would be a
        // pointless (if harmless) redundant write on every construction.
        const QSignalBlocker blocker(snapSwitch);
        snapSwitch->setChecked(scene_->gridSnapEnabled());
    }
    connect(snapSwitch, &kit::KSwitch::toggled, this,
            [this](bool checked) { applyGridSnap(checked); });
    footerSnapSwitch_ = snapSwitch;
    snapSwitch->hide();

    footerLinkStyleDropdown_ = new kit::KDropdown(this);
    footerLinkStyleDropdown_->setObjectName(QStringLiteral("nodeLinkStyleDropdown"));
    footerLinkStyleDropdown_->setAccessibleName(tr("Link Style"));
    footerLinkStyleDropdown_->setControlSize(kit::KDropdown::ControlSize::Compact);
    footerLinkStyleDropdown_->addItem(tr("Spline"), static_cast<int>(LinkStyle::Spline));
    footerLinkStyleDropdown_->addItem(tr("Straight"), static_cast<int>(LinkStyle::Straight));
    footerLinkStyleDropdown_->addItem(tr("Angled"), static_cast<int>(LinkStyle::Angled));
    {
        const QSignalBlocker blocker(footerLinkStyleDropdown_);
        footerLinkStyleDropdown_->setCurrentIndex(static_cast<int>(scene_->linkStyle()));
    }
    connect(footerLinkStyleDropdown_, &kit::KDropdown::currentIndexChanged, this,
            [this](int index) {
                applyLinkStyle(
                    static_cast<LinkStyle>(footerLinkStyleDropdown_->itemData(index).toInt()));
            });
    footerLinkStyleDropdown_->hide();

    layout->addStretch(1);

    footerSelectionLabel_ = new kit::KLabel(this);
    footerSelectionLabel_->setObjectName(QStringLiteral("nodeSelectionReadout"));
    footerSelectionLabel_->setFont(kit::font(kit::TypeRole::UiSmall));
    QPalette palette = footerSelectionLabel_->palette();
    palette.setColor(QPalette::WindowText, kit::color(kit::Color::Muted));
    footerSelectionLabel_->setPalette(palette);
    layout->addWidget(footerSelectionLabel_);

    connect(&session_, &CompositionSession::selectionChanged, this,
            &NodeGraphEditor::refreshSelectionReadout);
    footerWidget_ = EditorArea::buildChromeRow(chrome_.footer, this, true);
    refreshSelectionReadout();
}

QWidget* NodeGraphEditor::footerWidgetForTest() { return footerWidget_; }

} // namespace bloom::ui
