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

[[nodiscard]] std::optional<CompiledCurveTables> compileReachableCurves() {
    std::unordered_set<document::AnimationCurveId> reachableCurveIds;
    for (const auto* node : reachableNodes_) {
        if (cancelled()) {
            return std::nullopt;
        }
        for (const auto& binding : node->parameters) {
            if (cancelled()) {
                return std::nullopt;
            }
            if (request_.parameterOverride.has_value() &&
                request_.parameterOverride->parameterId == binding.parameterId) {
                continue;
            }
            const auto* parameter = findParameter(binding.parameterId);
            const auto* source =
                parameter == nullptr
                    ? nullptr
                    : std::get_if<document::AnimationCurveSource>(&parameter->source);
            if (source != nullptr) {
                reachableCurveIds.insert(source->curveId);
            }
        }
    }

    const auto interpolation = [](const document::KeyframeInterpolation mode) {
        return mode == document::KeyframeInterpolation::Hold
                   ? runtime::CompiledKeyframeInterpolation::Hold
                   : runtime::CompiledKeyframeInterpolation::Linear;
    };

    CompiledCurveTables tables;
    for (const auto& record : composition_->animationCurves().records()) {
        if (cancelled()) {
            return std::nullopt;
        }
        const auto curveId = document::animationCurveId(record);
        if (!reachableCurveIds.contains(curveId)) {
            continue;
        }
        std::visit(
            [&](const auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    std::vector<runtime::CompiledScalarKeyframe> keyframes;
                    keyframes.reserve(curve.keyframes.size());
                    for (const auto& keyframe : curve.keyframes) {
                        if (cancelled()) {
                            return;
                        }
                        keyframes.push_back({keyframe.id, keyframe.time, keyframe.value,
                                             interpolation(keyframe.outgoingInterpolation)});
                    }
                    if (cancelled()) {
                        return;
                    }
                    scalarCurveIndices_.emplace(
                        curve.id, runtime::ScalarCurveIndex::fromRaw(tables.scalar.size()));
                    tables.scalar.push_back({curve.id, std::move(keyframes)});
                } else {
                    std::vector<runtime::CompiledVec2Keyframe> keyframes;
                    keyframes.reserve(curve.keyframes.size());
                    for (const auto& keyframe : curve.keyframes) {
                        if (cancelled()) {
                            return;
                        }
                        keyframes.push_back({keyframe.id, keyframe.time, keyframe.value,
                                             interpolation(keyframe.outgoingInterpolation)});
                    }
                    if (cancelled()) {
                        return;
                    }
                    vec2CurveIndices_.emplace(curve.id,
                                              runtime::Vec2CurveIndex::fromRaw(tables.vec2.size()));
                    tables.vec2.push_back({curve.id, std::move(keyframes)});
                }
            },
            record);
        if (cancelled()) {
            return std::nullopt;
        }
    }
    if (scalarCurveIndices_.size() + vec2CurveIndices_.size() != reachableCurveIds.size()) {
        addTopologyFailure({}, "A reachable animation curve could not be lowered.");
        return std::nullopt;
    }
    return tables;
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
lower(const std::vector<document::NodeId>& order) {
    auto curveTables = compileReachableCurves();
    if (!curveTables.has_value()) {
        return {};
    }
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

    const auto& compositionOutput = composition_->graph().compositionOutput();
    if (!compositionOutput.has_value()) {
        addTopologyFailure({}, "Composition output disappeared before lowering.");
        return {};
    }
    const auto outputNodeId = compositionOutput->nodeId;
    const auto output = indices.find(outputNodeId);
    if (output == indices.end()) {
        addTopologyFailure(outputNodeId, "Composition output was not lowered.");
        return {};
    }
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{
            request_.snapshot.revision(), request_.snapshot.project().id(), request_.compositionId,
            composition_->format(), std::move(operations), output->second,
            std::move(curveTables->scalar), std::move(curveTables->vec2),
            runtime::kCompiledCompositionPlanSemanticsVersion,
            runtime::kAnimationSamplingSemanticsVersion});
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
    const auto position = compiledVec2Parameter(positionBinding);
    const auto opacity = compiledScalarParameter(opacityBinding);
    if (!input || boundary == layerOutputs_.end() || positionBinding == nullptr ||
        opacityBinding == nullptr || !position.has_value() || !opacity.has_value()) {
        addTopologyFailure(node.id, "Validated Layer Output could not be lowered.");
        return std::nullopt;
    }
    return runtime::CompiledLayerOutput{node.id, boundary->second->layerId, *input, *position,
                                        *opacity};
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerLayerStack(const document::NodeRecord& node, const runtime::NodeDefinition& definition,
                const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    if (!definition.layerSlotInput.has_value()) {
        addTopologyFailure(node.id, "Layer Stack definition has no slot-input contract.");
        return std::nullopt;
    }
    const auto& layerSlotInput = *definition.layerSlotInput;
    std::vector<runtime::CompiledLayerStackEntry> entries;
    entries.reserve(composition_->graph().layerStack().entries().size());
    for (const auto& entry : composition_->graph().layerStack().entries()) {
        if (cancelled()) {
            return std::nullopt;
        }
        const auto* edge = layerSlotInputEdge(node.id, entry.slotId, layerSlotInput.role);
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

[[nodiscard]] std::optional<runtime::CompiledScalarParameter>
compiledScalarParameter(const document::ParameterBinding* binding) const noexcept {
    if (binding == nullptr) {
        return std::nullopt;
    }
    const auto* parameter = findParameter(binding->parameterId);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    if (request_.parameterOverride.has_value() &&
        request_.parameterOverride->parameterId == parameter->id) {
        const auto* value = std::get_if<double>(&request_.parameterOverride->value);
        return value == nullptr
                   ? std::nullopt
                   : std::optional(runtime::CompiledScalarParameter{parameter->id, *value});
    }
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source)) {
        const auto* value = std::get_if<double>(&constant->value);
        return value == nullptr
                   ? std::nullopt
                   : std::optional(runtime::CompiledScalarParameter{parameter->id, *value});
    }
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    const auto index =
        source == nullptr ? scalarCurveIndices_.end() : scalarCurveIndices_.find(source->curveId);
    return index == scalarCurveIndices_.end()
               ? std::nullopt
               : std::optional(runtime::CompiledScalarParameter{parameter->id, index->second});
}

[[nodiscard]] std::optional<runtime::CompiledVec2Parameter>
compiledVec2Parameter(const document::ParameterBinding* binding) const noexcept {
    if (binding == nullptr) {
        return std::nullopt;
    }
    const auto* parameter = findParameter(binding->parameterId);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    if (request_.parameterOverride.has_value() &&
        request_.parameterOverride->parameterId == parameter->id) {
        const auto* value = std::get_if<document::Vec2d>(&request_.parameterOverride->value);
        return value == nullptr
                   ? std::nullopt
                   : std::optional(runtime::CompiledVec2Parameter{parameter->id, *value});
    }
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source)) {
        const auto* value = std::get_if<document::Vec2d>(&constant->value);
        return value == nullptr
                   ? std::nullopt
                   : std::optional(runtime::CompiledVec2Parameter{parameter->id, *value});
    }
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    const auto index =
        source == nullptr ? vec2CurveIndices_.end() : vec2CurveIndices_.find(source->curveId);
    return index == vec2CurveIndices_.end()
               ? std::nullopt
               : std::optional(runtime::CompiledVec2Parameter{parameter->id, index->second});
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
