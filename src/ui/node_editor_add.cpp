#include "node_editor_add.hpp"
#include <algorithm>
#include <array>
#include <utility>

namespace bloom::ui::node_editor {
commands::OperationResult AddEditorNode::apply(document::Draft& draft) const {
    const auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return commands::OperationResult::rejected(commands::OperationIssueCode::InvalidTarget,
                                                   "No active composition");
    // Task FIX1, item B: adding a source from the NODE CANVAS creates exactly that node and nothing
    // else. Solid and Text used to take a structured-layer branch here, so asking for a Text node
    // produced a Text source, a Layer Output, a stack slot and two edges -- a decision the artist
    // had not made. Wiring is now theirs: source output into a Layer node's image input, Layer
    // output into Merge, which is what creates the stack slot. The TIMELINE's Add menu still builds
    // a whole layer, because a layer is what it is about.
    auto added = commands::AddNode(composition_, type_, position_).apply(draft);
    constexpr std::string_view nodeOutput = commands::kAddNodeOutput;
    if (added.status == commands::OperationStatus::Rejected)
        return added;
    std::optional<document::NodeId> node;
    for (const auto& item : added.outputs) {
        const auto* id = std::get_if<document::NodeId>(&item.id);
        if (id != nullptr && item.name == nodeOutput)
            node = *id;
    }
    if (!node)
        return commands::OperationResult::rejected(commands::OperationIssueCode::InvalidTarget,
                                                   "Node command returned no node identity");
    auto result = std::move(added);
    result.outputs.push_back({"editorNode", *node});
    const auto* record = draft.project().findComposition(composition_)->graph().findNode(*node);
    const auto* definition =
        document::builtInNodeDefinitions().find(record->typeId, record->schemaVersion);
    if (!definition)
        return result;
    const auto& graph = draft.project().findComposition(composition_)->graph();
    if (input_) {
        const auto kind = graph.inputKind(*input_);
        for (const auto& port : definition->outputs) {
            if (port.valueKind != kind)
                continue;
            const auto connected =
                commands::ConnectPorts(composition_, {*node, port.name}, *input_).apply(draft);
            return connected.status == commands::OperationStatus::Rejected ? connected : result;
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
            return connected.status == commands::OperationStatus::Rejected ? connected : result;
        }
    }
    if (input_ || output_)
        return commands::OperationResult::rejected(commands::OperationIssueCode::Unsupported,
                                                   "Node has no compatible socket");
    return result;
}
} // namespace bloom::ui::node_editor
