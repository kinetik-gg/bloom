#include "node_editor_add.hpp"
#include <algorithm>
#include <array>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <limits>
#include <type_traits>
#include <utility>

namespace bloom::ui::node_editor {
namespace {
document::Vec2d nodePosition(const document::Composition& composition, const document::NodeId id) {
    if (const auto found = composition.nodeLayout().find(id);
        found != composition.nodeLayout().end())
        return found->second.position;
    return document::defaultNodeLayout(composition.graph().nodes()).at(id).position;
}

void placeConnectedNode(document::Composition& composition, const document::NodeId node,
                        const std::optional<document::InputPortRef>& input,
                        const std::optional<document::OutputPortRef>& output,
                        std::optional<document::NodeId> pendingSource = {}) {
    const auto connected = input
                               ? std::optional(std::visit(
                                     [](const auto& port) {
                                         if constexpr (std::is_same_v<std::decay_t<decltype(port)>,
                                                                      document::NodeInputRef>)
                                             return port.nodeId;
                                         else
                                             return port.stackNodeId;
                                     },
                                     *input))
                           : output ? std::optional(output->nodeId)
                                    : std::nullopt;
    if (!connected)
        return;
    const auto anchorPosition = nodePosition(composition, *connected);
    const auto gap = document::kNodeLayoutSpacing;
    const auto& size = document::kConservativeNodeCardSize;
    const auto preferred =
        input ? document::Vec2d{anchorPosition.x - size.width - gap, anchorPosition.y}
              : document::Vec2d{anchorPosition.x + size.width + gap, anchorPosition.y};
    auto occupied = composition.nodeLayout();
    const auto defaults = document::defaultNodeLayout(composition.graph().nodes());
    for (const auto& existing : composition.graph().nodes())
        if (existing.id != node && !occupied.contains(existing.id))
            occupied.emplace(existing.id, defaults.at(existing.id));
    occupied.erase(node);
    if (pendingSource)
        occupied.erase(*pendingSource);
    const auto placement =
        document::findNearestFreeNodePosition(occupied, {}, size, preferred, gap);
    auto record = composition.nodeLayout().at(node);
    record.position = placement;
    composition.nodeLayout()[node] = record;
}
} // namespace

commands::OperationResult AddCompositionSource::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    const auto* nested = draft.project().findComposition(source_);
    if (!composition || !nested ||
        source_.value() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return commands::OperationResult::rejected(commands::OperationIssueCode::InvalidTarget,
                                                   "Source composition does not exist");
    const auto name = nested->name();
    const auto duration = std::min(nested->duration(), composition->duration());
    auto added = commands::AddNode(composition_, std::string(document::kCompositionSourceNodeType),
                                   position_)
                     .apply(draft);
    if (added.status != commands::OperationStatus::Applied)
        return added;
    document::NodeId sourceNode;
    for (const auto& output : added.outputs) {
        if (output.name == commands::kAddNodeOutput)
            sourceNode = std::get<document::NodeId>(output.id);
        if (output.name == "parameter.composition") {
            const auto result =
                commands::SetParameterSource(
                    composition_, std::get<document::ParameterId>(output.id),
                    document::ConstantValueSource{static_cast<std::int64_t>(source_.value())})
                    .apply(draft);
            if (result.status == commands::OperationStatus::Rejected)
                return result;
        }
    }
    const auto nesting = draft.project().validateCompositionNesting();
    if (!nesting.ok())
        return commands::OperationResult::rejected(commands::OperationIssueCode::InvalidValue,
                                                   nesting.issues().front().message);
    if (!asLayer_)
        return added;
    const auto merge = composition->graph().outputMergeId();
    if (!merge)
        return commands::OperationResult::rejected(
            commands::OperationIssueCode::InvalidTarget,
            "Timeline layers need a Merge connected to Output");
    auto layerResult = AddEditorNode(composition_, std::string(document::kLayerOutputNodeType), {},
                                     document::LayerStackInputRef{*merge, {}, "content"})
                           .apply(draft);
    if (layerResult.status == commands::OperationStatus::Rejected)
        return layerResult;
    document::NodeId layerNode;
    document::LayerId layerId;
    for (const auto& output : layerResult.outputs) {
        if (output.name == commands::kAddNodeOutput)
            layerNode = std::get<document::NodeId>(output.id);
        if (output.name == "layer")
            layerId = std::get<document::LayerId>(output.id);
    }
    for (const auto* port : {"image", "audio"}) {
        auto result = commands::ConnectPorts(composition_, {sourceNode, port},
                                             document::NodeInputRef{layerNode, port})
                          .apply(draft);
        if (result.status == commands::OperationStatus::Rejected)
            return result;
    }
    auto audio = commands::ConnectPorts(composition_, {layerNode, "audio"},
                                        document::LayerStackInputRef{*merge, {}, "audio"})
                     .apply(draft);
    if (audio.status == commands::OperationStatus::Rejected)
        return audio;
    if (const auto& output = composition->graph().compositionOutput(); output) {
        auto result = commands::ConnectPorts(composition_, {*merge, "audio"},
                                             document::NodeInputRef{output->nodeId, "audio"})
                          .apply(draft);
        if (result.status == commands::OperationStatus::Rejected)
            return result;
    }
    auto renamed = commands::RenameLayer(composition_, layerId, name).apply(draft);
    if (renamed.status == commands::OperationStatus::Rejected)
        return renamed;
    auto range = commands::SetLayerRange(composition_, layerId, {}, duration).apply(draft);
    if (range.status == commands::OperationStatus::Rejected)
        return range;
    placeConnectedNode(*composition, layerNode, document::LayerStackInputRef{*merge, {}, "content"},
                       {}, sourceNode);
    placeConnectedNode(*composition, sourceNode, document::NodeInputRef{layerNode, "image"}, {});
    added.outputs.push_back({"layer", layerId});
    return added;
}

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
    placeConnectedNode(*draft.project().findComposition(composition_), *node, input_, output_);
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
        if (definition->layerSlotInput && kind == definition->layerSlotInput->valueKind) {
            const auto connected =
                commands::ConnectPorts(
                    composition_, *output_,
                    document::LayerStackInputRef{*node, {}, definition->layerSlotInput->role})
                    .apply(draft);
            return connected.status == commands::OperationStatus::Rejected ? connected : result;
        }
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

namespace bloom::ui::node_editor {
commands::OperationResult InsertReroute::apply(document::Draft& draft) const {
    const auto* composition = draft.project().findComposition(composition_);
    if (composition == nullptr)
        return commands::OperationResult::rejected(commands::OperationIssueCode::InvalidTarget,
                                                   "No active composition");
    const auto edges = composition->graph().edges();
    const auto link = std::ranges::find(edges, destination_, &document::EdgeRecord::destination);
    if (link == edges.end())
        return commands::OperationResult::rejected(commands::OperationIssueCode::MissingReference,
                                                   "That link no longer exists");
    const auto source = link->source;
    auto added = commands::AddNode(composition_, std::string(document::kRerouteNodeType), position_)
                     .apply(draft);
    if (added.status == commands::OperationStatus::Rejected)
        return added;
    std::optional<document::NodeId> reroute;
    for (const auto& item : added.outputs)
        if (const auto* id = std::get_if<document::NodeId>(&item.id);
            id != nullptr && item.name == commands::kAddNodeOutput)
            reroute = *id;
    if (!reroute)
        return commands::OperationResult::rejected(commands::OperationIssueCode::InvalidTarget,
                                                   "Reroute returned no node identity");
    // Upstream FIRST: the reroute takes its kind from whatever feeds it, so feeding it before it is
    // asked to satisfy the destination's kind is what lets the second connection be checked at all.
    auto upstream = commands::ConnectPorts(
                        composition_, source,
                        document::NodeInputRef{*reroute, std::string(document::kValuePortName)})
                        .apply(draft);
    if (upstream.status == commands::OperationStatus::Rejected)
        return upstream;
    auto downstream =
        commands::ConnectPorts(
            composition_, document::OutputPortRef{*reroute, std::string(document::kValuePortName)},
            destination_)
            .apply(draft);
    if (downstream.status == commands::OperationStatus::Rejected)
        return downstream;
    auto result = std::move(added);
    result.outputs.push_back({"editorNode", *reroute});
    return result;
}
} // namespace bloom::ui::node_editor
