#include <bloom/document/layer_source.hpp>

#include <bloom/document/project.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::document {
namespace {

// The constant a source node's parameter role holds, or null when it is animated, driven or absent.
[[nodiscard]] const ParameterValue*
constantFor(const Composition& composition, const NodeRecord& node, const std::string_view role) {
    for (const auto& binding : node.parameters) {
        if (binding.role != role)
            continue;
        const auto* record = composition.parameters().find(binding.parameterId);
        if (record == nullptr)
            return nullptr;
        const auto* constant = std::get_if<ConstantValueSource>(&record->source);
        return constant == nullptr ? nullptr : &constant->value;
    }
    return nullptr;
}

[[nodiscard]] const NodeRecord* feedingNode(const Composition& composition, const NodeId target,
                                            const std::string_view port) {
    for (const auto& edge : composition.graph().edges()) {
        const auto* input = std::get_if<NodeInputRef>(&edge.destination);
        if (input != nullptr && input->nodeId == target && input->port == port)
            return composition.graph().findNode(edge.source.nodeId);
    }
    return nullptr;
}

[[nodiscard]] std::optional<LayerSource> sourceOfNode(const Composition& composition,
                                                      const NodeRecord& node) {
    if (node.typeId == kCompositionSourceNodeType) {
        const auto* value = constantFor(composition, node, "composition");
        const auto* id = value == nullptr ? nullptr : std::get_if<std::int64_t>(value);
        if (id == nullptr || *id <= 0)
            return std::nullopt;
        return CompositionId::fromRaw(static_cast<std::uint64_t>(*id));
    }
    if (node.typeId != "bloom.image-source" && node.typeId != "bloom.video-source" &&
        node.typeId != kAudioSourceNodeType)
        return std::nullopt;
    const auto* value = constantFor(composition, node, "asset");
    const auto* text = value == nullptr ? nullptr : std::get_if<std::string>(value);
    if (text == nullptr || text->empty())
        return std::nullopt;
    std::uint64_t raw = 0;
    const auto* end = text->data() + text->size();
    const auto [last, error] = std::from_chars(text->data(), end, raw);
    if (error != std::errc{} || last != end || raw == 0)
        return std::nullopt;
    return AssetId::fromRaw(raw);
}

} // namespace

std::optional<LayerSource> layerSource(const Composition& composition,
                                       const LayerOutputBoundary& layer) {
    // Effects inserted between the source and the layer keep the layer's source: walk up the
    // content chain through each node's own "image" input, bounded by the node count so a malformed
    // cycle cannot spin.
    for (const auto port : {kLayerOutputContentInputPort, kLayerOutputAudioInputPort}) {
        const auto* node = feedingNode(composition, layer.nodeId, port);
        for (std::size_t step = 0; node != nullptr && step <= composition.graph().nodes().size();
             ++step) {
            if (auto source = sourceOfNode(composition, *node))
                return source;
            node = feedingNode(composition, node->id, kLayerOutputContentInputPort);
        }
    }
    return std::nullopt;
}

std::optional<std::string> layerSourceName(const Project& project, const Composition& composition,
                                           const LayerOutputBoundary& layer) {
    const auto source = layerSource(composition, layer);
    if (!source)
        return std::nullopt;
    if (const auto* asset = std::get_if<AssetId>(&*source)) {
        const auto* record = project.findAsset(*asset);
        if (record == nullptr || record->name.empty())
            return std::nullopt;
        return record->name;
    }
    const auto* nested = project.findComposition(std::get<CompositionId>(*source));
    if (nested == nullptr || nested->name().empty())
        return std::nullopt;
    return nested->name();
}

bool refreshLayerNamesFromSources(Project& project, const std::optional<CompositionId> only) {
    // Read first through the const view, so an unaffected composition's shared content is never
    // copied for a write that does not happen.
    std::vector<std::pair<CompositionId, std::vector<std::pair<LayerId, std::string>>>> renames;
    const auto& readable = std::as_const(project);
    for (const auto& composition : readable.compositions()) {
        if (only && composition.id() != *only)
            continue;
        std::vector<std::pair<LayerId, std::string>> layers;
        for (const auto& layer : composition.graph().layerOutputs()) {
            if (!layer.nameFollowsSource)
                continue;
            auto name = layerSourceName(readable, composition, layer);
            if (name && *name != layer.name)
                layers.emplace_back(layer.layerId, std::move(*name));
        }
        if (!layers.empty())
            renames.emplace_back(composition.id(), std::move(layers));
    }
    for (auto& [compositionId, layers] : renames) {
        auto* composition = project.findComposition(compositionId);
        if (composition == nullptr)
            continue;
        for (auto& [layerId, name] : layers)
            if (auto* layer = composition->graph().findLayer(layerId))
                layer->name = std::move(name);
    }
    return !renames.empty();
}

void inferLayerNameFollowsSource(Project& project) {
    std::vector<std::pair<CompositionId, std::vector<LayerId>>> following;
    const auto& readable = std::as_const(project);
    for (const auto& composition : readable.compositions()) {
        std::vector<LayerId> layers;
        for (const auto& layer : composition.graph().layerOutputs()) {
            const auto name = layerSourceName(readable, composition, layer);
            if (name && *name == layer.name)
                layers.push_back(layer.layerId);
        }
        if (!layers.empty())
            following.emplace_back(composition.id(), std::move(layers));
    }
    for (const auto& [compositionId, layers] : following) {
        auto* composition = project.findComposition(compositionId);
        if (composition == nullptr)
            continue;
        for (const auto layerId : layers)
            if (auto* layer = composition->graph().findLayer(layerId))
                layer->nameFollowsSource = true;
    }
}

} // namespace bloom::document
