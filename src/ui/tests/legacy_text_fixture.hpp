#pragma once

#include <bloom/commands/operations.hpp>
#include <bloom/ui/composition_session.hpp>

#include <stdexcept>
#include <string>

namespace bloom::ui::test {
// Existing files may contain text. Build that persisted topology as a fixture now that the
// authoring command intentionally refuses new text layers; selection assertions remain unchanged.
inline void installLegacyTextLayer(document::Document& document, commands::CommandStack& stack,
                                   CompositionSession& session, const std::string& name,
                                   const std::string& text) {
    const auto before = document.snapshot();
    auto draft = document.draft(before);
    const auto compositionId = session.compositionId();
    commands::AddSolidLayer scaffold(compositionId, name, {1, 1, 1, 1}, {0, 0});
    const auto added = scaffold.apply(draft);
    if (added.status != commands::OperationStatus::Applied)
        throw std::logic_error("legacy fixture scaffold");
    document::NodeId source;
    document::LayerId layer;
    document::ParameterId parameter;
    for (const auto& output : added.outputs) {
        if (output.name == commands::kAddSolidLayerSolidNodeOutput)
            source = std::get<document::NodeId>(output.id);
        if (output.name == commands::kAddSolidLayerLayerOutput)
            layer = std::get<document::LayerId>(output.id);
        if (output.name == commands::kAddSolidLayerColorParameterOutput)
            parameter = std::get<document::ParameterId>(output.id);
    }
    auto* composition = draft.project().findComposition(compositionId);
    auto* node = composition->graph().findNode(source);
    if (!node || !composition->parameters().erase(parameter) ||
        !composition->parameters().insert({parameter,
                                           std::string(document::kTextParameterSchemaKey),
                                           document::ConstantValueSource{text}}))
        throw std::logic_error("legacy fixture text parameter");
    node->typeId = std::string(document::kTextSourceNodeType);
    node->schemaVersion = document::kTextSourceNodeSchemaVersion;
    node->parameters = {{std::string(document::kTextParameterRole), parameter}};
    if (!document.commit(before.revision(), std::move(draft)).committed())
        throw std::logic_error("legacy fixture publication");
    stack.clear();
    session.rebind(document, stack, compositionId);
    session.selectLayer(layer);
}
} // namespace bloom::ui::test
