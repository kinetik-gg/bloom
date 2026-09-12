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
    bool changed = false;
    for (const auto& [id, position] : positions_) {
        auto record = detail::layoutFor(*composition, id);
        if (record.position == position)
            continue;
        record.position = position;
        composition->nodeLayout()[id] = record;
        changed = true;
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
