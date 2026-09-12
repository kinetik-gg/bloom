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

void NodeGraphEditor::handleCanvasKey(const int key, const Qt::KeyboardModifiers modifiers) {
    if (key == Qt::Key_Escape) {
        scene_->cancelGesture();
        return;
    }
    if (key == Qt::Key_A && modifiers == Qt::ControlModifier) {
        scene_->selectAllNodes();
        return;
    }
    if (key == Qt::Key_A && modifiers == Qt::ShiftModifier) {
        const QPoint global = QCursor::pos();
        openAddSearch(view_->sceneFromViewport(view_->viewport()->mapFromGlobal(global)), global);
        return;
    }
    if (!scene_->canSubmit()) {
        showStatus(tr("Node command submission is unavailable"));
        return;
    }
    const auto nodes = session_.selectedNodes();
    if (nodes.empty() || !session_.composition()) {
        showStatus(tr("Select a node first"));
        return;
    }
    if (scene_->gestureActive())
        scene_->cancelGesture();
    const auto composition = session_.compositionId();
    commands::Transaction transaction("Edit Nodes", session_.snapshot().revision());
    if ((key == Qt::Key_Delete || key == Qt::Key_X) && modifiers == Qt::NoModifier)
        transaction.emplace<commands::RemoveNodes>(composition, nodes);
    else if (key == Qt::Key_X && modifiers == Qt::ControlModifier) {
        if (nodes.size() != 1) {
            showStatus(tr("Dissolve requires exactly one selected node"));
            return;
        }
        transaction.emplace<commands::DissolveNode>(composition, *nodes.begin());
    } else if (key == Qt::Key_D && modifiers == Qt::ShiftModifier)
        transaction.emplace<commands::DuplicateNodes>(composition, nodes, document::Vec2d{24, 24});
    else if (key == Qt::Key_M || key == Qt::Key_H) {
        const bool mute = key == Qt::Key_M;
        const bool target = std::ranges::any_of(nodes, [&](const auto id) {
            const auto layout = layoutFor(*session_.composition(), id);
            return !(mute ? layout.muted : layout.collapsed);
        });
        for (const auto id : nodes) {
            if (mute)
                transaction.emplace<commands::SetNodeMuted>(composition, id, target);
            else
                transaction.emplace<commands::SetNodeCollapsed>(composition, id, target);
        }
    }
    if (transaction.empty())
        return;
    const auto result = scene_->submit(std::move(transaction));
    if (!result.succeeded())
        return;
    if (key == Qt::Key_D) {
        std::set<document::NodeId> copies;
        for (const auto id : nodes) {
            const auto copy =
                result.outputId<document::NodeId>("node." + std::to_string(id.value()));
            if (copy)
                copies.insert(*copy);
        }
        if (!copies.empty()) {
            session_.selectNodes(copies, *copies.begin());
            scene_->startDuplicateMove(
                view_->sceneFromViewport(view_->viewport()->mapFromGlobal(QCursor::pos())));
        }
    }
}

QMenu* NodeGraphEditor::buildContextMenu(QWidget* parent, const bool nodeMenu) {
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
                   [this] { handleCanvasKey(Qt::Key_D, Qt::ShiftModifier); });
        if (nodes.size() == 1 && accepts(commands::DissolveNode(composition, *nodes.begin())))
            action(tr("Dissolve"), QStringLiteral("nodeDissolveAction"),
                   [this] { handleCanvasKey(Qt::Key_X, Qt::ControlModifier); });
        const bool allMuted = std::ranges::all_of(
            nodes, [&](const auto id) { return layoutFor(*session_.composition(), id).muted; });
        const bool allCollapsed = std::ranges::all_of(
            nodes, [&](const auto id) { return layoutFor(*session_.composition(), id).collapsed; });
        if (std::ranges::all_of(nodes, [&](const auto id) {
                return accepts(commands::SetNodeMuted(composition, id, !allMuted));
            }))
            action(allMuted ? tr("Unmute") : tr("Mute"), QStringLiteral("nodeMuteAction"),
                   [this] { handleCanvasKey(Qt::Key_M, Qt::NoModifier); });
        if (std::ranges::all_of(nodes, [&](const auto id) {
                return accepts(commands::SetNodeCollapsed(composition, id, !allCollapsed));
            }))
            action(allCollapsed ? tr("Expand") : tr("Collapse"),
                   QStringLiteral("nodeCollapseAction"),
                   [this] { handleCanvasKey(Qt::Key_H, Qt::NoModifier); });
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
        if (accepts(commands::RemoveNodes(composition, nodes)))
            action(tr("Delete"), QStringLiteral("nodeDeleteAction"),
                   [this] { handleCanvasKey(Qt::Key_Delete, Qt::NoModifier); });
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
        auto* item = addMenu->addAction(displayTypeName(definition.key.typeId));
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

QMenu* NodeGraphEditor::contextMenuForTest(const bool nodeMenu) {
    return buildContextMenu(this, nodeMenu);
}
void NodeGraphEditor::showContextMenu(const QPoint& viewportPosition) {
    auto* card = nodeItemAncestor(view_->itemAt(viewportPosition));
    if (card && !session_.selectedNodes().contains(card->id()))
        session_.selectNode(card->id());
    addPosition_ = view_->sceneFromViewport(viewportPosition);
    addInput_.reset();
    addOutput_.reset();
    addRevision_ = session_.snapshot().revision();
    const QPointer<QMenu> menu = buildContextMenu(view_, card != nullptr && scene_->canSubmit());
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
            return displayTypeName(left->key.typeId) < displayTypeName(right->key.typeId);
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
                           displayTypeName(definition.key.typeId), keywords, refusal,
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
