[[nodiscard]] std::optional<std::vector<document::NodeId>> makeTopologicalOrder() {
    std::unordered_map<document::NodeId, std::size_t> indegree;
    std::unordered_map<document::NodeId, std::vector<document::NodeId>> outgoing;
    for (const auto* node : reachableNodes_) {
        if (cancelled()) {
            return std::nullopt;
        }
        indegree.emplace(node->id, 0);
    }
    for (const auto* edge : reachableEdges_) {
        if (cancelled()) {
            return std::nullopt;
        }
        outgoing[edge->source.nodeId].push_back(destinationNode(edge->destination));
        ++indegree[destinationNode(edge->destination)];
    }

    auto laterId = [](const document::NodeId left, const document::NodeId right) {
        return left.value() > right.value();
    };
    std::priority_queue<document::NodeId, std::vector<document::NodeId>, decltype(laterId)> ready(
        laterId);
    for (const auto* node : reachableNodes_) {
        if (cancelled()) {
            return std::nullopt;
        }
        if (indegree.at(node->id) == 0) {
            ready.push(node->id);
        }
    }

    std::vector<document::NodeId> order;
    order.reserve(reachableNodes_.size());
    while (!ready.empty()) {
        if (cancelled()) {
            return std::nullopt;
        }
        const auto nodeId = ready.top();
        ready.pop();
        order.push_back(nodeId);
        if (const auto targets = outgoing.find(nodeId); targets != outgoing.end()) {
            for (const auto target : targets->second) {
                if (cancelled()) {
                    return std::nullopt;
                }
                auto& degree = indegree.at(target);
                --degree;
                if (degree == 0) {
                    ready.push(target);
                }
            }
        }
    }
    if (order.size() != reachableNodes_.size()) {
        addFailure(runtime::CompileDiagnosticCode::TopologyInvariant, subject({}, "edges"),
                   "Reachable graph contains a cycle",
                   "The published graph violates its acyclic processing invariant.");
        return std::nullopt;
    }
    return order;
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
lower(const std::vector<document::NodeId>& order) {
    std::vector<runtime::CompiledOperation> operations;
    operations.reserve(order.size());
    std::unordered_map<document::NodeId, runtime::OperationIndex> indices;

    for (const auto nodeId : order) {
        if (cancelled()) {
            return {};
        }
        const auto* node = findNode(nodeId);
        const auto definition = definitions_.find(nodeId);
        if (node == nullptr || definition == definitions_.end()) {
            addTopologyFailure(nodeId, "A reachable node lost its registered definition.");
            return {};
        }
        const auto operation = lowerNode(*node, *definition->second, indices);
        if (!operation.has_value()) {
            return {};
        }
        const auto index = runtime::OperationIndex::fromRaw(operations.size());
        operations.push_back(*operation);
        indices.emplace(nodeId, index);
    }

    const auto outputNodeId = composition_->graph().compositionOutput()->nodeId;
    const auto output = indices.find(outputNodeId);
    if (output == indices.end()) {
        addTopologyFailure(outputNodeId, "Composition output was not lowered.");
        return {};
    }
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlan{
            request_.snapshot.revision(), request_.snapshot.project().id(), request_.compositionId,
            composition_->format(), std::move(operations), output->second});
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerNode(const document::NodeRecord& node, const runtime::NodeDefinition& definition,
          const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using runtime::NodeLoweringKind;
    switch (definition.lowering) {
    case NodeLoweringKind::Solid:
        return lowerSolid(node);
    case NodeLoweringKind::LayerOutput:
        return lowerLayerOutput(node, indices);
    case NodeLoweringKind::LayerStack:
        return lowerLayerStack(node, definition, indices);
    case NodeLoweringKind::CompositionOutput:
        return lowerCompositionOutput(node, indices);
    case NodeLoweringKind::Unsupported:
        break;
    }
    addTopologyFailure(node.id, "Unsupported lowering reached plan publication.");
    return std::nullopt;
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerSolid(const document::NodeRecord& node) {
    using namespace document;
    const auto* binding = findParameterBinding(node, kSolidColorParameterRole);
    const auto* parameter = binding == nullptr ? nullptr : findParameter(binding->parameterId);
    const auto* constant =
        parameter == nullptr ? nullptr : std::get_if<ConstantValueSource>(&parameter->source);
    const auto* color =
        constant == nullptr ? nullptr : std::get_if<core::Color4d>(&constant->value);
    if (binding == nullptr || color == nullptr) {
        addTopologyFailure(node.id, "Validated solid color could not be lowered.");
        return std::nullopt;
    }
    return runtime::CompiledSolid{node.id, binding->parameterId, *color};
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerLayerOutput(const document::NodeRecord& node,
                 const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using namespace document;
    const auto input = findInputOperation(node.id, kLayerOutputContentInputPort, indices);
    const auto boundary = layerOutputs_.find(node.id);
    const auto* positionBinding = findParameterBinding(node, kPositionParameterRole);
    const auto* opacityBinding = findParameterBinding(node, kOpacityParameterRole);
    const auto* position = parameterConstant<Vec2d>(positionBinding);
    const auto* opacity = parameterConstant<double>(opacityBinding);
    if (!input || boundary == layerOutputs_.end() || positionBinding == nullptr ||
        opacityBinding == nullptr || position == nullptr || opacity == nullptr) {
        addTopologyFailure(node.id, "Validated Layer Output could not be lowered.");
        return std::nullopt;
    }
    return runtime::CompiledLayerOutput{
        node.id,   boundary->second->layerId,   *input,  positionBinding->parameterId,
        *position, opacityBinding->parameterId, *opacity};
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerLayerStack(const document::NodeRecord& node, const runtime::NodeDefinition& definition,
                const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    std::vector<runtime::CompiledLayerStackEntry> entries;
    entries.reserve(composition_->graph().layerStack().entries().size());
    for (const auto& entry : composition_->graph().layerStack().entries()) {
        if (cancelled()) {
            return std::nullopt;
        }
        const auto* edge =
            layerSlotInputEdge(node.id, entry.slotId, definition.layerSlotInput->role);
        if (edge == nullptr) {
            addTopologyFailure(node.id, "Validated Layer Stack input could not be lowered.");
            return std::nullopt;
        }
        const auto source = indices.find(edge->source.nodeId);
        if (source == indices.end()) {
            addTopologyFailure(node.id, "Layer Stack input operation is unavailable.");
            return std::nullopt;
        }
        entries.push_back({entry.slotId, entry.layerId, source->second});
    }
    return runtime::CompiledLayerStack{node.id, std::move(entries)};
}

[[nodiscard]] std::optional<runtime::CompiledOperation> lowerCompositionOutput(
    const document::NodeRecord& node,
    const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto input = findInputOperation(node.id, document::kCompositionOutputInputPort, indices);
    if (!input) {
        addTopologyFailure(node.id, "Validated Composition Output could not be lowered.");
        return std::nullopt;
    }
    return runtime::CompiledCompositionOutput{node.id, *input};
}

template <typename Value>
[[nodiscard]] const Value*
parameterConstant(const document::ParameterBinding* binding) const noexcept {
    if (binding == nullptr) {
        return nullptr;
    }
    const auto* parameter = findParameter(binding->parameterId);
    const auto* constant = parameter == nullptr
                               ? nullptr
                               : std::get_if<document::ConstantValueSource>(&parameter->source);
    return constant == nullptr ? nullptr : std::get_if<Value>(&constant->value);
}

[[nodiscard]] std::optional<runtime::OperationIndex> findInputOperation(
    const document::NodeId nodeId, const std::string_view port,
    const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) const {
    const auto* edge = fixedInputEdge(nodeId, port);
    if (edge == nullptr) {
        return std::nullopt;
    }
    const auto source = indices.find(edge->source.nodeId);
    return source == indices.end() ? std::nullopt : std::optional(source->second);
}
