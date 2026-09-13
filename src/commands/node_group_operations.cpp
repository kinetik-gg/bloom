#include <bloom/commands/node_operations.hpp>

#include "node_operation_support.hpp"

#include <bloom/document/persisted_text.hpp>

namespace bloom::commands {
namespace {
// Every group command starts the same way: find the composition, then find the group inside it.
// A missing composition and a missing group are the same refusal -- the caller named something that
// is not there.
struct Target final {
    document::Composition* composition = nullptr;
    document::NodeGroupRecord* group = nullptr;
};

[[nodiscard]] Target findGroup(document::Draft& draft, const document::CompositionId compositionId,
                               const document::NodeGroupId groupId) {
    auto* composition = draft.project().findComposition(compositionId);
    if (!composition)
        return {};
    const auto found = composition->nodeGroups().find(groupId);
    return {composition, found == composition->nodeGroups().end() ? nullptr : &found->second};
}

[[nodiscard]] OperationResult invalidName() {
    return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                     "Node group name must be valid nonempty UTF-8");
}
} // namespace

std::string_view GroupNodes::typeId() const noexcept { return "bloom.node-group.create"; }
OperationResult GroupNodes::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition)
        return detail::invalidTarget();
    if (nodes_.empty())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "A node group needs at least one node");
    if (!document::isValidHumanFacingName(name_))
        return invalidName();
    for (const auto id : nodes_)
        if (!composition->graph().findNode(id))
            return detail::invalidTarget();
    const auto groupId = draft.ids().allocateNodeGroup();
    if (!groupId)
        return detail::exhaustedIds();
    // The new frame takes its members with it: a node belongs to exactly one group, so whatever
    // held them before loses them, and loses itself if that was all it held.
    (void)detail::detachFromNodeGroups(*composition, nodes_);
    // Field by field, so the record's own frozen default padding applies rather than a zero this
    // command would be inventing.
    document::NodeGroupRecord record;
    record.id = *groupId;
    record.name = name_;
    record.members = nodes_;
    composition->nodeGroups()[*groupId] = std::move(record);
    return OperationResult::applied({{std::string(kGroupNodesOutput), *groupId}});
}

std::string_view UngroupNodes::typeId() const noexcept { return "bloom.node-group.remove"; }
OperationResult UngroupNodes::apply(document::Draft& draft) const {
    const auto target = findGroup(draft, compositionId_, groupId_);
    if (!target.group)
        return detail::invalidTarget();
    // Only the frame goes. Its members keep their positions, their sizes and their wiring: a group
    // never owned any of that.
    target.composition->nodeGroups().erase(groupId_);
    return OperationResult::applied();
}

std::string_view RenameGroup::typeId() const noexcept { return "bloom.node-group.rename"; }
OperationResult RenameGroup::apply(document::Draft& draft) const {
    const auto target = findGroup(draft, compositionId_, groupId_);
    if (!target.group)
        return detail::invalidTarget();
    if (!document::isValidHumanFacingName(name_))
        return invalidName();
    if (target.group->name == name_)
        return OperationResult::noChange();
    target.group->name = name_;
    return OperationResult::applied();
}

std::string_view SetGroupMembers::typeId() const noexcept { return "bloom.node-group.set-members"; }
OperationResult SetGroupMembers::apply(document::Draft& draft) const {
    const auto target = findGroup(draft, compositionId_, groupId_);
    if (!target.group)
        return detail::invalidTarget();
    for (const auto id : members_)
        if (!target.composition->graph().findNode(id))
            return detail::invalidTarget();
    if (target.group->members == members_)
        return OperationResult::noChange();
    if (members_.empty()) {
        target.composition->nodeGroups().erase(groupId_);
        return OperationResult::applied();
    }
    // The incoming members leave every OTHER frame first -- this one is about to be rewritten
    // wholesale, and excluding it is what keeps it from being pruned on the way through.
    (void)detail::detachFromNodeGroups(*target.composition, members_, groupId_);
    target.group->members = members_;
    return OperationResult::applied();
}
} // namespace bloom::commands
