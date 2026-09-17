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
    // Driver bindings are edges for ordering purposes (task S7): a node whose parameter is driven
    // depends on the value node that drives it, so it has to come after it. Feeding them into THIS
    // map rather than ordering the value graph separately is what makes a cycle through a driver a
    // cycle the one existing check reports.
    for (const auto* node : reachableNodes_) {
        if (cancelled()) {
            return std::nullopt;
        }
        for (const auto& reference : driverReferences(*node)) {
            if (!indegree.contains(reference.source.nodeId)) {
                continue;
            }
            outgoing[reference.source.nodeId].push_back(node->id);
            ++indegree[node->id];
        }
    }

    for (const auto* node : reachableNodes_) {
        const auto boundary = layerOutputs_.find(node->id);
        if (boundary == layerOutputs_.end())
            continue;
        const auto parentId = boundary->second->parent;
        if (!parentId)
            continue;
        const auto* parent = composition_->graph().findLayer(*parentId);
        if (parent && indegree.contains(parent->nodeId)) {
            outgoing[parent->nodeId].push_back(node->id);
            ++indegree[node->id];
        }
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
        if ((isMuted(node->id) || emptyImages_.contains(node->id)) &&
            !transformParents_.contains(node->id))
            continue;
        for (const auto& binding : node->parameters) {
            if (cancelled()) {
                return std::nullopt;
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
        // runtime::compileAnimationCurve() (issue #86, task E1;
        // bloom/runtime/curve_compilation.hpp)
        // -- this loop keeps only the per-curve index bookkeeping and cancellation checkpoints that
        // are specific to compiling a REACHABLE SET of curves.
        std::visit(
            [&](const auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    scalarCurveIndices_.emplace(
                        curve.id, runtime::ScalarCurveIndex::fromRaw(tables.scalar.size()));
                    tables.scalar.push_back(runtime::compileAnimationCurve(curve));
                } else if constexpr (std::is_same_v<Curve, document::Vec2AnimationCurve>) {
                    vec2CurveIndices_.emplace(curve.id,
                                              runtime::Vec2CurveIndex::fromRaw(tables.vec2.size()));
                    tables.vec2.push_back(runtime::compileAnimationCurve(curve));
                    for (const auto& parameter : composition_->parameters().records()) {
                        const auto* source =
                            std::get_if<document::AnimationCurveSource>(&parameter.source);
                        if (source != nullptr && source->curveId == curve.id &&
                            source->defaultValue.has_value()) {
                            if (const auto* value =
                                    std::get_if<document::Vec2d>(&*source->defaultValue))
                                tables.vec2.back().defaultValue = *value;
                        }
                    }
                } else if constexpr (std::is_same_v<Curve, document::Vec3AnimationCurve>) {
                    vec3CurveIndices_.emplace(curve.id,
                                              runtime::Vec3CurveIndex::fromRaw(tables.vec3.size()));
                    tables.vec3.push_back(runtime::compileAnimationCurve(curve));
                    for (const auto& parameter : composition_->parameters().records()) {
                        const auto* source =
                            std::get_if<document::AnimationCurveSource>(&parameter.source);
                        if (source != nullptr && source->curveId == curve.id &&
                            source->defaultValue.has_value()) {
                            if (const auto* value =
                                    std::get_if<document::Vec3d>(&*source->defaultValue))
                                tables.vec3.back().defaultValue = *value;
                        }
                    }
                } else {
                    color4CurveIndices_.emplace(
                        curve.id, runtime::Color4CurveIndex::fromRaw(tables.color4.size()));
                    tables.color4.push_back(runtime::compileAnimationCurve(curve));
                    for (const auto& parameter : composition_->parameters().records()) {
                        const auto* source =
                            std::get_if<document::AnimationCurveSource>(&parameter.source);
                        if (source != nullptr && source->curveId == curve.id &&
                            source->defaultValue.has_value()) {
                            if (const auto* value =
                                    std::get_if<core::Color4d>(&*source->defaultValue))
                                tables.color4.back().defaultValue = *value;
                        }
                    }
                }
            },
            record);
        if (cancelled()) {
            return std::nullopt;
        }
    }
    if (scalarCurveIndices_.size() + vec2CurveIndices_.size() + vec3CurveIndices_.size() +
            color4CurveIndices_.size() !=
        reachableCurveIds.size()) {
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
        if (cancelled())
            return {};
        const auto* node = findNode(id);
        if (!node)
            continue;
        const auto* definition = registry_.find(node->typeId, node->schemaVersion);
        if (!definition || definition->lowering == runtime::NodeLoweringKind::LayerStack ||
            definition->lowering == runtime::NodeLoweringKind::CompositionOutput)
            continue;
        // A value node carries no pixels, so "empty image" is not a state it can be in -- and a
        // muted one must not be classified as one, or the parameter it drives would lose its
        // source.
        if (isValueNode(id))
            continue;
        if (!isMuted(id) && definition->lowering != runtime::NodeLoweringKind::LayerOutput)
            continue;
        const auto input = firstImageInput(*node);
        const auto edge = input ? std::ranges::find_if(reachableEdges_,
                                                       [&](const auto* candidate) {
                                                           return candidate->destination == *input;
                                                       })
                                : reachableEdges_.end();
        // A Layer Output with nothing feeding its content port is empty whether it is muted or not
        // (task FIX1, item B). The artist wires a Layer node up by hand, so "added but not yet fed"
        // is an ordinary intermediate state; it draws nothing and says nothing, rather than failing
        // the whole compile on a required input.
        if ((edge == reachableEdges_.end() &&
             (isMuted(id) || definition->lowering == runtime::NodeLoweringKind::LayerOutput)) ||
            (edge != reachableEdges_.end() && emptyImages_.contains((*edge)->source.nodeId))) {
            emptyImages_.insert(id);
        }
    }
    auto curveTables = compileReachableCurves();
    if (!curveTables.has_value()) {
        return {};
    }
    // The value graph is compiled FIRST, and in the same order: every parameter the image pass
    // lowers may name one of its outputs, so the whole of it has to exist before a single image
    // operation is built. It shares no address space with the image chain -- a ValueOutputIndex and
    // an OperationIndex are different things -- which is exactly why the two passes can be
    // sequential rather than interleaved.
    if (!compileValueGraph(order)) {
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
        // Audio sources are compiled into the sibling audio mix below. They must remain in the
        // reachable/topological set so audio edges participate in graph validation, but never enter
        // the image operation variant or alter image evaluation semantics.
        if (definition->second->lowering == runtime::NodeLoweringKind::AudioSource)
            continue;
        if (emptyImages_.contains(nodeId) && !transformParents_.contains(nodeId))
            continue;
        if (isValueNode(nodeId))
            continue;
        // An Image Reroute is ELIDED rather than compiled: its consumers read its input's operation
        // directly, so it costs nothing at evaluation -- the same treatment a muted node's bypass
        // already gets, and the generalisation of DissolveNode's single Image pair to a node that
        // exists only to tidy a wire.
        if (isImageReroute(nodeId)) {
            const auto* rerouteEdge = fixedInputEdge(nodeId, document::kValuePortName);
            const auto source =
                rerouteEdge == nullptr ? indices.end() : indices.find(rerouteEdge->source.nodeId);
            if (source == indices.end()) {
                addTopologyFailure(nodeId, "Image Reroute input was not lowered.");
                return {};
            }
            indices.emplace(nodeId, source->second);
            continue;
        }
        if (isMuted(nodeId) && !transformParents_.contains(nodeId) &&
            definition->second->lowering != runtime::NodeLoweringKind::LayerStack &&
            definition->second->lowering != runtime::NodeLoweringKind::CompositionOutput) {
            const auto input = firstImageInput(*node);
            const auto edge =
                input ? std::ranges::find_if(
                            reachableEdges_,
                            [&](const auto* candidate) { return candidate->destination == *input; })
                      : reachableEdges_.end();
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
            operations.emplace_back(runtime::CompiledMerge{nodeId, {}});
            indices.emplace(nodeId, runtime::OperationIndex::fromRaw(operations.size()));
            operations.emplace_back(runtime::CompiledCompositionOutput{nodeId, empty});
            continue;
        }
        // An empty transform parent still owns authored transform values. Give it an empty
        // image input without adding document nodes or Merge membership.
        std::optional<runtime::OperationIndex> emptyParentInput;
        if (emptyImages_.contains(nodeId) && transformParents_.contains(nodeId)) {
            emptyParentInput = runtime::OperationIndex::fromRaw(operations.size());
            operations.emplace_back(runtime::CompiledMerge{nodeId, {}});
        }
        auto operation = emptyParentInput ? lowerLayerOutput(*node, indices, emptyParentInput)
                                          : lowerNode(*node, *definition->second, indices);
        if (!operation.has_value()) {
            return {};
        }
        // Output always publishes a full composition image. A graph wired directly to a
        // source or Layer gets a derived Normal Merge, including empty/cropped Layer images.
        // Its zero slot ID is synthetic plan metadata, never a durable document slot.
        if (auto* output = std::get_if<runtime::CompiledCompositionOutput>(&*operation);
            output &&
            !std::holds_alternative<runtime::CompiledMerge>(operations[output->input.value()])) {
            const auto normalized = runtime::OperationIndex::fromRaw(operations.size());
            operations.emplace_back(runtime::CompiledMerge{nodeId, {{{}, {}, output->input}}});
            output->input = normalized;
        }
        const auto index = runtime::OperationIndex::fromRaw(operations.size());
        operations.push_back(*operation);
        indices.emplace(nodeId, index);
    }

    if (!resolveBoundsReadouts(indices)) {
        return {};
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
    const auto audioMix = lowerAudioMix();
    if (!audioMix.has_value()) {
        return {};
    }
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{
            request_.snapshot.revision(), request_.snapshot.project().id(), request_.compositionId,
            composition_->format(), std::move(operations), output->second,
            std::move(curveTables->scalar), std::move(curveTables->vec2),
            std::move(curveTables->vec3), std::move(curveTables->color4),
            std::move(valueOperations_), valueOutputCount_,
            runtime::kCompiledCompositionPlanSemanticsVersion,
            runtime::kAnimationSamplingSemanticsVersion, !request_.parameterOverrides.empty(),
            *audioMix});
}

[[nodiscard]] std::optional<runtime::CompositionAudioMix> lowerAudioMix() {
    runtime::CompositionAudioMix mix;
    const auto& graph = composition_->graph();
    const auto& endpoint = graph.compositionOutput();
    if (!endpoint.has_value())
        return std::nullopt;
    mix.outputNodeId = endpoint->nodeId;
    const auto outputEdge = std::ranges::find_if(graph.edges(), [&](const auto& edge) {
        const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
        return input != nullptr && input->nodeId == endpoint->nodeId &&
               input->port == document::kCompositionOutputAudioInputPort;
    });
    if (outputEdge == graph.edges().end())
        return mix;
    // Lowers one audio source node into `mix.sources`, returning its index.
    const auto appendSource =
        [&](const document::NodeRecord& audioSource) -> std::optional<std::size_t> {
        const auto* assetBinding = findParameterBinding(audioSource, "asset");
        const auto* startBinding = findParameterBinding(audioSource, "startFrame");
        const auto* assetText = parameterConstant<std::string>(assetBinding);
        const auto* startFrame = parameterConstant<std::int64_t>(startBinding);
        const auto level = compiledScalarParameter(findParameterBinding(audioSource, "level"));
        std::uint64_t assetRaw = 0;
        if (assetText == nullptr || startFrame == nullptr || !level || assetText->empty() ||
            std::from_chars(assetText->data(), assetText->data() + assetText->size(), assetRaw)
                    .ec != std::errc{} ||
            !document::AssetId::fromRaw(assetRaw).isValid() ||
            composition_->parameters().find(assetBinding->parameterId) == nullptr ||
            request_.snapshot.project().findAsset(document::AssetId::fromRaw(assetRaw)) ==
                nullptr) {
            addTopologyFailure(audioSource.id, "Audio source parameters could not be lowered.");
            return std::nullopt;
        }
        const auto sourceIndex = mix.sources.size();
        mix.sources.push_back(
            {audioSource.id, document::AssetId::fromRaw(assetRaw), *startFrame, *level});
        return sourceIndex;
    };
    // Lowers one Layer node as an audio layer: its boundary supplies enable, solo, and range; the
    // source on its audio input supplies the clip. A Layer whose audio input is unwired contributes
    // nothing rather than failing, so an image-only layer routed into the output stays valid.
    const auto appendLayer = [&](const document::NodeId layerNodeId) -> bool {
        const auto* layerNode = findNode(layerNodeId);
        const auto boundary =
            std::ranges::find_if(graph.layerOutputs(), [&](const auto& candidate) {
                return candidate.nodeId == layerNodeId;
            });
        if (layerNode == nullptr || layerNode->typeId != document::kLayerOutputNodeType ||
            boundary == graph.layerOutputs().end()) {
            addTopologyFailure(layerNodeId, "Validated audio layer topology could not be lowered.");
            return false;
        }
        const auto layerAudioEdgeIterator =
            std::ranges::find_if(graph.edges(), [&](const auto& edge) {
                const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
                return input != nullptr && input->nodeId == layerNodeId &&
                       input->port == document::kLayerOutputAudioInputPort;
            });
        if (layerAudioEdgeIterator == graph.edges().end())
            return true;
        const auto* audioSource = findNode(layerAudioEdgeIterator->source.nodeId);
        if (audioSource == nullptr || audioSource->typeId != document::kAudioSourceNodeType) {
            addTopologyFailure(layerAudioEdgeIterator->source.nodeId,
                               "Audio layer does not have an audio source.");
            return false;
        }
        const auto sourceIndex = appendSource(*audioSource);
        if (!sourceIndex)
            return false;
        mix.layers.push_back({layerNodeId, boundary->layerId, *sourceIndex,
                              boundary->enabled && !isMuted(boundary->nodeId), boundary->solo,
                              boundary->inPoint, boundary->endPoint(composition_->duration())});
        return true;
    };

    const auto feedNodeId = outputEdge->source.nodeId;
    if (const auto* stack = graph.merge(feedNodeId); stack != nullptr) {
        // The usual shape: the Merge sums the audio of every slot that carries an audio edge.
        for (const auto& entry : stack->entries()) {
            if (cancelled())
                return std::nullopt;
            const auto stackEdgeIterator =
                std::ranges::find_if(graph.edges(), [&](const auto& edge) {
                    const auto* input =
                        std::get_if<document::LayerStackInputRef>(&edge.destination);
                    return input != nullptr && input->stackNodeId == stack->nodeId() &&
                           input->slotId == entry.slotId &&
                           input->role == document::kLayerStackAudioInputRole;
                });
            if (stackEdgeIterator == graph.edges().end())
                continue;
            if (!appendLayer(stackEdgeIterator->source.nodeId))
                return std::nullopt;
        }
        return mix;
    }
    const auto* feed = findNode(feedNodeId);
    if (feed != nullptr && feed->typeId == document::kLayerOutputNodeType) {
        // A Layer wired straight into the output is a one-layer mix.
        if (!appendLayer(feedNodeId))
            return std::nullopt;
        return mix;
    }
    if (feed != nullptr && feed->typeId == document::kAudioSourceNodeType) {
        // An audio source wired straight into the output plays whole, from the composition start.
        const auto sourceIndex = appendSource(*feed);
        if (!sourceIndex)
            return std::nullopt;
        mix.layers.push_back({feedNodeId, document::LayerId{}, *sourceIndex, !isMuted(feedNodeId),
                              false, core::RationalTime{}, composition_->duration()});
        return mix;
    }
    addTopologyFailure(feedNodeId,
                       "Composition audio is not fed by a Merge, a Layer, or an Audio source.");
    return std::nullopt;
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerNode(const document::NodeRecord& node, const runtime::NodeDefinition& definition,
          const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using runtime::NodeLoweringKind;
    switch (definition.lowering) {
    case NodeLoweringKind::Solid:
        return lowerSolid(node);
    case NodeLoweringKind::ImageSource:
        return lowerImageSource(node);
    case NodeLoweringKind::AudioSource:
        break;
    case NodeLoweringKind::Text:
        return lowerText(node);
    case NodeLoweringKind::LayerOutput:
        return lowerLayerOutput(node, indices);
    case NodeLoweringKind::LayerStack:
        return lowerLayerStack(node, definition, indices);
    case NodeLoweringKind::CompositionOutput:
        return lowerCompositionOutput(node, indices);
    case NodeLoweringKind::Shape:
        return lowerShape(node);
    case NodeLoweringKind::Unsupported:
    // A value lowering never reaches here: compileValueGraph() compiled it into the plan's value
    // operations, and lower()'s own loop skips it. Reaching this arm means the two passes disagree
    // about which graph a node belongs to, which is a topology failure rather than a silent skip.
    case NodeLoweringKind::ValueConstant:
    case NodeLoweringKind::ValueTime:
    case NodeLoweringKind::ValueScalarMath:
    case NodeLoweringKind::ValueVectorMath:
    case NodeLoweringKind::ValueVectorReduce:
    case NodeLoweringKind::ValueMapRange:
    case NodeLoweringKind::ValueClamp:
    case NodeLoweringKind::ValueMix:
    case NodeLoweringKind::ValueColorMix:
    case NodeLoweringKind::ValueCompare:
    case NodeLoweringKind::ValueSwitch:
    case NodeLoweringKind::ValueSeparate:
    case NodeLoweringKind::ValueCombine:
    case NodeLoweringKind::ValueRandom:
    case NodeLoweringKind::ValueReroute:
    case NodeLoweringKind::ValueBoundsReadout:
    case NodeLoweringKind::ValueUtility:
        break;
    }
    addTopologyFailure(node.id, "Unsupported lowering reached plan publication.");
    return std::nullopt;
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerSolid(const document::NodeRecord& node) {
    using namespace document;
    const auto* binding = findParameterBinding(node, kSolidColorParameterRole);
    // Task S5: a solid colour is a typed operand now, lowered through exactly the same
    // constant-or-curve-index helper a transform operand uses, so an animated solid colour and an
    // animated position are resolved by one rule rather than two.
    const auto color = compiledColorParameter(binding);
    if (binding == nullptr || !color.has_value()) {
        addTopologyFailure(node.id, "Validated solid color could not be lowered.");
        return std::nullopt;
    }
    auto width = compiledScalarParameter(findParameterBinding(node, kSolidWidthParameterRole));
    auto height = compiledScalarParameter(findParameterBinding(node, kSolidHeightParameterRole));
    if (!width || !height) {
        addTopologyFailure(node.id, "Solid dimensions could not be lowered.");
        return std::nullopt;
    }
    return runtime::CompiledSolid{node.id, *color, *width, *height};
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerImageSource(const document::NodeRecord& node) {
    const auto* asset = parameterConstant<std::string>(findParameterBinding(node, "asset"));
    const auto* start = parameterConstant<std::int64_t>(findParameterBinding(node, "startFrame"));
    const auto* loop = parameterConstant<std::int64_t>(findParameterBinding(node, "loopMode"));
    const auto* space = parameterConstant<std::int64_t>(findParameterBinding(node, "colorSpace"));
    const auto* premultiply = parameterConstant<bool>(findParameterBinding(node, "premultiply"));
    if (!asset || !start || !loop || !space || !premultiply) {
        addTopologyFailure(node.id, "Image source parameters could not be lowered.");
        return std::nullopt;
    }
    std::uint64_t raw = 0;
    const auto parsed = std::from_chars(asset->data(), asset->data() + asset->size(), raw);
    const auto* record =
        parsed.ec == std::errc{} && parsed.ptr == asset->data() + asset->size()
            ? request_.snapshot.project().findAsset(document::AssetId::fromRaw(raw))
            : nullptr;
    return runtime::CompiledImageSource{node.id, record ? std::optional{*record} : std::nullopt,
                                        *start,  *loop,
                                        *space,  *premultiply};
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerShape(const document::NodeRecord& node) {
    const auto size = compiledVec2Parameter(findParameterBinding(node, "size"));
    const auto fill = compiledColorParameter(findParameterBinding(node, "fillColor"));
    const auto stroke = compiledColorParameter(findParameterBinding(node, "strokeColor"));
    const auto width = compiledScalarParameter(findParameterBinding(node, "strokeWidth"));
    const auto* kind = parameterConstant<std::int64_t>(findParameterBinding(node, "kind"));
    const auto* radius = parameterConstant<double>(findParameterBinding(node, "cornerRadius"));
    const auto* points = parameterConstant<std::int64_t>(findParameterBinding(node, "points"));
    const auto* ratio = parameterConstant<double>(findParameterBinding(node, "innerRatio"));
    const auto* start = parameterConstant<document::Vec2d>(findParameterBinding(node, "lineStart"));
    const auto* end = parameterConstant<document::Vec2d>(findParameterBinding(node, "lineEnd"));
    const auto* path = parameterConstant<document::PathValue>(findParameterBinding(node, "path"));
    const auto* fillEnabled = parameterConstant<bool>(findParameterBinding(node, "fillEnabled"));
    const auto* strokeEnabled =
        parameterConstant<bool>(findParameterBinding(node, "strokeEnabled"));
    const auto* align = parameterConstant<std::int64_t>(findParameterBinding(node, "strokeAlign"));
    const auto* join = parameterConstant<std::int64_t>(findParameterBinding(node, "strokeJoin"));
    const auto* cap = parameterConstant<std::int64_t>(findParameterBinding(node, "strokeCap"));
    const auto* rule = parameterConstant<std::int64_t>(findParameterBinding(node, "fillRule"));
    if (!size || !fill || !stroke || !width || !kind || !radius || !points || !ratio || !start ||
        !end || !path || !fillEnabled || !strokeEnabled || !align || !join || !cap || !rule) {
        addTopologyFailure(node.id, "Shape parameters could not be lowered.");
        return std::nullopt;
    }
    return runtime::CompiledShape{node.id,
                                  static_cast<document::ShapeKind>(*kind),
                                  *size,
                                  *radius,
                                  *points,
                                  *ratio,
                                  *start,
                                  *end,
                                  *path,
                                  *fillEnabled,
                                  *fill,
                                  *strokeEnabled,
                                  *stroke,
                                  *width,
                                  static_cast<document::ShapeStrokeAlign>(*align),
                                  static_cast<document::ShapeStrokeJoin>(*join),
                                  static_cast<document::ShapeStrokeCap>(*cap),
                                  static_cast<document::ShapeFillRule>(*rule)};
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerText(const document::NodeRecord& node) {
    using namespace document;
    const auto* contentBinding = findParameterBinding(node, kTextParameterRole);
    const auto* sizeBinding = findParameterBinding(node, kTextSizeParameterRole);
    const auto* colorBinding = findParameterBinding(node, kTextColorParameterRole);
    const auto* fontBinding = findParameterBinding(node, kTextFontParameterRole);
    const auto* content = parameterConstant<std::string>(contentBinding);
    // Task DRIVE-1: a driven content parameter holds no constant at all -- its source IS the driver
    // -- so the constant is absent exactly when the driver is present, and the lowering needs one
    // of the two rather than both.
    const auto drivenContent = drivenOutput(contentBinding, runtime::SocketValueKind::String);
    const auto size = compiledScalarParameter(sizeBinding);
    const auto color = compiledColorParameter(colorBinding);
    const auto* storedFont = parameterConstant<std::int64_t>(fontBinding);
    auto face = render::EmbeddedFace::DejaVuSans;
    document::AssetId fontAssetId;
    render::TextFont font = face;
    const auto fontWarning = [&](const std::string& detail) {
        auto diagnosticSubject = subject(node.id, "font");
        if (fontBinding != nullptr)
            diagnosticSubject.parameterId = fontBinding->parameterId;
        addWarning(runtime::CompileDiagnosticCode::FontAssetUnavailable,
                   std::move(diagnosticSubject), "Text font is unavailable", detail);
    };
    const auto embeddedDigest = [](const render::EmbeddedFace candidate) {
        constexpr std::array<std::string_view, 4> digests{
            "7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954",
            "40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82",
            "97ad806f526e41546d46365bb3a393145f75b7b1568913db74549ad8b8dba872",
            "78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3"};
        return core::Sha256Digest::fromLowercaseHex(
            digests[static_cast<std::size_t>(candidate)]);
    };
    const auto useEmbedded = [&](const render::EmbeddedFace candidate,
                                 const document::AssetId assetId = {}) {
        face = candidate;
        font = candidate;
        fontAssetId = assetId;
    };
    if (fontBinding != nullptr) {
        if (storedFont != nullptr) {
            if (*storedFont < 0 || *storedFont >= kTextFontChoiceCount) {
                fontWarning("The legacy embedded face value is outside the supported range; DejaVu Sans was used.");
            } else {
                useEmbedded(static_cast<render::EmbeddedFace>(*storedFont));
            }
        } else if (const auto* reference = parameterConstant<std::string>(fontBinding)) {
            std::uint64_t rawId = 0;
            const auto parsedId = std::from_chars(reference->data(), reference->data() + reference->size(), rawId);
            const auto* asset = parsedId.ec == std::errc{} &&
                                        parsedId.ptr == reference->data() + reference->size()
                                    ? request_.snapshot.project().findAsset(
                                          document::AssetId::fromRaw(rawId))
                                    : nullptr;
            if (asset == nullptr || asset->kind != document::AssetKind::Font) {
                fontWarning("The font reference does not name a Font asset; DejaVu Sans was used.");
            } else {
                fontAssetId = asset->id;
                if (asset->locator.portability == "builtin") {
                    if (asset->fontIndex >= kTextFontChoiceCount) {
                        fontWarning("The embedded Font asset face index is invalid; DejaVu Sans was used.");
                    } else {
                        const auto candidate = static_cast<render::EmbeddedFace>(asset->fontIndex);
                        if (embeddedDigest(candidate).value_or(core::Sha256Digest{}) !=
                            asset->contentDigest) {
                            fontWarning("The embedded Font asset digest changed; DejaVu Sans was used.");
                        } else {
                            useEmbedded(candidate, asset->id);
                        }
                    }
                } else {
                    constexpr std::uintmax_t kMaximumFontBytes =
                        static_cast<std::uintmax_t>(64) * 1024U * 1024U;
                    std::error_code error;
                    const auto sizeOnDisk = std::filesystem::file_size(asset->locator.path, error);
                    if (error || sizeOnDisk == 0 || sizeOnDisk > kMaximumFontBytes) {
                        fontWarning("The system font file is missing or exceeds the compile-time size limit; DejaVu Sans was used.");
                    } else {
                        try {
                            auto bytes = std::make_shared<std::vector<std::uint8_t>>(
                                static_cast<std::size_t>(sizeOnDisk));
                            std::ifstream input(asset->locator.path, std::ios::binary);
                            input.read(reinterpret_cast<char*>(bytes->data()),
                                       static_cast<std::streamsize>(bytes->size()));
                            const auto digest = input
                                                    ? core::Sha256Hasher::hash(
                                                          std::as_bytes(std::span(*bytes)))
                                                    : std::nullopt;
                            if (!input || !digest.has_value() || *digest != asset->contentDigest) {
                                fontWarning("The system font file is missing or its digest changed; DejaVu Sans was used.");
                            } else {
                                font = render::ExternalFontFile{std::move(bytes), *digest,
                                                                 asset->fontIndex};
                            }
                        } catch (const std::bad_alloc&) {
                            fontWarning("The system font could not be loaded within the compile memory budget; DejaVu Sans was used.");
                        }
                    }
                }
            }
        } else {
            fontWarning("The Font parameter has an unsupported value; DejaVu Sans was used.");
        }
    }
    if (contentBinding == nullptr || sizeBinding == nullptr || colorBinding == nullptr ||
        (content == nullptr && !drivenContent.has_value()) || !size.has_value() ||
        !color.has_value()) {
        addTopologyFailure(node.id, "Validated text parameters could not be lowered.");
        return std::nullopt;
    }
    runtime::CompiledTextLayout layout;
    {
        const auto* alignmentBinding = findParameterBinding(node, kTextAlignmentParameterRole);
        const auto* alignment = parameterConstant<std::int64_t>(alignmentBinding);
        const auto drivenAlignment =
            drivenOutput(alignmentBinding, runtime::SocketValueKind::Integer);
        const auto lineHeight =
            compiledScalarParameter(findParameterBinding(node, kTextLineHeightParameterRole));
        const auto letterSpacing =
            compiledScalarParameter(findParameterBinding(node, kTextLetterSpacingParameterRole));
        if (alignmentBinding == nullptr || (alignment == nullptr && !drivenAlignment.has_value()) ||
            !lineHeight || !letterSpacing) {
            addTopologyFailure(node.id, "Text layout could not be lowered.");
            return std::nullopt;
        }
        layout.alignmentId = alignmentBinding->parameterId;
        layout.alignment = alignment == nullptr ? 0 : *alignment;
        layout.lineHeight = *lineHeight;
        layout.letterSpacing = *letterSpacing;
        layout.drivenAlignment = drivenAlignment;
        const auto* boxBinding = findParameterBinding(node, kTextBoxParameterRole);
        const auto* wrapBinding = findParameterBinding(node, kTextWrapParameterRole);
        const auto* verticalBinding = findParameterBinding(node, kTextVerticalAlignmentParameterRole);
        const auto* anchorBinding = findParameterBinding(node, kTextAnchorModeParameterRole);
        const auto* overflowBinding = findParameterBinding(node, kTextOverflowParameterRole);
        if (boxBinding != nullptr) {
            layout.boxId = boxBinding->parameterId;
            if (const auto* box = parameterConstant<Vec2d>(boxBinding))
                layout.box = *box;
        }
        if (wrapBinding != nullptr) {
            layout.wrapId = wrapBinding->parameterId;
            if (const auto* wrap = parameterConstant<bool>(wrapBinding))
                layout.wrap = *wrap;
        }
        const auto readDiscrete = [this](const document::ParameterBinding* binding,
                                         const std::int64_t fallback) {
            const auto* value = parameterConstant<std::int64_t>(binding);
            return value == nullptr ? fallback : *value;
        };
        if (verticalBinding != nullptr) {
            layout.verticalAlignmentId = verticalBinding->parameterId;
            layout.verticalAlignment = readDiscrete(verticalBinding, 0);
        }
        if (anchorBinding != nullptr) {
            layout.anchorModeId = anchorBinding->parameterId;
            layout.anchorMode = readDiscrete(anchorBinding, 0);
        }
        if (overflowBinding != nullptr) {
            layout.overflowId = overflowBinding->parameterId;
            layout.overflow = readDiscrete(overflowBinding, 0);
        }
    }
    runtime::CompiledText result;
    result.sourceNodeId = node.id;
    result.contentParameterId = contentBinding->parameterId;
    result.content = content == nullptr ? std::string{} : *content;
    result.size = *size;
    result.color = *color;
    result.layout = layout;
    result.drivenContent = drivenContent;
    result.face = face;
    result.fontAssetId = fontAssetId;
    result.font = std::move(font);
    return result;
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerLayerOutput(const document::NodeRecord& node,
                 const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices,
                 std::optional<runtime::OperationIndex> emptyInput = std::nullopt) {
    using namespace document;
    const auto input = emptyInput
                           ? emptyInput
                           : findInputOperation(node.id, kLayerOutputContentInputPort, indices);
    const auto boundary = layerOutputs_.find(node.id);
    const auto* positionBinding = findParameterBinding(node, kPositionParameterRole);
    const auto* anchorBinding = findParameterBinding(node, kAnchorParameterRole);
    const auto* scaleBinding = findParameterBinding(node, kScaleParameterRole);
    const auto* rotationBinding = findParameterBinding(node, kRotationParameterRole);
    const auto* opacityBinding = findParameterBinding(node, kOpacityParameterRole);
    const auto* blendModeBinding = findParameterBinding(node, kBlendModeParameterRole);
    // The blend mode lowers like CompiledText's three values: a validated constant read straight
    // off the parameter store, not a curve source, because the schema declares it non-animatable.
    // An integer naming no implemented mode cannot reach here -- ParameterStore refuses it on
    // insert and document validation refuses it on publication -- so failing to resolve one is a
    // topology failure, exactly as a missing transform binding is.
    const auto* storedBlendMode = parameterConstant<std::int64_t>(blendModeBinding);
    const auto blendMode = storedBlendMode == nullptr
                               ? std::nullopt
                               : core::blendModeFromStoredValue(*storedBlendMode);
    // Task DRIVE-1: driven, the mode has no stored constant, so the plan carries the registry
    // default beside the driver and the evaluator reads the driver. A mode is a jump between two
    // named modes rather than a value with a midpoint, which is exactly what a driver expresses.
    const auto drivenBlendMode = drivenOutput(blendModeBinding, runtime::SocketValueKind::Integer);
    const auto position = compiledVec2Parameter(positionBinding);
    const auto anchor = compiledVec2Parameter(anchorBinding);
    const auto scale = compiledVec2Parameter(scaleBinding);
    const auto rotation = compiledScalarParameter(rotationBinding);
    const auto opacity = compiledScalarParameter(opacityBinding);
    if (!input || boundary == layerOutputs_.end() || positionBinding == nullptr ||
        anchorBinding == nullptr || scaleBinding == nullptr || rotationBinding == nullptr ||
        opacityBinding == nullptr || blendModeBinding == nullptr || !position.has_value() ||
        !anchor.has_value() || !scale.has_value() || !rotation.has_value() ||
        !opacity.has_value() || (!blendMode.has_value() && !drivenBlendMode.has_value())) {
        addTopologyFailure(node.id, "Validated Layer Output could not be lowered.");
        return std::nullopt;
    }
    std::optional<runtime::OperationIndex> parentIndex;
    if (boundary->second->parent) {
        const auto* parent = composition_->graph().findLayer(*boundary->second->parent);
        const auto found = parent ? indices.find(parent->nodeId) : indices.end();
        if (found == indices.end()) {
            addTopologyFailure(node.id, "Layer parent must be lowered before its child.");
            return std::nullopt;
        }
        parentIndex = found->second;
    }
    return runtime::CompiledLayerOutput{node.id,
                                        boundary->second->layerId,
                                        *input,
                                        *position,
                                        *anchor,
                                        *scale,
                                        *rotation,
                                        *opacity,
                                        blendModeBinding->parameterId,
                                        blendMode.value_or(core::kDefaultBlendMode),
                                        boundary->second->inPoint,
                                        boundary->second->endPoint(composition_->duration()),
                                        drivenBlendMode,
                                        parentIndex};
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerLayerStack(const document::NodeRecord& node, const runtime::NodeDefinition& definition,
                const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    if (!definition.layerSlotInput.has_value()) {
        addTopologyFailure(node.id, "Layer Stack definition has no slot-input contract.");
        return std::nullopt;
    }
    if (!composition_->graph().merge(node.id)->enabled())
        return runtime::CompiledMerge{node.id, {}};
    const auto& layerSlotInput = *definition.layerSlotInput;
    std::vector<runtime::CompiledMergeInput> entries;
    entries.reserve(composition_->graph().merge(node.id)->entries().size());
    const auto slots = composition_->graph().merge(node.id)->entries();
    for (const auto& entry : slots) {
        if ((isMuted(node.id) && entry.slotId != slots.front().slotId) || mutedLayer(entry))
            continue;
        if (cancelled()) {
            return std::nullopt;
        }
        const auto* edge = layerSlotInputEdge(node.id, entry.slotId, layerSlotInput.role);
        if (edge == nullptr) {
            // An audio-only layer occupies the same stable stack slot but has no image operation.
            // Its typed audio edge is lowered by lowerAudioMix(), so it is intentionally absent
            // from the image merge entries.
            if (definition.audioLayerSlotInput.has_value() &&
                layerSlotInputEdge(node.id, entry.slotId, definition.audioLayerSlotInput->role) !=
                    nullptr)
                continue;
            addTopologyFailure(node.id, "Validated Layer Stack input could not be lowered.");
            return std::nullopt;
        }
        if (emptyImages_.contains(edge->source.nodeId))
            continue;
        const auto source = indices.find(edge->source.nodeId);
        if (source == indices.end()) {
            addTopologyFailure(node.id, "Layer Stack input operation is unavailable.");
            return std::nullopt;
        }
        entries.push_back({entry.slotId, entry.layerId, source->second});
    }
    return runtime::CompiledMerge{node.id, std::move(entries)};
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

// Task DRIVE-1. The value-graph output the parameter behind `binding` is driven by, or nothing.
// The companion of parameterConstant() for the kinds that have no curve table of their own: a
// String, an Integer or a Boolean is either the constant the store holds or the value a driver
// hands it, and this answers the second half of that question for every one of them through the
// same driverOutput() the typed operands already use.
[[nodiscard]] std::optional<runtime::ValueOutputIndex>
drivenOutput(const document::ParameterBinding* binding, const runtime::SocketValueKind kind) {
    const auto* parameter = binding == nullptr ? nullptr : findParameter(binding->parameterId);
    return parameter == nullptr ? std::nullopt : driverOutput(*parameter, kind);
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

// No longer const: resolving a driver may SYNTHESIZE a promotion operation into the value graph,
// and a widening that needs an operation is better compiled once here than hidden inside whoever
// reads the value.
[[nodiscard]] std::optional<runtime::CompiledScalarParameter>
compiledScalarParameter(const document::ParameterBinding* binding) {
    if (binding == nullptr) {
        return std::nullopt;
    }
    const auto* parameter = findParameter(binding->parameterId);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source)) {
        const auto* value = std::get_if<double>(&constant->value);
        return value == nullptr
                   ? std::nullopt
                   : std::optional(runtime::CompiledScalarParameter{parameter->id, *value});
    }
    // Task S7: the third arm. A driver binding resolves to the value-graph output it names, through
    // a promotion operation when the kinds differ -- so an Integer node can drive a Scalar operand
    // without the widening being invisible.
    if (const auto driven = driverOutput(*parameter, runtime::SocketValueKind::Scalar)) {
        return runtime::CompiledScalarParameter{parameter->id, *driven};
    }
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    const auto index =
        source == nullptr ? scalarCurveIndices_.end() : scalarCurveIndices_.find(source->curveId);
    return index == scalarCurveIndices_.end()
               ? std::nullopt
               : std::optional(runtime::CompiledScalarParameter{parameter->id, index->second});
}

[[nodiscard]] std::optional<runtime::CompiledVec2Parameter>
compiledVec2Parameter(const document::ParameterBinding* binding) {
    if (binding == nullptr) {
        return std::nullopt;
    }
    const auto* parameter = findParameter(binding->parameterId);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source)) {
        const auto* value = std::get_if<document::Vec2d>(&constant->value);
        return value == nullptr
                   ? std::nullopt
                   : std::optional(runtime::CompiledVec2Parameter{parameter->id, *value});
    }
    if (const auto driven = driverOutput(*parameter, runtime::SocketValueKind::Vector2)) {
        return runtime::CompiledVec2Parameter{parameter->id, *driven};
    }
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    const auto index =
        source == nullptr ? vec2CurveIndices_.end() : vec2CurveIndices_.find(source->curveId);
    return index == vec2CurveIndices_.end()
               ? std::nullopt
               : std::optional(runtime::CompiledVec2Parameter{parameter->id, index->second});
}

[[nodiscard]] std::optional<runtime::CompiledColorParameter>
compiledColorParameter(const document::ParameterBinding* binding) {
    if (binding == nullptr) {
        return std::nullopt;
    }
    const auto* parameter = findParameter(binding->parameterId);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source)) {
        const auto* value = std::get_if<core::Color4d>(&constant->value);
        return value == nullptr
                   ? std::nullopt
                   : std::optional(runtime::CompiledColorParameter{parameter->id, *value});
    }
    if (const auto driven = driverOutput(*parameter, runtime::SocketValueKind::Color)) {
        return runtime::CompiledColorParameter{parameter->id, *driven};
    }
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    const auto index =
        source == nullptr ? color4CurveIndices_.end() : color4CurveIndices_.find(source->curveId);
    return index == color4CurveIndices_.end()
               ? std::nullopt
               : std::optional(runtime::CompiledColorParameter{parameter->id, index->second});
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
