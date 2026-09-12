#pragma once

#include <bloom/commands/operation.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/node_layout.hpp>

#include <map>
#include <set>
#include <string>
#include <utility>

namespace bloom::commands {
inline constexpr std::string_view kAddNodeOutput = "node";

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

class ConnectPorts final : public Operation {
  public:
    ConnectPorts(
        document::CompositionId compositionId, document::OutputPortRef source,
        document::InputPortRef destination,
        const document::NodeDefinitionRegistry& registry = document::builtInNodeDefinitions())
        : compositionId_(compositionId), source_(std::move(source)),
          destination_(std::move(destination)), registry_(registry) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    document::OutputPortRef source_;
    document::InputPortRef destination_;
    const document::NodeDefinitionRegistry& registry_;
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
              std::map<document::NodeId, document::Vec2d> positions)
        : compositionId_(compositionId), positions_(std::move(positions)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId compositionId_;
    std::map<document::NodeId, document::Vec2d> positions_;
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
