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
    if (!definition || !hasImageOutput(*definition)) return std::nullopt;
    for (const auto& input : definition->inputs) {
        if (input.valueKind == runtime::SocketValueKind::Image)
            return document::NodeInputRef{node.id, input.name};
    }
    const auto entries = composition_->graph().layerStack().entries();
    if (definition->layerSlotInput &&
        definition->layerSlotInput->valueKind == runtime::SocketValueKind::Image && !entries.empty()) {
        return document::LayerStackInputRef{node.id, entries.front().slotId,
                                             definition->layerSlotInput->role};
    }
    return std::nullopt;
}

[[nodiscard]] bool mutedLayer(const document::LayerStackEntry& entry) const {
    for (const auto& boundary : composition_->graph().layerOutputs()) {
        if (boundary.layerId == entry.layerId) return isMuted(boundary.nodeId);
    }
    return false;
}

[[nodiscard]] bool consumesEdge(const document::NodeRecord& node,
                                const document::EdgeRecord& edge) const {
    if (isMuted(node.id)) {
        const auto input = firstImageInput(node);
        if (!input || *input != edge.destination) return false;
    }
    if (const auto* slot = std::get_if<document::LayerStackInputRef>(&edge.destination)) {
        const auto* entry = composition_->graph().layerStack().find(slot->slotId);
        if (entry && mutedLayer(*entry)) return false;
    }
    return true;
}
