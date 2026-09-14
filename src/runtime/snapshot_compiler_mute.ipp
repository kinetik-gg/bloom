[[nodiscard]] bool isMuted(const document::NodeId id) const {
    const auto found = composition_->nodeLayout().find(id);
    return found != composition_->nodeLayout().end() && found->second.muted;
}

[[nodiscard]] bool hasImageOutput(const runtime::NodeDefinition& definition) const {
    return std::ranges::any_of(definition.outputs, [](const auto& output) {
        return output.valueKind == runtime::SocketValueKind::Image;
    });
}

[[nodiscard]] std::optional<document::InputPortRef>
firstImageInput(const document::NodeRecord& node) const {
    const auto* definition = registry_.find(node.typeId, node.schemaVersion);
    // The image-output test is what excludes a VALUE node, which carries no pixels for a mute
    // bypass to pass through. The composition Output carries pixels -- it is where they end -- and
    // since task FIX1, item H it publishes no socket at all, so it is named here rather than
    // failing the test and taking its own input edge out of the reachable set when muted.
    if (definition == nullptr ||
        (!hasImageOutput(*definition) &&
         definition->lowering != runtime::NodeLoweringKind::CompositionOutput))
        return std::nullopt;
    for (const auto& input : definition->inputs) {
        if (input.valueKind == runtime::SocketValueKind::Image)
            return document::NodeInputRef{node.id, input.name};
    }
    const auto* stack = composition_->graph().merge(node.id);
    const auto entries = stack ? stack->entries() : std::span<const document::LayerStackEntry>{};
    if (definition->layerSlotInput &&
        definition->layerSlotInput->valueKind == runtime::SocketValueKind::Image &&
        !entries.empty()) {
        return document::LayerStackInputRef{node.id, entries.front().slotId,
                                            definition->layerSlotInput->role};
    }
    return std::nullopt;
}

[[nodiscard]] bool mutedLayer(const document::LayerStackEntry& entry) const {
    const auto boundaries = composition_->graph().layerOutputs();
    const bool anySolo = std::ranges::any_of(boundaries, &document::LayerOutputBoundary::solo);
    for (const auto& boundary : boundaries) {
        if (boundary.layerId == entry.layerId)
            return !boundary.enabled || (anySolo && !boundary.solo) || isMuted(boundary.nodeId);
    }
    return false;
}

[[nodiscard]] bool consumesEdge(const document::NodeRecord& node,
                                const document::EdgeRecord& edge) const {
    if (isMuted(node.id)) {
        const auto input = firstImageInput(node);
        if (!input || *input != edge.destination)
            return false;
    }
    if (const auto* slot = std::get_if<document::LayerStackInputRef>(&edge.destination)) {
        const auto* stack = composition_->graph().merge(slot->stackNodeId);
        const auto* entry = stack ? stack->find(slot->slotId) : nullptr;
        if (entry && mutedLayer(*entry))
            return false;
    }
    return true;
}
