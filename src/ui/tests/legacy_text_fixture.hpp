#pragma once

#include <bloom/commands/operations.hpp>
#include <bloom/ui/composition_session.hpp>

#include <stdexcept>
#include <string>

namespace bloom::ui::test {
// Existing files may contain text. Install that persisted topology through a test-only operation
// so ordinary publication signals preserve preview pixels and the single-selection assertions.
class LegacyTextFixture final : public commands::Operation {
  public:
    LegacyTextFixture(document::CompositionId compositionId, std::string name, std::string text)
        : compositionId_(compositionId), name_(std::move(name)), text_(std::move(text)) {}
    std::string_view typeId() const noexcept override { return "test.legacy-text"; }
    commands::OperationResult apply(document::Draft& draft) const override {
        commands::AddSolidLayer scaffold(compositionId_, name_, {1, 1, 1, 1}, {0, 0});
        const auto added = scaffold.apply(draft);
        if (added.status != commands::OperationStatus::Applied)
            throw std::logic_error("legacy fixture scaffold");
        document::NodeId source;
        document::ParameterId parameter;
        for (const auto& output : added.outputs) {
            if (output.name == commands::kAddSolidLayerSolidNodeOutput)
                source = std::get<document::NodeId>(output.id);
            if (output.name == commands::kAddSolidLayerColorParameterOutput)
                parameter = std::get<document::ParameterId>(output.id);
        }
        auto* composition = draft.project().findComposition(compositionId_);
        auto* node = composition->graph().findNode(source);
        if (!node || !composition->parameters().erase(parameter) ||
            !composition->parameters().insert({parameter,
                                               std::string(document::kTextParameterSchemaKey),
                                               document::ConstantValueSource{text_}}))
            throw std::logic_error("legacy fixture text parameter");
        node->typeId = std::string(document::kTextSourceNodeType);
        node->schemaVersion = document::kTextSourceNodeSchemaVersion;
        node->parameters = {{std::string(document::kTextParameterRole), parameter}};
        return added;
    }

  private:
    document::CompositionId compositionId_;
    std::string name_;
    std::string text_;
};

inline void installLegacyTextLayer(document::Document& document, commands::CommandStack& stack,
                                   CompositionSession& session, const std::string& name,
                                   const std::string& text) {
    commands::Transaction transaction("Legacy text fixture", document.snapshot().revision());
    transaction.emplace<LegacyTextFixture>(session.compositionId(), name, text);
    const auto added = stack.execute(std::move(transaction));
    const auto layer = added.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
    if (!added.changed() || !layer || !stack.undo().changed() || !session.redo())
        throw std::logic_error("legacy fixture publication");
    session.selectLayer(*layer);
}
} // namespace bloom::ui::test
