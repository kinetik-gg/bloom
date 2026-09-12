#include "node_editor_add.hpp"
#include "node_editor_items.hpp"
#include <QCursor>
#include <QMenu>
#include <bloom/ui/kit/search_popup.hpp>

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
    return QStringLiteral("nodeAddCompositionOutputAction");
}
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

QMenu* NodeGraphEditor::buildContextMenu(QWidget* parent, const bool nodeMenu,
                                         const std::optional<document::NodeGroupId> group) {
    addRevision_ = session_.snapshot().revision();
    auto* menu = new QMenu(parent);
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
    if (scene_->canSubmit()) {
        action(tr("Add…"), QStringLiteral("nodeAddSearchAction"), [this] {
            openAddSearch(addPosition_,
                          view_->viewport()->mapToGlobal(view_->mapFromScene(addPosition_)));
        })->setEnabled(session_.composition() != nullptr);
    }
    // Preserve existing action names. Without an application submission adapter, only the existing
    // session Add Solid path is offered; a search promising cursor placement would be misleading.
    auto* addMenu = scene_->canSubmit() ? new QMenu(menu) : menu->addMenu(tr("Add"));
    addMenu->setObjectName(QStringLiteral("nodeAddMenu"));
    for (const auto& definition : document::builtInNodeDefinitions().definitions()) {
        auto* item = addMenu->addAction(nodeTypeDisplayName(definition.key.typeId));
        item->setObjectName(addActionName(definition.key.typeId));
        if (!scene_->canSubmit()) {
            const bool solid = definition.key.typeId == document::kSolidSourceNodeType;
            const bool text = definition.key.typeId == document::kTextSourceNodeType;
            item->setVisible(solid || text);
            item->setEnabled(solid && session_.composition() != nullptr);
            if (text)
                item->setToolTip(tr("Text requires a portable CPU font pipeline"));
        }
        connect(item, &QAction::triggered, this,
                [this, type = QString::fromStdString(definition.key.typeId)] { addNode(type); });
    }
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
void NodeGraphEditor::showContextMenu(const QPoint& viewportPosition) {
    auto* card = nodeItemAncestor(view_->itemAt(viewportPosition));
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
    for (const auto category : nodeCategoryOrder()) {
        std::vector<const document::NodeDefinition*> section;
        for (const auto& definition : document::builtInNodeDefinitions().definitions())
            if (definition.category == category)
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
        else if (!scene_->canSubmit() && definition.key.typeId != document::kSolidSourceNodeType)
            refusal = tr("Node command submission is unavailable");
        entries.push_back({QString::fromStdString(definition.key.typeId),
                           nodeTypeDisplayName(definition.key.typeId), keywords, refusal,
                           nodeCategoryName(definition.category)});
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
    commands::Transaction transaction(
        type.toStdString() == document::kSolidSourceNodeType ? "Add Solid Layer" : "Add Node",
        addRevision_);
    transaction.emplace<AddEditorNode>(session_.compositionId(), type.toStdString(),
                                       document::Vec2d{addPosition_.x(), addPosition_.y()},
                                       addInput_, addOutput_);
    const auto result = scene_->submit(std::move(transaction));
    if (const auto id = result.outputId<document::NodeId>("editorNode"); result.succeeded() && id)
        session_.selectNode(*id);
    view_->setFocus(Qt::PopupFocusReason);
}
} // namespace bloom::ui
