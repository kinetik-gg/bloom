#pragma once

#include <bloom/commands/operation.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/node_layout.hpp>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace bloom::commands {
// Checks the exact operation and final document validation on an isolated draft. Never publishes,
// advances the caller's allocator, or touches history. Menus must not duplicate command policy.
[[nodiscard]] bool canApplyNodeOperation(const document::Snapshot& snapshot,
                                         const Operation& operation);

inline constexpr std::string_view kAddNodeOutput = "node";
inline constexpr std::string_view kGroupNodesOutput = "nodeGroup";
inline constexpr std::string_view kDefaultNodeGroupName = "Group";

// Where a moved node ends up: a group id to land in, or nullopt to leave whatever group it was in.
// A drag that crosses a group frame's boundary carries this alongside its positions so the move and
// the membership change are ONE transaction, and therefore one undo.
using NodeGroupMembershipDelta = std::map<document::NodeId, std::optional<document::NodeGroupId>>;

class AddNode final : public Operation {
  public:
    AddNode(document::CompositionId compositionId, std::string nodeTypeId,
            document::Vec2d layoutPosition,
            const document::NodeDefinitionRegistry& registry = document::builtInNodeDefinitions())
        : compositionId_(compositionId), nodeTypeId_(std::move(nodeTypeId)),
          layoutPosition_(layoutPosition), registry_(registry) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    std::string nodeTypeId_;
    document::Vec2d layoutPosition_;
    const document::NodeDefinitionRegistry& registry_;
};

class RemoveNodes final : public Operation {
  public:
    RemoveNodes(document::CompositionId compositionId, std::set<document::NodeId> nodes)
        : compositionId_(compositionId), nodes_(std::move(nodes)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    std::set<document::NodeId> nodes_;
};

class DuplicateNodes final : public Operation {
  public:
    DuplicateNodes(document::CompositionId compositionId, std::set<document::NodeId> nodes,
                   document::Vec2d offset)
        : compositionId_(compositionId), nodes_(std::move(nodes)), offset_(offset) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    std::set<document::NodeId> nodes_;
    document::Vec2d offset_;
};

class RenameLayer final : public Operation {
  public:
    RenameLayer(document::CompositionId compositionId, document::LayerId layerId, std::string name)
        : compositionId_(compositionId), layerId_(layerId), name_(std::move(name)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::LayerId layerId_;
    std::string name_;
};

// The stack slot a link into Merge's ordered multi-input should land BEFORE, or nothing to append
// at the bottom. Only meaningful for a `LayerStackInputRef` destination whose slot id is invalid --
// the "a new slot here" sentinel (task FIX1, item B).
inline constexpr std::string_view kConnectPortsSlotOutput = "layerSlot";

class ConnectPorts final : public Operation {
  public:
    ConnectPorts(
        document::CompositionId compositionId, document::OutputPortRef source,
        document::InputPortRef destination,
        const document::NodeDefinitionRegistry& registry = document::builtInNodeDefinitions(),
        std::optional<document::LayerSlotId> insertBefore = std::nullopt)
        : compositionId_(compositionId), source_(std::move(source)),
          destination_(std::move(destination)), registry_(registry), insertBefore_(insertBefore) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::OutputPortRef source_;
    document::InputPortRef destination_;
    const document::NodeDefinitionRegistry& registry_;
    std::optional<document::LayerSlotId> insertBefore_;
};

class SetMergeEnabled final : public Operation {
  public:
    SetMergeEnabled(document::CompositionId compositionId, document::NodeId mergeId, bool enabled)
        : compositionId_(compositionId), mergeId_(mergeId), enabled_(enabled) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::NodeId mergeId_;
    bool enabled_;
};

class ReorderMergeInput final : public Operation {
  public:
    ReorderMergeInput(document::CompositionId compositionId, document::NodeId mergeId,
                      document::LayerSlotId slotId, std::size_t newIndex)
        : compositionId_(compositionId), mergeId_(mergeId), slotId_(slotId), newIndex_(newIndex) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::NodeId mergeId_;
    document::LayerSlotId slotId_;
    std::size_t newIndex_;
};

class DisconnectInput final : public Operation {
  public:
    DisconnectInput(
        document::CompositionId compositionId, document::InputPortRef input,
        const document::NodeDefinitionRegistry& registry = document::builtInNodeDefinitions())
        : compositionId_(compositionId), input_(std::move(input)), registry_(registry) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::InputPortRef input_;
    const document::NodeDefinitionRegistry& registry_;
};

class DissolveNode final : public Operation {
  public:
    DissolveNode(
        document::CompositionId compositionId, document::NodeId nodeId,
        const document::NodeDefinitionRegistry& registry = document::builtInNodeDefinitions())
        : compositionId_(compositionId), nodeId_(nodeId), registry_(registry) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::NodeId nodeId_;
    const document::NodeDefinitionRegistry& registry_;
};

class MoveNodes final : public Operation {
  public:
    MoveNodes(document::CompositionId compositionId,
              std::map<document::NodeId, document::Vec2d> positions,
              NodeGroupMembershipDelta membership = {})
        : compositionId_(compositionId), positions_(std::move(positions)),
          membership_(std::move(membership)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    std::map<document::NodeId, document::Vec2d> positions_;
    NodeGroupMembershipDelta membership_;
};

// Node groups (layout, never semantics -- see bloom/document/node_layout.hpp). Each of these is one
// transaction and one undo entry like every other node command, and each removes any group it
// leaves with no members at all: an empty frame is not something an artist asked for, and keeping
// one would leave a nameplate floating over nothing.

class GroupNodes final : public Operation {
  public:
    GroupNodes(document::CompositionId compositionId, std::set<document::NodeId> nodes,
               std::string name = std::string(kDefaultNodeGroupName))
        : compositionId_(compositionId), nodes_(std::move(nodes)), name_(std::move(name)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    std::set<document::NodeId> nodes_;
    std::string name_;
};

class UngroupNodes final : public Operation {
  public:
    UngroupNodes(document::CompositionId compositionId, document::NodeGroupId groupId)
        : compositionId_(compositionId), groupId_(groupId) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::NodeGroupId groupId_;
};

class RenameGroup final : public Operation {
  public:
    RenameGroup(document::CompositionId compositionId, document::NodeGroupId groupId,
                std::string name)
        : compositionId_(compositionId), groupId_(groupId), name_(std::move(name)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::NodeGroupId groupId_;
    std::string name_;
};

class SetGroupMembers final : public Operation {
  public:
    SetGroupMembers(document::CompositionId compositionId, document::NodeGroupId groupId,
                    std::set<document::NodeId> members)
        : compositionId_(compositionId), groupId_(groupId), members_(std::move(members)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::NodeGroupId groupId_;
    std::set<document::NodeId> members_;
};

class SetNodeCollapsed final : public Operation {
  public:
    SetNodeCollapsed(document::CompositionId compositionId, document::NodeId nodeId, bool collapsed)
        : compositionId_(compositionId), nodeId_(nodeId), collapsed_(collapsed) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::NodeId nodeId_;
    bool collapsed_;
};

class SetNodeMuted final : public Operation {
  public:
    SetNodeMuted(document::CompositionId compositionId, document::NodeId nodeId, bool muted)
        : compositionId_(compositionId), nodeId_(nodeId), muted_(muted) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::NodeId nodeId_;
    bool muted_;
};

class SetNodeWidth final : public Operation {
  public:
    SetNodeWidth(document::CompositionId compositionId, document::NodeId nodeId, double width)
        : compositionId_(compositionId), nodeId_(nodeId), width_(width) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::NodeId nodeId_;
    double width_;
};

} // namespace bloom::commands
