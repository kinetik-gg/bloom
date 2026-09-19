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
        auto value = parameter.defaultValue;
        if (parameter.schemaKey == document::kSolidWidthParameterSchemaKey)
            value = static_cast<double>(composition->format().width());
        else if (parameter.schemaKey == document::kSolidHeightParameterSchemaKey)
            value = static_cast<double>(composition->format().height());
        else if (parameter.schemaKey == document::kPositionParameterSchemaKey)
            value = document::Vec2d{static_cast<double>(composition->format().width()) / 2.0,
                                    static_cast<double>(composition->format().height()) / 2.0};
        if (!composition->parameters().insert({*parameterId, parameter.schemaKey,
                                               document::ConstantValueSource{std::move(value)}}))
            return OperationResult::rejected(
                OperationIssueCode::InvalidValue,
                "Node definition default violates its parameter schema");
        node.parameters.push_back({parameter.role, *parameterId});
        outputs.push_back({"parameter." + parameter.role, *parameterId});
    }
    if (!composition->graph().addNode(std::move(node)))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Node could not be inserted");
    // The requested point is still authoritative when it is clear. When it is occupied, the
    // document-side spatial index moves only this new card to the nearest free slot; existing
    // authoring positions are never rewritten by AddNode.
    auto occupied = composition->nodeLayout();
    const auto defaults = document::defaultNodeLayout(composition->graph().nodes());
    for (const auto& existing : composition->graph().nodes())
        if (existing.id != *nodeId && !occupied.contains(existing.id))
            occupied.emplace(existing.id, defaults.at(existing.id));
    const auto placement = document::findNearestFreeNodePosition(
        occupied, {}, document::kConservativeNodeCardSize, layoutPosition_);
    composition->nodeLayout()[*nodeId] = {placement, 128.0, false, false};
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

namespace {

// TEMPORAL-DELETE. Inspects the graph BEFORE any erase and decides whether removing exactly this
// set of nodes can only change rendered output inside the union of the removed layer boundaries'
// half-open active spans. Conservative default: any mismatch yields std::nullopt, which means the
// whole render may have changed and no reuse evidence is published.
[[nodiscard]] std::optional<AffectedTimeFootprint>
removalTimeFootprint(const document::Composition& composition,
                     const std::set<document::NodeId>& nodes) {
    const auto& graph = composition.graph();
    const auto duration = composition.duration();

    // Every removed node must be a known layer boundary. Anything else (a source, effect, reroute,
    // merge, or value node) is unproven.
    std::vector<const document::LayerOutputBoundary*> boundaries;
    std::set<document::LayerId> removedLayers;
    for (const auto id : nodes) {
        const auto boundary =
            std::ranges::find(graph.layerOutputs(), id, &document::LayerOutputBoundary::nodeId);
        if (boundary == graph.layerOutputs().end())
            return std::nullopt;
        boundaries.push_back(&*boundary);
        removedLayers.insert(boundary->layerId);
    }

    // A surviving child parented to a removed boundary follows only the deleted head, so its output
    // outside the deleted span changes too.
    for (const auto& layer : graph.layerOutputs()) {
        if (nodes.contains(layer.nodeId))
            continue;
        if (layer.parent.has_value() && removedLayers.contains(*layer.parent))
            return std::nullopt;
    }

    // A removed boundary's output may not feed any non-merge consumer: node inputs, effects,
    // reroutes and the composition output all consume the whole output, including outside the
    // visible span.
    for (const auto& edge : graph.edges()) {
        if (!nodes.contains(edge.source.nodeId))
            continue;
        if (!std::holds_alternative<document::LayerStackInputRef>(edge.destination))
            return std::nullopt;
    }

    // A surviving parameter driven by a removed node's output depends on it at every time.
    for (const auto& record : composition.parameters().records()) {
        if (const auto* driver = std::get_if<document::DriverBindingSource>(&record.source)) {
            if (nodes.contains(driver->sourceNodeId))
                return std::nullopt;
        }
    }

    // Removing a solo layer can UNsuppress other layers across the whole composition.
    for (const auto* boundary : boundaries) {
        if (boundary->solo)
            return std::nullopt;
    }

    // A muted Layer Stack compiles only its first slot (snapshot_compiler_lowering.ipp). Removing a
    // participating layer can promote a later layer into that slot, changing output wherever the
    // promoted layer is active -- which need not lie inside the removed boundary's own span. That
    // is unprovable, so a removed boundary participating in a muted stack forces whole-render.
    for (const auto* boundary : boundaries) {
        const bool participatesInMutedStack =
            std::ranges::any_of(graph.merges(), [&](const document::LayerStack& stack) {
                const auto layout = composition.nodeLayout().find(stack.nodeId());
                if (layout == composition.nodeLayout().end() || !layout->second.muted)
                    return false;
                return std::ranges::any_of(stack.entries(),
                                           [&](const document::LayerStackEntry& entry) {
                                               return entry.layerId == boundary->layerId;
                                           });
            });
        if (participatesInMutedStack)
            return std::nullopt;
    }

    // The change is confined to where a removed, merge-participating boundary was active. A
    // boundary with no merge slot is isolated (no outgoing edges were allowed above) and
    // contributes nothing.
    std::vector<AffectedTimeRange> intervals;
    for (const auto* boundary : boundaries) {
        const bool participating = std::ranges::any_of(graph.merges(), [&](const auto& merge) {
            return std::ranges::any_of(merge.entries(),
                                       [&](const document::LayerStackEntry& entry) {
                                           return entry.layerId == boundary->layerId;
                                       });
        });
        if (!participating)
            continue;
        intervals.push_back({boundary->inPoint, boundary->endPoint(duration)});
    }
    return normalizeAffectedTimeFootprint(AffectedTimeFootprint{.compositionId = composition.id(),
                                                                .intervals = std::move(intervals)});
}

} // namespace

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
                "Output and its directly connected Merge cannot be removed");
        for (const auto& binding : node->parameters)
            candidates.insert(binding.parameterId);
    }
    // Classify from the pre-erase graph. std::nullopt is the conservative whole-render default; a
    // present footprint (possibly empty) is provable bounded reuse evidence.
    const auto footprint = removalTimeFootprint(*composition, nodes_);
    for (const auto id : nodes_) {
        (void)composition->graph().eraseNode(id);
        composition->nodeLayout().erase(id);
    }
    // A removed node leaves its frame, and a frame with nothing left in it goes with it.
    (void)detail::detachFromNodeGroups(*composition, nodes_);
    detail::eraseOrphanedParameters(*composition, candidates);
    auto result = OperationResult::applied();
    result.affectedTimes = footprint;
    return result;
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
        // An audio edge shares its Layer's existing row rather than opening a second one: one slot
        // per layer carries both roles (docs/architecture/layer-graph-model.md, Audio Sources And
        // Layers). A content drop always opens a fresh row -- the image edge is what puts a Layer
        // in the stack in the first place -- so this reuse applies only to the audio role.
        const document::LayerStackEntry* existingSlot = nullptr;
        if (slot->role == document::kLayerStackAudioInputRole && layerId.isValid()) {
            const auto entries = stack->entries();
            const auto found =
                std::ranges::find(entries, layerId, &document::LayerStackEntry::layerId);
            if (found != entries.end())
                existingSlot = &*found;
        }
        document::LayerSlotId slotId;
        if (existingSlot != nullptr) {
            slotId = existingSlot->slotId;
        } else {
            const auto allocatedSlotId = draft.ids().allocateLayerSlot();
            if (!allocatedSlotId)
                return detail::exhaustedIds();
            slotId = *allocatedSlotId;
        }
        const auto edgeId = draft.ids().allocateEdge();
        if (!edgeId)
            return detail::exhaustedIds();
        if (existingSlot == nullptr) {
            if (!stack->append({slotId, layerId}))
                return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                 "Layer stack slot could not be inserted");
            if (insertBefore_.has_value() && !stack->moveBefore(slotId, insertBefore_))
                return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                 "Layer stack slot could not be ordered");
        }
        if (!graph.addEdge({*edgeId, source_,
                            document::LayerStackInputRef{slot->stackNodeId, slotId, slot->role}},
                           registry_))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Layer stack connection could not be inserted");
        if (const auto failure = detail::validateGraph(*composition, registry_))
            return *failure;
        return OperationResult::applied(
            {{std::string(kConnectPortsSlotOutput), slotId}, {"edge", *edgeId}});
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
        // Detaching a stack slot's ONLY remaining edge removes the slot (task FIX1, item B): a slot
        // with nothing in it is not a shape the canonical graph admits, so "the slot" and "the link
        // into it" are one thing to the artist and one thing here. A slot now carries up to two
        // typed edges, content and audio (task FOLLOW-1) -- one Layer, one row, both roles -- so
        // removing just the audio link from an otherwise-fed Layer must leave its image row exactly
        // where it was; only a slot left with NEITHER edge is the shape this actually removes. The
        // Layer node keeps its boundary and its LayerId regardless, so reconnecting is one gesture
        // rather than a rebuild.
        const auto* edge = detail::inputEdge(graph, input_);
        if (edge != nullptr)
            (void)graph.eraseEdge(edge->id);
        const bool stillFed = std::ranges::any_of(graph.edges(), [&](const auto& candidate) {
            const auto* other = std::get_if<document::LayerStackInputRef>(&candidate.destination);
            return other != nullptr && other->stackNodeId == slot->stackNodeId &&
                   other->slotId == slot->slotId;
        });
        if (!stillFed && !stack->erase(slot->slotId))
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
            "Output and its directly connected Merge cannot be dissolved");
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
