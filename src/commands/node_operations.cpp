#include <bloom/commands/node_operations.hpp>

#include "node_operation_support.hpp"

#include <bloom/document/persisted_text.hpp>

#include <algorithm>
#include <string>
#include <variant>

namespace bloom::commands {
bool canApplyNodeOperation(const document::Snapshot& snapshot, const Operation& operation) {
    document::Document isolated(snapshot.project(), snapshot.ids().highWater());
    auto draft = isolated.draft(isolated.snapshot());
    return operation.apply(draft).status != OperationStatus::Rejected && draft.validate().ok();
}

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
    // Cardinality (task S1, item 5). Checked here rather than in each surface that offers an Add,
    // so the keyboard, the menu and the search popup cannot disagree about whether a second one is
    // allowed -- and so the search popup can read the refusal back from a dry run of this very
    // operation instead of keeping its own copy of the rule.
    if (definition->cardinality == document::NodeCardinality::OnePerComposition &&
        std::ranges::any_of(composition->graph().nodes(), [this](const auto& existing) {
            return existing.typeId == nodeTypeId_;
        }))
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Only one " + nodeTypeId_ +
                                             " node is allowed per composition");
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
    // A Layer node gets its layer IDENTITY here, at creation, rather than on its first connection
    // to Merge (task FIX1, item B). The alternative -- waiting for the Merge link -- would leave a
    // card on the canvas with no name to rename, no LayerId for Properties and the Timeline to
    // address, and two different shapes of Layer node to reason about. The TIMELINE still lists a
    // layer only once it has a stack slot, which is the thing that actually makes it draw, so
    // "added" and "participating" stay distinguishable without a second node shape.
    if (nodeTypeId_ == document::kLayerOutputNodeType) {
        const auto layerId = draft.ids().allocateLayer();
        if (!layerId)
            return detail::exhaustedIds();
        const auto number = composition->graph().layerOutputs().size() + 1;
        if (!composition->graph().addLayerOutput({*nodeId, *layerId,
                                                  "Layer " + std::to_string(number),
                                                  std::string(document::kLayerOutputOutputPort)}))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Layer boundary could not be inserted");
        outputs.push_back({"layer", *layerId});
    }
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
    // A removed node leaves its frame, and a frame with nothing left in it goes with it.
    (void)detail::detachFromNodeGroups(*composition, nodes_);
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
    // An unconnected REROUTE has no kind yet, and that is not a missing port -- it is the whole of
    // what "a reroute takes the kind of the link it joins" means (task FIX1, item I). The first
    // link into one is accepted whatever it carries; every link after it is checked against what
    // the reroute now holds, because by then outputKind()/inputKind() answer with that kind.
    const auto kindless = [&graph](const document::NodeId nodeId) {
        const auto* node = graph.findNode(nodeId);
        return node != nullptr && document::isRerouteNodeType(node->typeId);
    };
    const bool sourceIsKindlessReroute = !sourceKind && kindless(source_.nodeId);
    const bool destinationIsKindlessReroute =
        !inputKind && kindless(detail::destinationNode(destination_));
    if ((!sourceKind && !sourceIsKindlessReroute) || (!inputKind && !destinationIsKindlessReroute))
        return OperationResult::rejected(
            OperationIssueCode::InvalidTarget,
            "Connection requires existing registered source and destination ports");
    if (sourceKind && inputKind && !document::isAcceptedSocketConnection(*sourceKind, *inputKind))
        return OperationResult::rejected(OperationIssueCode::SocketKindMismatch,
                                         "Connected socket kinds do not match");
    // Task S7: a link into an operand socket is the PARAMETER's driver binding, not an edge. One
    // authored value, one durable record of where it comes from -- so there is nothing for an edge
    // and a binding to disagree about, and DisconnectInput has one thing to undo.
    if (const auto* binding = detail::parameterSocketBinding(graph, destination_)) {
        const auto* parameter = composition->parameters().find(binding->parameterId);
        if (parameter == nullptr)
            return detail::invalidTarget();
        const document::DriverBindingSource driver{source_.nodeId, source_.port};
        if (std::holds_alternative<document::DriverBindingSource>(parameter->source) &&
            std::get<document::DriverBindingSource>(parameter->source) == driver)
            return OperationResult::noChange();
        if (!composition->parameters().setSource(binding->parameterId, driver))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Driver binding could not be applied");
        if (const auto failure = detail::validateGraph(*composition, registry_))
            return *failure;
        return OperationResult::applied();
    }
    // A link into Merge's ordered multi-input whose slot id is the invalid sentinel MEANS "make a
    // new slot here" (task FIX1, item B): the stack slot is what a Layer node's connection to Merge
    // IS, so creating it is part of connecting rather than a separate command the artist has to
    // find.
    if (const auto* slot = std::get_if<document::LayerStackInputRef>(&destination_);
        slot != nullptr && !slot->slotId.isValid()) {
        auto* stack = graph.merge(slot->stackNodeId);
        if (!stack)
            return detail::invalidTarget();
        const auto boundaries = graph.layerOutputs();
        const auto boundary = std::ranges::find_if(boundaries, [this](const auto& candidate) {
            return candidate.nodeId == source_.nodeId;
        });
        const auto layerId = boundary == boundaries.end() ? document::LayerId{} : boundary->layerId;
        const auto slotId = draft.ids().allocateLayerSlot();
        const auto edgeId = draft.ids().allocateEdge();
        if (!slotId || !edgeId)
            return detail::exhaustedIds();
        if (!stack->append({*slotId, layerId}))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Layer stack slot could not be inserted");
        if (insertBefore_.has_value() && !stack->moveBefore(*slotId, insertBefore_))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Layer stack slot could not be ordered");
        if (!graph.addEdge({*edgeId, source_,
                            document::LayerStackInputRef{slot->stackNodeId, *slotId, slot->role}},
                           registry_))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Layer stack connection could not be inserted");
        if (const auto failure = detail::validateGraph(*composition, registry_))
            return *failure;
        return OperationResult::applied(
            {{std::string(kConnectPortsSlotOutput), *slotId}, {"edge", *edgeId}});
    }
    const auto* previous = detail::inputEdge(graph, destination_);
    if (previous && previous->source == source_)
        return OperationResult::noChange();
    const auto edgeId = previous ? std::optional(previous->id) : draft.ids().allocateEdge();
    if (!edgeId)
        return detail::exhaustedIds();
    if (previous)
        (void)graph.eraseEdge(*edgeId);
    if (const auto* slot = std::get_if<document::LayerStackInputRef>(&destination_)) {
        auto* stack = graph.merge(slot->stackNodeId);
        if (!stack || !stack->find(slot->slotId))
            return detail::invalidTarget();
        const auto entries = stack->entries();
        const auto found =
            std::ranges::find(entries, slot->slotId, &document::LayerStackEntry::slotId);
        const auto next = std::next(found) == entries.end()
                              ? std::nullopt
                              : std::optional(std::next(found)->slotId);
        const auto boundaries = graph.layerOutputs();
        const auto boundary =
            std::ranges::find(boundaries, source_.nodeId, &document::LayerOutputBoundary::nodeId);
        const auto layerId = boundary == boundaries.end() ? document::LayerId{} : boundary->layerId;
        (void)stack->erase(slot->slotId);
        if (!stack->append({slot->slotId, layerId}) || !stack->moveBefore(slot->slotId, next))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Merge slot source cannot be replaced");
    }
    if (!graph.addEdge({*edgeId, source_, destination_}, registry_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Connection could not be inserted");
    if (const auto failure = detail::validateGraph(*composition, registry_))
        return *failure;
    return OperationResult::applied({{"edge", *edgeId}});
}

std::string_view SetMergeEnabled::typeId() const noexcept { return "bloom.merge.set-enabled"; }
OperationResult SetMergeEnabled::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    auto* merge = composition ? composition->graph().merge(mergeId_) : nullptr;
    if (!merge)
        return detail::invalidTarget();
    if (merge->enabled() == enabled_)
        return OperationResult::noChange();
    merge->setEnabled(enabled_);
    return OperationResult::applied();
}

std::string_view ReorderMergeInput::typeId() const noexcept { return "bloom.merge.reorder-input"; }
OperationResult ReorderMergeInput::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    auto* stack = composition ? composition->graph().merge(mergeId_) : nullptr;
    if (!stack || !stack->find(slotId_) || newIndex_ >= stack->entries().size())
        return detail::invalidTarget();
    const auto entries = stack->entries();
    const auto found = std::ranges::find(entries, slotId_, &document::LayerStackEntry::slotId);
    const auto oldIndex = static_cast<std::size_t>(std::distance(entries.begin(), found));
    if (oldIndex == newIndex_)
        return OperationResult::noChange();
    const auto beforeIndex = newIndex_ + (oldIndex < newIndex_ ? 1U : 0U);
    const auto before =
        beforeIndex == entries.size() ? std::nullopt : std::optional(entries[beforeIndex].slotId);
    if (!stack->moveBefore(slotId_, before))
        return detail::invalidTarget();
    return OperationResult::applied();
}

std::string_view DisconnectInput::typeId() const noexcept { return "bloom.node.disconnect-input"; }
OperationResult DisconnectInput::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition)
        return detail::invalidTarget();
    auto& graph = composition->graph();
    if (!graph.inputKind(input_, registry_))
        return detail::invalidTarget();
    if (const auto* slot = std::get_if<document::LayerStackInputRef>(&input_); slot != nullptr) {
        auto* stack = graph.merge(slot->stackNodeId);
        if (!stack || stack->find(slot->slotId) == nullptr)
            return detail::invalidTarget();
        // Detaching a stack slot's content REMOVES the slot (task FIX1, item B). A slot with
        // nothing in it is not a shape the canonical graph admits -- every visible slot requires
        // one typed content connection -- so "the slot" and "the link into it" are one thing to the
        // artist and one thing here. The Layer node keeps its boundary and its LayerId, so
        // reconnecting it is one gesture rather than a rebuild.
        const auto* edge = detail::inputEdge(graph, input_);
        if (edge != nullptr)
            (void)graph.eraseEdge(edge->id);
        if (!stack->erase(slot->slotId))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Layer stack slot could not be removed");
        if (const auto failure = detail::validateGraph(*composition, registry_))
            return *failure;
        return OperationResult::applied();
    }
    // Unlinking an operand socket restores its parameter to the constant its registered default
    // names; see registeredDefault() for why that, and not a remembered previous value, is what an
    // explicit disconnect lands on.
    if (const auto* binding = detail::parameterSocketBinding(graph, input_)) {
        const auto* parameter = composition->parameters().find(binding->parameterId);
        if (parameter == nullptr)
            return detail::invalidTarget();
        if (!std::holds_alternative<document::DriverBindingSource>(parameter->source))
            return OperationResult::noChange();
        const auto* fixed = std::get_if<document::NodeInputRef>(&input_);
        auto fallback = fixed == nullptr ? std::nullopt
                                         : detail::registeredDefault(graph, fixed->nodeId,
                                                                     binding->role, registry_);
        if (!fallback.has_value())
            return OperationResult::rejected(OperationIssueCode::Unsupported,
                                             "This socket has no registered default to restore");
        if (!composition->parameters().setSource(
                binding->parameterId, document::ConstantValueSource{*std::move(fallback)}))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Registered default violates its parameter schema");
        if (const auto failure = detail::validateGraph(*composition, registry_))
            return *failure;
        return OperationResult::applied();
    }
    const auto* edge = detail::inputEdge(graph, input_);
    if (!edge)
        return OperationResult::noChange();
    const auto upstream = edge->source.nodeId;
    const auto downstream = detail::destinationNode(input_);
    (void)graph.eraseEdge(edge->id);
    // A reroute with nothing on either side is a dot floating in the canvas that the artist never
    // placed: it existed only to bend a wire, and the wire is gone (task FIX1, item I). Removed in
    // the SAME transaction, so one undo puts the link and its bend back together. Both ends are
    // checked, because either of them may be the reroute this disconnect stranded.
    for (const auto candidate : {upstream, downstream})
        (void)detail::eraseStrandedReroute(*composition, candidate);
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
        // A stack slot is NOT reconnected to the dissolved node's upstream source: a slot belongs
        // to a layer, and the layer goes with its boundary node (CanonicalGraph::eraseNode removes
        // both). Before task FIX1 this refused outright; now that a slot is created and removed by
        // connecting and disconnecting it, dissolving a participating Layer Output simply takes the
        // layer out of the stack, which is what the gesture means.
        if (std::holds_alternative<document::LayerStackInputRef>(edge.destination))
            continue;
        edge.source = source;
        consumers.push_back(std::move(edge));
    }
    std::set<document::ParameterId> candidates;
    for (const auto& binding : node->parameters)
        candidates.insert(binding.parameterId);
    (void)graph.eraseNode(nodeId_);
    composition->nodeLayout().erase(nodeId_);
    (void)detail::detachFromNodeGroups(*composition, {nodeId_});
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
