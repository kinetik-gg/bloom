#include "node_editor_add.hpp"
#include <algorithm>
#include <array>

namespace bloom::ui::node_editor {
commands::OperationResult AddEditorNode::apply(document::Draft& draft) const {
    const auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return commands::OperationResult::rejected(commands::OperationIssueCode::InvalidTarget,
                                                   "No active composition");
    commands::OperationResult added;
    std::string_view nodeOutput = commands::kAddNodeOutput;
    if (type_ == document::kTextSourceNodeType)
        return commands::AddTextLayer(composition_, "Text", "Text", {}).apply(draft);
    if (type_ == document::kSolidSourceNodeType) {
        constexpr std::array palette{
            core::Color4d{0.62, 0.08, 0.04, 1}, core::Color4d{0.04, 0.20, 0.72, 1},
            core::Color4d{0.06, 0.52, 0.16, 1}, core::Color4d{0.46, 0.07, 0.58, 1}};
        const auto number = composition->graph().layerOutputs().size();
        added = commands::AddSolidLayer(
                    composition_, "Solid " + std::to_string(number + 1),
                    palette[number % palette.size()],
                    {composition->format().width() * 0.5, composition->format().height() * 0.5})
                    .apply(draft);
        nodeOutput = commands::kAddSolidLayerSolidNodeOutput;
    } else {
        added = commands::AddNode(composition_, type_, position_).apply(draft);
    }
    if (added.status == commands::OperationStatus::Rejected)
        return added;
    std::optional<document::NodeId> node;
    std::map<document::NodeId, document::Vec2d> positions;
    for (const auto& item : added.outputs) {
        const auto* id = std::get_if<document::NodeId>(&item.id);
        if (!id)
            continue;
        if (item.name == nodeOutput) {
            node = *id;
            positions.emplace(*id, position_);
        }
        if (type_ == document::kSolidSourceNodeType &&
            item.name == commands::kAddSolidLayerLayerOutputNodeOutput)
            positions.emplace(*id, document::Vec2d{position_.x + 256, position_.y});
    }
    if (!node)
        return commands::OperationResult::rejected(commands::OperationIssueCode::InvalidTarget,
                                                   "Node command returned no node identity");
    auto moved = commands::MoveNodes(composition_, positions).apply(draft);
    if (moved.status == commands::OperationStatus::Rejected)
        return moved;
    added.outputs.push_back({"editorNode", *node});
    const auto* record = draft.project().findComposition(composition_)->graph().findNode(*node);
    const auto* definition =
        document::builtInNodeDefinitions().find(record->typeId, record->schemaVersion);
    if (!definition)
        return added;
    const auto& graph = draft.project().findComposition(composition_)->graph();
    if (input_) {
        const auto kind = graph.inputKind(*input_);
        for (const auto& port : definition->outputs) {
            if (port.valueKind != kind)
                continue;
            const auto connected =
                commands::ConnectPorts(composition_, {*node, port.name}, *input_).apply(draft);
            return connected.status == commands::OperationStatus::Rejected ? connected : added;
        }
    }
    if (output_) {
        const auto kind = graph.outputKind(*output_);
        for (const auto& port : definition->inputs) {
            if (port.valueKind != kind)
                continue;
            const auto connected = commands::ConnectPorts(composition_, *output_,
                                                          document::NodeInputRef{*node, port.name})
                                       .apply(draft);
            return connected.status == commands::OperationStatus::Rejected ? connected : added;
        }
    }
    if (input_ || output_)
        return commands::OperationResult::rejected(commands::OperationIssueCode::Unsupported,
                                                   "Node has no compatible socket");
    return added;
}
} // namespace bloom::ui::node_editor
