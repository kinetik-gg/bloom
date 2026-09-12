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
} // namespace bloom::ui::node_editor
