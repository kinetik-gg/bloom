#include <bloom/commands/node_operations.hpp>

#include "node_operation_support.hpp"

namespace bloom::commands {
std::string_view MoveNodes::typeId() const noexcept { return "bloom.node.move"; }
OperationResult MoveNodes::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition)
        return detail::invalidTarget();
    for (const auto& [id, position] : positions_) {
        if (!composition->graph().findNode(id))
            return detail::invalidTarget();
        if (!detail::finite(position))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Node position must be finite");
    }
    for (const auto& [id, group] : membership_) {
        if (!composition->graph().findNode(id))
            return detail::invalidTarget();
        if (group && !composition->nodeGroups().contains(*group))
            return detail::invalidTarget();
    }
    bool changed = false;
    for (const auto& [id, position] : positions_) {
        auto record = detail::layoutFor(*composition, id);
        if (record.position == position)
            continue;
        record.position = position;
        composition->nodeLayout()[id] = record;
        changed = true;
    }
    // Membership after position, in this same operation and therefore this same transaction: a drag
    // that ends inside or outside a frame is one gesture and one undo, never a move the artist can
    // undo away from the grouping it caused.
    for (const auto& [id, group] : membership_) {
        const auto* current = document::findNodeGroupOf(composition->nodeGroups(), id);
        if (current && group && current->id == *group)
            continue;
        if (!current && !group)
            continue;
        changed = detail::detachFromNodeGroups(*composition, {id}, group) || changed;
        if (group) {
            const auto landing = composition->nodeGroups().find(*group);
            // detachFromNodeGroups() never prunes `group` itself, so the landing frame is still
            // here; the guard is a truthful refusal rather than an assumption if that ever changes.
            if (landing == composition->nodeGroups().end())
                return detail::invalidTarget();
            changed = landing->second.members.insert(id).second || changed;
        }
    }
    return changed ? OperationResult::applied() : OperationResult::noChange();
}

std::string_view SetNodeCollapsed::typeId() const noexcept { return "bloom.node.set-collapsed"; }
OperationResult SetNodeCollapsed::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition || !composition->graph().findNode(nodeId_))
        return detail::invalidTarget();
    auto record = detail::layoutFor(*composition, nodeId_);
    if (record.collapsed == collapsed_)
        return OperationResult::noChange();
    record.collapsed = collapsed_;
    composition->nodeLayout()[nodeId_] = record;
    return OperationResult::applied();
}

std::string_view SetNodeMuted::typeId() const noexcept { return "bloom.node.set-muted"; }
OperationResult SetNodeMuted::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition || !composition->graph().findNode(nodeId_))
        return detail::invalidTarget();
    auto record = detail::layoutFor(*composition, nodeId_);
    if (record.muted == muted_)
        return OperationResult::noChange();
    for (const auto& boundary : composition->graph().layerOutputs())
        if (boundary.nodeId == nodeId_) composition->graph().findLayer(boundary.layerId)->enabled = !muted_;
    record.muted = muted_;
    composition->nodeLayout()[nodeId_] = record;
    return OperationResult::applied();
}

std::string_view SetNodeWidth::typeId() const noexcept { return "bloom.node.set-width"; }
OperationResult SetNodeWidth::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition || !composition->graph().findNode(nodeId_))
        return detail::invalidTarget();
    if (!std::isfinite(width_) || width_ <= 0.0)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Node width must be positive and finite");
    auto record = detail::layoutFor(*composition, nodeId_);
    if (record.width == width_)
        return OperationResult::noChange();
    record.width = width_;
    composition->nodeLayout()[nodeId_] = record;
    return OperationResult::applied();
}
} // namespace bloom::commands
