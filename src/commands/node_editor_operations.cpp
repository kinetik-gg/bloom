#include <algorithm>
#include <array>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/node_editor_operations.hpp>
#include <bloom/commands/node_layout_placement.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/project.hpp>
#include <limits>
#include <type_traits>
#include <utility>

namespace bloom::commands {

OperationResult
checkAddEditorNodeEligibility(const document::Snapshot& snapshot,
                              const document::CompositionId compositionId,
                              const std::string_view type, const document::Vec2d position,
                              const std::optional<document::InputPortRef>& input,
                              const std::optional<document::OutputPortRef>& output) {
    const auto* composition = snapshot.project().findComposition(compositionId);
    if (!composition)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "No active composition");
    auto eligibility =
        checkAddNodeEligibility(snapshot.project(), snapshot.ids(), compositionId, type, position);
    if (eligibility.status == OperationStatus::Rejected)
        return eligibility;
    if (!input && !output)
        return eligibility;
    const document::NodeDefinition* definition = nullptr;
    for (const auto& candidate : document::builtInNodeDefinitions().definitions())
        if (candidate.key.typeId == type &&
            (!definition || candidate.key.schemaVersion > definition->key.schemaVersion))
            definition = &candidate;
    if (!definition)
        return eligibility;
    const auto& graph = composition->graph();
    auto available = snapshot.ids();
    const auto checkConnectionIds = [&](const bool needsSlot, const bool needsEdge) {
        if ((needsSlot && !available.allocateLayerSlot()) ||
            (needsEdge && !available.allocateEdge()))
            return OperationResult::rejected(OperationIssueCode::Unsupported,
                                             "Document ID space is exhausted");
        return eligibility;
    };
    if (input) {
        if (const auto kind = graph.inputKind(*input); kind)
            for (const auto& port : definition->outputs)
                if (port.valueKind == *kind) {
                    const auto* fixed = std::get_if<document::NodeInputRef>(&*input);
                    const auto* target = fixed ? graph.findNode(fixed->nodeId) : nullptr;
                    const bool parameter =
                        fixed && target &&
                        std::ranges::any_of(target->parameters, [&](const auto& binding) {
                            return binding.role == fixed->port;
                        });
                    const auto* slot = std::get_if<document::LayerStackInputRef>(&*input);
                    const bool freshSlot = slot && !slot->slotId.isValid();
                    const bool replacingEdge =
                        std::ranges::any_of(graph.edges(), [&](const auto& edge) {
                            return edge.destination == *input;
                        });
                    return checkConnectionIds(freshSlot, !parameter && !replacingEdge);
                }
    }
    if (output) {
        if (const auto kind = graph.outputKind(*output); kind) {
            if (definition->layerSlotInput && *kind == definition->layerSlotInput->valueKind)
                return checkConnectionIds(true, true);
            for (const auto& port : definition->inputs)
                if (*kind == port.valueKind)
                    return checkConnectionIds(
                        false,
                        std::ranges::none_of(definition->parameters, [&](const auto& parameter) {
                            return parameter.role == port.name;
                        }));
        }
    }
    return OperationResult::rejected(OperationIssueCode::Unsupported,
                                     "Node has no compatible socket");
}

OperationResult AddCompositionSource::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    const auto* nested = draft.project().findComposition(source_);
    if (!composition || !nested ||
        source_.value() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Source composition does not exist");
    const auto name = nested->name();
    const auto duration = std::min(nested->duration(), composition->duration());
    auto added = AddNode(composition_, std::string(document::kCompositionSourceNodeType), position_)
                     .apply(draft);
    if (added.status != OperationStatus::Applied)
        return added;
    document::NodeId sourceNode;
    for (const auto& output : added.outputs) {
        if (output.name == kAddNodeOutput)
            sourceNode = std::get<document::NodeId>(output.id);
        if (output.name == "parameter.composition") {
            const auto result =
                SetParameterSource(
                    composition_, std::get<document::ParameterId>(output.id),
                    document::ConstantValueSource{static_cast<std::int64_t>(source_.value())})
                    .apply(draft);
            if (result.status == OperationStatus::Rejected)
                return result;
        }
    }
    const auto nesting = draft.project().validateCompositionNesting();
    if (!nesting.ok())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         nesting.issues().front().message);
    if (!asLayer_)
        return added;
    const auto merge = composition->graph().outputMergeId();
    if (!merge)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Timeline layers need a Merge connected to Output");
    auto layerResult = AddEditorNode(composition_, std::string(document::kLayerOutputNodeType), {},
                                     document::LayerStackInputRef{*merge, {}, "content"})
                           .apply(draft);
    if (layerResult.status == OperationStatus::Rejected)
        return layerResult;
    document::NodeId layerNode;
    document::LayerId layerId;
    for (const auto& output : layerResult.outputs) {
        if (output.name == kAddNodeOutput)
            layerNode = std::get<document::NodeId>(output.id);
        if (output.name == "layer")
            layerId = std::get<document::LayerId>(output.id);
    }
    for (const auto* port : {"image", "audio"}) {
        auto result =
            ConnectPorts(composition_, {sourceNode, port}, document::NodeInputRef{layerNode, port})
                .apply(draft);
        if (result.status == OperationStatus::Rejected)
            return result;
    }
    auto audio = ConnectPorts(composition_, {layerNode, "audio"},
                              document::LayerStackInputRef{*merge, {}, "audio"})
                     .apply(draft);
    if (audio.status == OperationStatus::Rejected)
        return audio;
    if (const auto& output = composition->graph().compositionOutput(); output) {
        auto result = ConnectPorts(composition_, {*merge, "audio"},
                                   document::NodeInputRef{output->nodeId, "audio"})
                          .apply(draft);
        if (result.status == OperationStatus::Rejected)
            return result;
    }
    auto renamed = RenameLayer(composition_, layerId, name).apply(draft);
    if (renamed.status == OperationStatus::Rejected)
        return renamed;
    // The nested layer shows its composition's name and follows it (document 1.30).
    if (auto* target = draft.project().findComposition(composition_))
        if (auto* layer = target->graph().findLayer(layerId))
            layer->nameFollowsSource = true;
    auto range = SetLayerRange(composition_, layerId, {}, duration).apply(draft);
    if (range.status == OperationStatus::Rejected)
        return range;
    // ADR 0024: the nested duration is the layer's content extent; after it, nothing shows.
    auto extent =
        SetLayerContentExtent(composition_, layerId, document::SourceLayerExtent{sourceNode})
            .apply(draft);
    if (extent.status == OperationStatus::Rejected)
        return extent;
    placeLayerCards(*composition, layerNode, sourceNode);
    added.outputs.push_back({"layer", layerId});
    return added;
}

OperationResult AddEditorNode::apply(document::Draft& draft) const {
    const auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "No active composition");
    // Task FIX1, item B: adding a source from the NODE CANVAS creates exactly that node and nothing
    // else. Solid and Text used to take a structured-layer branch here, so asking for a Text node
    // produced a Text source, a Layer Output, a stack slot and two edges -- a decision the artist
    // had not made. Wiring is now theirs: source output into a Layer node's image input, Layer
    // output into Merge, which is what creates the stack slot. The TIMELINE's Add menu still builds
    // a whole layer, because a layer is what it is about.
    // Task N1, item 3: the card lands at the cursor. AddNode keeps that point whenever it is clear
    // of the cards the editor measured, and otherwise moves it just past its neighbour; a link
    // dragged out of a socket no longer re-anchors the new card beside the node it came from.
    AddNode add(composition_, type_, position_);
    add.setMeasuredSizes(sizes_);
    auto added = add.apply(draft);
    constexpr std::string_view nodeOutput = kAddNodeOutput;
    if (added.status == OperationStatus::Rejected)
        return added;
    std::optional<document::NodeId> node;
    for (const auto& item : added.outputs) {
        const auto* id = std::get_if<document::NodeId>(&item.id);
        if (id != nullptr && item.name == nodeOutput)
            node = *id;
    }
    if (!node)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
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
                ConnectPorts(composition_, {*node, port.name}, *input_).apply(draft);
            return connected.status == OperationStatus::Rejected ? connected : result;
        }
    }
    if (output_) {
        const auto kind = graph.outputKind(*output_);
        if (definition->layerSlotInput && kind == definition->layerSlotInput->valueKind) {
            const auto connected = ConnectPorts(composition_, *output_,
                                                document::LayerStackInputRef{
                                                    *node, {}, definition->layerSlotInput->role})
                                       .apply(draft);
            return connected.status == OperationStatus::Rejected ? connected : result;
        }
        for (const auto& port : definition->inputs) {
            if (port.valueKind != kind)
                continue;
            const auto connected =
                ConnectPorts(composition_, *output_, document::NodeInputRef{*node, port.name})
                    .apply(draft);
            return connected.status == OperationStatus::Rejected ? connected : result;
        }
    }
    if (input_ || output_)
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Node has no compatible socket");
    return result;
}
} // namespace bloom::commands

namespace bloom::commands {
OperationResult InsertReroute::apply(document::Draft& draft) const {
    const auto* composition = draft.project().findComposition(composition_);
    if (composition == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "No active composition");
    const auto edges = composition->graph().edges();
    const auto link = std::ranges::find(edges, destination_, &document::EdgeRecord::destination);
    if (link == edges.end())
        return OperationResult::rejected(OperationIssueCode::MissingReference,
                                         "That link no longer exists");
    const auto source = link->source;
    auto added =
        AddNode(composition_, std::string(document::kRerouteNodeType), position_).apply(draft);
    if (added.status == OperationStatus::Rejected)
        return added;
    std::optional<document::NodeId> reroute;
    for (const auto& item : added.outputs)
        if (const auto* id = std::get_if<document::NodeId>(&item.id);
            id != nullptr && item.name == kAddNodeOutput)
            reroute = *id;
    if (!reroute)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Reroute returned no node identity");
    // Upstream FIRST: the reroute takes its kind from whatever feeds it, so feeding it before it is
    // asked to satisfy the destination's kind is what lets the second connection be checked at all.
    auto upstream =
        ConnectPorts(composition_, source,
                     document::NodeInputRef{*reroute, std::string(document::kValuePortName)})
            .apply(draft);
    if (upstream.status == OperationStatus::Rejected)
        return upstream;
    auto downstream =
        ConnectPorts(composition_,
                     document::OutputPortRef{*reroute, std::string(document::kValuePortName)},
                     destination_)
            .apply(draft);
    if (downstream.status == OperationStatus::Rejected)
        return downstream;
    auto result = std::move(added);
    result.outputs.push_back({"editorNode", *reroute});
    return result;
}
OperationResult AddImageNode::apply(document::Draft& draft) const {
    if (!draft.project().findAsset(asset_))
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Image asset does not exist");
    const auto* asset = draft.project().findAsset(asset_);
    auto added = AddNode(composition_,
                         asset && asset->kind == document::AssetKind::Video ? "bloom.video-source"
                                                                            : "bloom.image-source",
                         position_)
                     .apply(draft);
    if (added.status != OperationStatus::Applied)
        return added;
    for (const auto& output : added.outputs)
        if (output.name == kAddNodeOutput) {
            const auto* id = std::get_if<document::NodeId>(&output.id);
            const auto* composition = draft.project().findComposition(composition_);
            const auto* node = id && composition ? composition->graph().findNode(*id) : nullptr;
            if (!node)
                break;
            // The new node inherits the asset's probed alpha association (see AddImageLayer).
            for (const auto& binding : node->parameters)
                if (binding.role == "premultiply") {
                    auto result =
                        SetParameterSource(composition_, binding.parameterId,
                                           document::ConstantValueSource{
                                               asset->interpretation.alphaAssociation !=
                                               document::AssetAlphaAssociation::Premultiplied})
                            .apply(draft);
                    if (result.status == OperationStatus::Rejected)
                        return result;
                }
            for (const auto& binding : node->parameters)
                if (binding.role == "asset") {
                    auto result = SetParameterSource(
                                      composition_, binding.parameterId,
                                      document::ConstantValueSource{std::to_string(asset_.value())})
                                      .apply(draft);
                    if (result.status == OperationStatus::Rejected)
                        return result;
                    return added;
                }
        }
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Image node asset parameter is missing");
}

OperationResult AddAudioNode::apply(document::Draft& draft) const {
    const auto* asset = draft.project().findAsset(asset_);
    if (asset == nullptr || asset->kind != document::AssetKind::Audio)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Audio asset does not exist");
    auto added = AddNode(composition_, "bloom.audio-source", position_).apply(draft);
    if (added.status != OperationStatus::Applied)
        return added;
    for (const auto& output : added.outputs)
        if (output.name == kAddNodeOutput) {
            const auto* id = std::get_if<document::NodeId>(&output.id);
            const auto* composition = draft.project().findComposition(composition_);
            const auto* node = id && composition ? composition->graph().findNode(*id) : nullptr;
            if (!node)
                break;
            for (const auto& binding : node->parameters)
                if (binding.role == "asset") {
                    auto result = SetParameterSource(
                                      composition_, binding.parameterId,
                                      document::ConstantValueSource{std::to_string(asset_.value())})
                                      .apply(draft);
                    if (result.status == OperationStatus::Rejected)
                        return result;
                    return added;
                }
        }
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Audio node asset parameter is missing");
}

OperationResult AddAssetSourceNode::apply(document::Draft& draft) const {
    const auto* asset = draft.project().findAsset(asset_);
    if (asset == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "The asset does not exist");
    switch (asset->kind) {
    case document::AssetKind::Audio:
        return AddAudioNode(composition_, asset_, position_).apply(draft);
    case document::AssetKind::Image:
    case document::AssetKind::Sequence:
    case document::AssetKind::Video:
        return AddImageNode(composition_, asset_, position_).apply(draft);
    case document::AssetKind::Font:
    case document::AssetKind::Lut:
        break;
    }
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Only image, sequence, video and audio assets make a "
                                     "source node");
}
} // namespace bloom::commands
