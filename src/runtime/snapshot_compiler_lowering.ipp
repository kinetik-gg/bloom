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
        if (isMuted(node->id) || emptyImages_.contains(node->id)) continue;
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

    CompiledCurveTables tables;
    for (const auto& record : composition_->animationCurves().records()) {
        if (cancelled()) {
            return std::nullopt;
        }
        const auto curveId = document::animationCurveId(record);
        if (!reachableCurveIds.contains(curveId)) {
            continue;
        }
        // The document -> compiled per-curve conversion itself is the shared, pure
        // runtime::compileAnimationCurve() (issue #86, task E1; bloom/runtime/curve_compilation.hpp)
        // -- this loop keeps only the per-curve index bookkeeping and cancellation checkpoints that
        // are specific to compiling a REACHABLE SET of curves.
        std::visit(
            [&](const auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    scalarCurveIndices_.emplace(
                        curve.id, runtime::ScalarCurveIndex::fromRaw(tables.scalar.size()));
                    tables.scalar.push_back(runtime::compileAnimationCurve(curve));
                } else {
                    vec2CurveIndices_.emplace(curve.id,
                                              runtime::Vec2CurveIndex::fromRaw(tables.vec2.size()));
                    tables.vec2.push_back(runtime::compileAnimationCurve(curve));
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
    // Empty image values need no fake parameter IDs or evaluator operations. Propagate them
    // through bypasses and transparent-preserving boundaries before compiling curve tables.
    for (const auto id : order) {
        if (cancelled()) return {};
        const auto* node = findNode(id);
        if (!node) continue;
        const auto* definition = registry_.find(node->typeId, node->schemaVersion);
        if (!definition || definition->lowering == runtime::NodeLoweringKind::LayerStack ||
            definition->lowering == runtime::NodeLoweringKind::CompositionOutput) continue;
        if (!isMuted(id) && definition->lowering != runtime::NodeLoweringKind::LayerOutput) continue;
        const auto input = firstImageInput(*node);
        const auto edge = input ? std::ranges::find_if(reachableEdges_, [&](const auto* candidate) {
            return candidate->destination == *input;
        }) : reachableEdges_.end();
        if ((edge == reachableEdges_.end() && isMuted(id)) ||
            (edge != reachableEdges_.end() && emptyImages_.contains((*edge)->source.nodeId))) {
            emptyImages_.insert(id);
        }
    }
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
        if (emptyImages_.contains(nodeId)) continue;
        if (isMuted(nodeId) && definition->second->lowering != runtime::NodeLoweringKind::LayerStack &&
            definition->second->lowering != runtime::NodeLoweringKind::CompositionOutput) {
            const auto input = firstImageInput(*node);
            const auto edge = input ? std::ranges::find_if(reachableEdges_, [&](const auto* candidate) {
                return candidate->destination == *input;
            }) : reachableEdges_.end();
            if (edge != reachableEdges_.end()) {
                const auto source = indices.find((*edge)->source.nodeId);
                if (source == indices.end()) {
                    addTopologyFailure(nodeId, "Muted node input was not lowered.");
                    return {};
                }
                indices.emplace(nodeId, source->second);
            } else {
                addTopologyFailure(nodeId, "Muted empty image was not classified.");
                return {};
            }
            continue;
        }
        const auto* outputEdge = fixedInputEdge(nodeId, document::kCompositionOutputInputPort);
        if (definition->second->lowering == runtime::NodeLoweringKind::CompositionOutput &&
            ((isMuted(nodeId) && outputEdge == nullptr) ||
             (outputEdge && emptyImages_.contains(outputEdge->source.nodeId)))) {
            const auto empty = runtime::OperationIndex::fromRaw(operations.size());
            operations.emplace_back(runtime::CompiledLayerStack{nodeId, {}});
            indices.emplace(nodeId, runtime::OperationIndex::fromRaw(operations.size()));
            operations.emplace_back(runtime::CompiledCompositionOutput{nodeId, empty});
            continue;
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
    const auto slots = composition_->graph().layerStack().entries();
    for (const auto& entry : slots) {
        if ((isMuted(node.id) && entry.slotId != slots.front().slotId) || mutedLayer(entry)) continue;
        if (cancelled()) {
            return std::nullopt;
        }
        const auto* edge = layerSlotInputEdge(node.id, entry.slotId, layerSlotInput.role);
        if (edge == nullptr) {
            addTopologyFailure(node.id, "Validated Layer Stack input could not be lowered.");
            return std::nullopt;
        }
        if (emptyImages_.contains(edge->source.nodeId)) continue;
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
