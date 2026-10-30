#pragma once
// The Node editor's compound adds: each resolves freshly allocated ids inside one operation and
// delegates every durable write to the ordinary node and layer commands, so one add is one undo
// step. They are Qt-free so scripts, MCP and the editor issue the same command.
#include <bloom/commands/operations.hpp>

namespace bloom::commands {
// Read-only Add-surface preflight over one immutable snapshot. A successful result is only an
// invitation to submit; AddEditorNode::apply remains the authoritative command path.
[[nodiscard]] OperationResult checkAddEditorNodeEligibility(
    const document::Snapshot& snapshot, document::CompositionId composition, std::string_view type,
    document::Vec2d position, const std::optional<document::InputPortRef>& input = {},
    const std::optional<document::OutputPortRef>& output = {});

// Resolves freshly allocated IDs inside one command operation, delegating every durable write to
// N1/N2 commands. This keeps add + cursor layout + optional connection in one undo entry.
class AddCompositionSource final : public Operation {
  public:
    AddCompositionSource(document::CompositionId composition, document::CompositionId source,
                         document::Vec2d position = {}, bool asLayer = false)
        : composition_(composition), source_(source), position_(position), asLayer_(asLayer) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.node.add-composition-source";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId composition_, source_;
    document::Vec2d position_;
    bool asLayer_;
};

class AddEditorNode final : public Operation {
  public:
    AddEditorNode(document::CompositionId composition, std::string type, document::Vec2d position,
                  std::optional<document::InputPortRef> input = {},
                  std::optional<document::OutputPortRef> output = {})
        : composition_(composition), type_(std::move(type)), position_(position),
          input_(std::move(input)), output_(std::move(output)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.node.add-at-cursor";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId composition_;
    std::string type_;
    document::Vec2d position_;
    std::optional<document::InputPortRef> input_;
    std::optional<document::OutputPortRef> output_;
};

// Task FIX1, item I: inserts a reroute INTO an existing link, in one transaction and therefore one
// undo step -- add the node at the drop point, rewire the link's destination to read from it, and
// feed it from the link's original source. Resolving the freshly allocated node id inside one
// operation is the same reason AddEditorNode exists.
class InsertReroute final : public Operation {
  public:
    InsertReroute(document::CompositionId composition, document::InputPortRef destination,
                  document::Vec2d position)
        : composition_(composition), destination_(std::move(destination)), position_(position) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.node.insert-reroute";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId composition_;
    document::InputPortRef destination_;
    document::Vec2d position_;
};

// An asset dropped on the Node canvas: an Image Source (a Video Source for a video asset) whose
// asset parameter names it and whose premultiply flag follows the asset's probed alpha. Outputs
// the AddNode outputs.
class AddImageNode final : public Operation {
  public:
    AddImageNode(document::CompositionId composition, document::AssetId asset,
                 document::Vec2d position)
        : composition_(composition), asset_(asset), position_(position) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.node.add-image";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId composition_;
    document::AssetId asset_;
    document::Vec2d position_;
};

// An audio asset dropped on the Node canvas: an Audio Source naming it.
class AddAudioNode final : public Operation {
  public:
    AddAudioNode(document::CompositionId composition, document::AssetId asset,
                 document::Vec2d position)
        : composition_(composition), asset_(asset), position_(position) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.node.add-audio";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId composition_;
    document::AssetId asset_;
    document::Vec2d position_;
};

// Either of the two above, chosen by the asset's kind when it applies: what the Node canvas does
// with any footage asset dropped on it.
class AddAssetSourceNode final : public Operation {
  public:
    AddAssetSourceNode(document::CompositionId composition, document::AssetId asset,
                       document::Vec2d position)
        : composition_(composition), asset_(asset), position_(position) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.node.add-asset-source";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    document::AssetId asset_;
    document::Vec2d position_;
};
} // namespace bloom::commands
