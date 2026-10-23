// Read-only menu eligibility for node operations (issue #357).
//
// Each predicate mirrors the rejection rules of the operation's apply() and the command stack's
// locked-layer refusal, answering "would executing this be accepted?" from the published
// composition alone. None of them copies the document, opens a draft or validates a project;
// where apply() would fail on its final graph validation, the predicate checks the specific
// condition that validation would find. A NoChange outcome is accepted, exactly as
// canApplyNodeOperation reports it.
#include <bloom/commands/node_operations.hpp>

#include "node_operation_support.hpp"

#include <bloom/document/persisted_text.hpp>

#include <algorithm>
#include <variant>

namespace bloom::commands {
namespace {
// Validation refuses a surviving parameter whose driver names a node that no longer exists.
[[nodiscard]] bool survivorsDrivenBy(const document::Composition& composition,
                                     const std::set<document::NodeId>& removed) {
    for (const auto& node : composition.graph().nodes()) {
        if (removed.contains(node.id))
            continue;
        for (const auto& binding : node.parameters) {
            const auto* parameter = composition.parameters().find(binding.parameterId);
            const auto* driver =
                parameter ? std::get_if<document::DriverBindingSource>(&parameter->source)
                          : nullptr;
            if (driver != nullptr && removed.contains(driver->sourceNodeId))
                return true;
        }
    }
    return false;
}
} // namespace

bool canRemoveNodes(const document::Composition& composition,
                    const std::set<document::NodeId>& nodes) {
    if (nodes.empty())
        return true; // NoChange.
    const auto& graph = composition.graph();
    for (const auto id : nodes)
        if (!graph.findNode(id) || detail::protectedNode(graph, id) || composition.nodeLocked(id))
            return false;
    return !survivorsDrivenBy(composition, nodes);
}

bool canDissolveNode(const document::Composition& composition, const document::NodeId nodeId,
                     const document::NodeDefinitionRegistry& registry) {
    const auto& graph = composition.graph();
    const auto* node = graph.findNode(nodeId);
    if (!node || detail::protectedNode(graph, nodeId) || composition.nodeLocked(nodeId))
        return false;
    const auto* definition = registry.find(node->typeId, node->schemaVersion);
    if (!definition)
        return false;
    const auto input = std::ranges::find(definition->inputs, document::SocketValueKind::Image,
                                         &document::InputPortDefinition::valueKind);
    const auto output = std::ranges::find(definition->outputs, document::SocketValueKind::Image,
                                          &document::OutputPortDefinition::valueKind);
    if (input == definition->inputs.end() || output == definition->outputs.end())
        return false;
    const auto* incoming = detail::inputEdge(graph, document::NodeInputRef{nodeId, input->name});
    if (!incoming)
        return false;
    // Each consumer is re-linked to the dissolved node's upstream source; CanonicalGraph::addEdge
    // refuses a link whose source and destination kinds no longer connect.
    const auto sourceKind = graph.outputKind(incoming->source, registry);
    for (const auto& edge : graph.edges()) {
        if (edge.source.nodeId != nodeId || edge.source.port != output->name ||
            std::holds_alternative<document::LayerStackInputRef>(edge.destination))
            continue;
        const auto targetKind = graph.inputKind(edge.destination, registry);
        if (sourceKind && targetKind &&
            !document::isAcceptedSocketConnection(*sourceKind, *targetKind))
            return false;
    }
    return !survivorsDrivenBy(composition, {nodeId});
}

bool canSetNodeMuted(const document::Composition& composition, const document::NodeId nodeId,
                     bool /*muted*/) {
    return composition.graph().findNode(nodeId) != nullptr;
}

bool canSetNodeCollapsed(const document::Composition& composition, const document::NodeId nodeId,
                         bool /*collapsed*/) {
    return composition.graph().findNode(nodeId) != nullptr;
}

bool canRenameLayer(const document::Composition& composition, const document::LayerId layerId,
                    const std::string_view name) {
    return document::isValidHumanFacingName(name) && composition.graph().findLayer(layerId);
}

bool canGroupNodes(const document::Composition& composition, const document::IdAllocator& ids,
                   const std::set<document::NodeId>& nodes, const std::string_view name) {
    if (nodes.empty() || !document::isValidHumanFacingName(name))
        return false;
    for (const auto id : nodes)
        if (!composition.graph().findNode(id))
            return false;
    auto available = ids;
    return available.allocateNodeGroup().has_value();
}
} // namespace bloom::commands
