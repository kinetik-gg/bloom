#pragma once
#include <bloom/commands/operations.hpp>

namespace bloom::ui::node_editor {
// Resolves freshly allocated IDs inside one command operation, delegating every durable write to
// N1/N2 commands. This keeps add + cursor layout + optional connection in one undo entry.
class AddEditorNode final : public commands::Operation {
  public:
    AddEditorNode(document::CompositionId composition, std::string type, document::Vec2d position,
                  std::optional<document::InputPortRef> input = {},
                  std::optional<document::OutputPortRef> output = {})
        : composition_(composition), type_(std::move(type)), position_(position),
          input_(std::move(input)), output_(std::move(output)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.node.add-at-cursor";
    }
    [[nodiscard]] commands::OperationResult apply(document::Draft& draft) const override;

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
class InsertReroute final : public commands::Operation {
  public:
    InsertReroute(document::CompositionId composition, document::InputPortRef destination,
                  document::Vec2d position)
        : composition_(composition), destination_(std::move(destination)), position_(position) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.node.insert-reroute";
    }
    [[nodiscard]] commands::OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId composition_;
    document::InputPortRef destination_;
    document::Vec2d position_;
};
} // namespace bloom::ui::node_editor
