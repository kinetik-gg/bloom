#include <bloom/commands/node_operations.hpp>

#include "node_operation_support.hpp"

#include <bloom/document/persisted_text.hpp>

namespace bloom::commands {
std::string_view AddNode::typeId() const noexcept { return "bloom.node.add"; }
OperationResult AddNode::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition)
        return detail::invalidTarget();
    if (!detail::finite(layoutPosition_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Node position must be finite");
    if (!registry_.isFrozen())
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Node authoring requires a frozen definition registry");
    const document::NodeDefinition* definition = nullptr;
    for (const auto& candidate : registry_.definitions()) {
        if (candidate.key.typeId == nodeTypeId_ &&
            (!definition || candidate.key.schemaVersion > definition->key.schemaVersion))
            definition = &candidate;
    }
    if (!definition)
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Node type is not registered: " + nodeTypeId_);
    const auto nodeId = draft.ids().allocateNode();
    if (!nodeId)
        return detail::exhaustedIds();
    document::NodeRecord node{*nodeId, definition->key.typeId, {}, definition->key.schemaVersion};
    std::vector<OperationOutput> outputs{{std::string(kAddNodeOutput), *nodeId}};
    for (const auto& parameter : definition->parameters) {
        const auto parameterId = draft.ids().allocateParameter();
        if (!parameterId)
            return detail::exhaustedIds();
        if (!composition->parameters().insert(
                {*parameterId, parameter.schemaKey,
                 document::ConstantValueSource{parameter.defaultValue}}))
            return OperationResult::rejected(
                OperationIssueCode::InvalidValue,
                "Node definition default violates its parameter schema");
        node.parameters.push_back({parameter.role, *parameterId});
        outputs.push_back({"parameter." + parameter.role, *parameterId});
    }
    if (!composition->graph().addNode(std::move(node)))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Node could not be inserted");
    composition->nodeLayout()[*nodeId] = {layoutPosition_, 128.0, false, false};
    return OperationResult::applied(std::move(outputs));
}

std::string_view RemoveNodes::typeId() const noexcept { return "bloom.node.remove"; }
OperationResult RemoveNodes::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition)
        return detail::invalidTarget();
    if (nodes_.empty())
        return OperationResult::noChange();
    std::set<document::ParameterId> candidates;
    for (const auto id : nodes_) {
        const auto* node = composition->graph().findNode(id);
        if (!node)
            return detail::invalidTarget();
        if (detail::protectedNode(composition->graph(), id))
            return OperationResult::rejected(
                OperationIssueCode::Unsupported,
                "The Layer Stack and composition output cannot be removed");
        for (const auto& binding : node->parameters)
            candidates.insert(binding.parameterId);
    }
    for (const auto id : nodes_) {
        (void)composition->graph().eraseNode(id);
        composition->nodeLayout().erase(id);
    }
    detail::eraseOrphanedParameters(*composition, candidates);
    return OperationResult::applied();
}

std::string_view RenameLayer::typeId() const noexcept { return "bloom.layer.rename"; }
OperationResult RenameLayer::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition)
        return detail::invalidTarget();
    if (!document::isValidHumanFacingName(name_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Layer name must be valid nonempty UTF-8");
    for (const auto& layer : composition->graph().layerOutputs()) {
        if (layer.layerId != layerId_)
            continue;
        if (layer.name == name_)
            return OperationResult::noChange();
        (void)composition->graph().renameLayer(layerId_, name_);
        return OperationResult::applied();
    }
    return detail::invalidTarget();
}

std::string_view ConnectPorts::typeId() const noexcept { return "bloom.node.connect"; }
OperationResult ConnectPorts::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition)
        return detail::invalidTarget();
    auto& graph = composition->graph();
    const auto sourceKind = graph.outputKind(source_, registry_);
    const auto inputKind = graph.inputKind(destination_, registry_);
    if (!sourceKind || !inputKind)
        return OperationResult::rejected(
            OperationIssueCode::InvalidTarget,
            "Connection requires existing registered source and destination ports");
    if (sourceKind != inputKind)
        return OperationResult::rejected(OperationIssueCode::SocketKindMismatch,
                                         "Connected socket kinds do not match");
    const auto* previous = detail::inputEdge(graph, destination_);
    if (previous && previous->source == source_)
        return OperationResult::noChange();
    const auto edgeId = previous ? std::optional(previous->id) : draft.ids().allocateEdge();
    if (!edgeId)
        return detail::exhaustedIds();
    if (previous)
        (void)graph.eraseEdge(*edgeId);
    if (!graph.addEdge({*edgeId, source_, destination_}, registry_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Connection could not be inserted");
    if (const auto failure = detail::validateGraph(*composition, registry_))
        return *failure;
    return OperationResult::applied({{"edge", *edgeId}});
}

std::string_view DisconnectInput::typeId() const noexcept { return "bloom.node.disconnect-input"; }
OperationResult DisconnectInput::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition)
        return detail::invalidTarget();
    auto& graph = composition->graph();
    if (!graph.inputKind(input_, registry_))
        return detail::invalidTarget();
    if (const auto* slot = std::get_if<document::LayerStackInputRef>(&input_);
        slot && (slot->stackNodeId != graph.layerStack().nodeId() ||
                 !graph.layerStack().find(slot->slotId)))
        return detail::invalidTarget();
    const auto* edge = detail::inputEdge(graph, input_);
    if (!edge)
        return OperationResult::noChange();
    (void)graph.eraseEdge(edge->id);
    if (const auto failure = detail::validateGraph(*composition, registry_))
        return *failure;
    return OperationResult::applied();
}

std::string_view DissolveNode::typeId() const noexcept { return "bloom.node.dissolve"; }
OperationResult DissolveNode::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition)
        return detail::invalidTarget();
    auto& graph = composition->graph();
    const auto* node = graph.findNode(nodeId_);
    if (!node)
        return detail::invalidTarget();
    if (detail::protectedNode(graph, nodeId_))
        return OperationResult::rejected(
            OperationIssueCode::Unsupported,
            "The Layer Stack and composition output cannot be dissolved");
    const auto* definition = registry_.find(node->typeId, node->schemaVersion);
    if (!definition)
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Node has no registered socket schema");
    const auto input = std::ranges::find(definition->inputs, document::SocketValueKind::Image,
                                         &document::InputPortDefinition::valueKind);
    const auto output = std::ranges::find(definition->outputs, document::SocketValueKind::Image,
                                          &document::OutputPortDefinition::valueKind);
    if (input == definition->inputs.end() || output == definition->outputs.end())
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Dissolve requires an Image input and Image output pair");
    const auto* incoming = detail::inputEdge(graph, document::NodeInputRef{nodeId_, input->name});
    if (!incoming)
        return OperationResult::rejected(OperationIssueCode::MissingReference,
                                         "Dissolve requires a connected first Image input");
    const auto source = incoming->source;
    std::vector<document::EdgeRecord> consumers;
    for (auto edge : graph.edges()) {
        if (edge.source.nodeId != nodeId_ || edge.source.port != output->name)
            continue;
        if (std::holds_alternative<document::LayerStackInputRef>(edge.destination))
            return OperationResult::rejected(
                OperationIssueCode::Unsupported,
                "Dissolving a participating Layer Output would break its required stack boundary; "
                "remove the layer instead");
        edge.source = source;
        consumers.push_back(std::move(edge));
    }
    std::set<document::ParameterId> candidates;
    for (const auto& binding : node->parameters)
        candidates.insert(binding.parameterId);
    (void)graph.eraseNode(nodeId_);
    composition->nodeLayout().erase(nodeId_);
    for (const auto& edge : consumers) {
        if (!graph.addEdge(edge, registry_))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Dissolve could not reconnect consumers");
    }
    detail::eraseOrphanedParameters(*composition, candidates);
    if (const auto failure = detail::validateGraph(*composition, registry_))
        return *failure;
    return OperationResult::applied();
}
} // namespace bloom::commands
