#include <bloom/document/graph.hpp>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

using bloom::document::InputPortRef;
using bloom::document::LayerStackInputRef;
using bloom::document::NodeId;
using bloom::document::NodeInputRef;

[[nodiscard]] NodeId destinationNode(const InputPortRef& destination) noexcept {
    return std::visit(
        [](const auto& input) {
            using Input = std::decay_t<decltype(input)>;
            if constexpr (std::is_same_v<Input, NodeInputRef>) {
                return input.nodeId;
            } else {
                return input.stackNodeId;
            }
        },
        destination);
}

[[nodiscard]] std::string destinationKey(const InputPortRef& destination) {
    return std::visit(
        [](const auto& input) {
            using Input = std::decay_t<decltype(input)>;
            if constexpr (std::is_same_v<Input, NodeInputRef>) {
                return "node:" + std::to_string(input.nodeId.value()) + ":" + input.port;
            } else {
                return "stack:" + std::to_string(input.stackNodeId.value()) + ":" +
                       std::to_string(input.slotId.value()) + ":" + input.role;
            }
        },
        destination);
}

[[nodiscard]] bool validDestination(const InputPortRef& destination) noexcept {
    return std::visit(
        [](const auto& input) {
            using Input = std::decay_t<decltype(input)>;
            if constexpr (std::is_same_v<Input, NodeInputRef>) {
                return input.nodeId.isValid() && !input.port.empty();
            } else {
                return input.stackNodeId.isValid() && input.slotId.isValid() && !input.role.empty();
            }
        },
        destination);
}

} // namespace

namespace bloom::document {

const NodeRecord* CanonicalGraph::findNode(const NodeId id) const noexcept {
    const auto iterator = std::find_if(nodes_.begin(), nodes_.end(),
                                       [id](const auto& node) { return node.id == id; });
    return iterator == nodes_.end() ? nullptr : &*iterator;
}

NodeRecord* CanonicalGraph::findNode(const NodeId id) noexcept {
    return const_cast<NodeRecord*>(std::as_const(*this).findNode(id));
}

bool CanonicalGraph::addNode(NodeRecord node) {
    if (!node.id.isValid() || node.typeId.empty() || findNode(node.id) != nullptr) {
        return false;
    }

    std::unordered_set<std::string> roles;
    for (const auto& binding : node.parameters) {
        if (binding.role.empty() || !binding.parameterId.isValid() ||
            !roles.insert(binding.role).second) {
            return false;
        }
    }

    nodes_.push_back(std::move(node));
    return true;
}

bool CanonicalGraph::addEdge(EdgeRecord edge) {
    if (!edge.id.isValid() || !edge.source.nodeId.isValid() || edge.source.port.empty() ||
        !validDestination(edge.destination)) {
        return false;
    }

    const auto duplicateId = std::find_if(edges_.begin(), edges_.end(),
                                          [&edge](const auto& item) { return item.id == edge.id; });
    const auto key = destinationKey(edge.destination);
    const auto duplicateInput =
        std::find_if(edges_.begin(), edges_.end(),
                     [&key](const auto& item) { return destinationKey(item.destination) == key; });
    if (duplicateId != edges_.end() || duplicateInput != edges_.end()) {
        return false;
    }

    edges_.push_back(std::move(edge));
    return true;
}

bool CanonicalGraph::addLayerOutput(LayerOutputBoundary boundary) {
    if (!boundary.nodeId.isValid() || !boundary.layerId.isValid() || boundary.name.empty() ||
        boundary.outputPort.empty()) {
        return false;
    }

    const auto duplicate =
        std::find_if(layerOutputs_.begin(), layerOutputs_.end(), [&boundary](const auto& item) {
            return item.nodeId == boundary.nodeId || item.layerId == boundary.layerId;
        });
    if (duplicate != layerOutputs_.end()) {
        return false;
    }

    layerOutputs_.push_back(std::move(boundary));
    return true;
}

ValidationResult CanonicalGraph::validate(const ParameterStore& parameters) const {
    ValidationResult result;
    result.append("layerStack", layerStack_.validate());

    std::unordered_set<NodeId> nodeIds;
    for (const auto& node : nodes_) {
        const auto path = "nodes[" + std::to_string(node.id.value()) + "]";
        if (!node.id.isValid()) {
            result.add(ValidationCode::InvalidId, path + ".id", "Node ID must not be zero");
        } else if (!nodeIds.insert(node.id).second) {
            result.add(ValidationCode::DuplicateId, path + ".id", "Node ID is duplicated");
        }
        if (node.typeId.empty()) {
            result.add(ValidationCode::EmptyKey, path + ".typeId",
                       "Node type ID must not be empty");
        }

        std::unordered_set<std::string> roles;
        for (const auto& binding : node.parameters) {
            const auto bindingPath = path + ".parameters[" + binding.role + "]";
            if (binding.role.empty()) {
                result.add(ValidationCode::EmptyKey, bindingPath,
                           "Parameter binding role must not be empty");
            } else if (!roles.insert(binding.role).second) {
                result.add(ValidationCode::DuplicateId, bindingPath,
                           "Parameter binding role is duplicated on the node");
            }
            if (!binding.parameterId.isValid()) {
                result.add(ValidationCode::InvalidId, bindingPath,
                           "Parameter binding ID must not be zero");
            } else if (parameters.find(binding.parameterId) == nullptr) {
                result.add(ValidationCode::MissingReference, bindingPath,
                           "Parameter binding references a missing parameter");
            }
        }
    }

    const auto* stackNode = findNode(layerStack_.nodeId());
    if (stackNode == nullptr) {
        result.add(ValidationCode::MissingReference, "layerStack.nodeId",
                   "Layer Stack references a missing node");
    } else if (stackNode->typeId != kLayerStackNodeType) {
        result.add(ValidationCode::InvalidLayerStack, "layerStack.nodeId",
                   "Layer Stack node has the wrong node type");
    }

    std::unordered_set<NodeId> boundaryNodeIds;
    std::unordered_map<LayerId, const LayerOutputBoundary*> boundariesByLayer;
    for (const auto& boundary : layerOutputs_) {
        const auto path = "layerOutputs[" + std::to_string(boundary.layerId.value()) + "]";
        if (!boundary.nodeId.isValid() || !boundary.layerId.isValid()) {
            result.add(ValidationCode::InvalidId, path,
                       "Layer Output node and layer IDs must not be zero");
        }
        if (boundary.outputPort.empty()) {
            result.add(ValidationCode::EmptyKey, path + ".outputPort",
                       "Layer Output port must not be empty");
        }
        if (boundary.name.empty()) {
            result.add(ValidationCode::EmptyKey, path + ".name", "Layer name must not be empty");
        }
        if (!boundaryNodeIds.insert(boundary.nodeId).second ||
            !boundariesByLayer.emplace(boundary.layerId, &boundary).second) {
            result.add(ValidationCode::DuplicateId, path,
                       "Layer Output node and layer IDs must be unique");
        }

        const auto* node = findNode(boundary.nodeId);
        if (node == nullptr) {
            result.add(ValidationCode::MissingReference, path + ".nodeId",
                       "Layer Output references a missing node");
        } else if (node->typeId != kLayerOutputNodeType) {
            result.add(ValidationCode::InvalidLayerBoundary, path + ".nodeId",
                       "Layer Output boundary node has the wrong node type");
        }
    }

    std::unordered_set<EdgeId> edgeIds;
    std::unordered_set<std::string> destinations;
    std::unordered_map<NodeId, std::vector<NodeId>> adjacency;
    std::unordered_map<NodeId, std::size_t> indegree;
    for (const auto& node : nodes_) {
        indegree.try_emplace(node.id, 0);
    }

    for (const auto& edge : edges_) {
        const auto path = "edges[" + std::to_string(edge.id.value()) + "]";
        if (!edge.id.isValid()) {
            result.add(ValidationCode::InvalidId, path + ".id", "Edge ID must not be zero");
        } else if (!edgeIds.insert(edge.id).second) {
            result.add(ValidationCode::DuplicateId, path + ".id", "Edge ID is duplicated");
        }
        if (!edge.source.nodeId.isValid() || edge.source.port.empty()) {
            result.add(ValidationCode::InvalidValue, path + ".source",
                       "Edge source must have a node and port");
        } else if (findNode(edge.source.nodeId) == nullptr) {
            result.add(ValidationCode::MissingReference, path + ".source.nodeId",
                       "Edge source references a missing node");
        }

        if (!validDestination(edge.destination)) {
            result.add(ValidationCode::InvalidValue, path + ".destination",
                       "Edge destination is invalid");
            continue;
        }

        const auto inputKey = destinationKey(edge.destination);
        if (!destinations.insert(inputKey).second) {
            result.add(ValidationCode::DuplicateInput, path + ".destination",
                       "More than one edge targets the same input");
        }

        const auto targetNodeId = destinationNode(edge.destination);
        if (findNode(targetNodeId) == nullptr) {
            result.add(ValidationCode::MissingReference, path + ".destination",
                       "Edge destination references a missing node");
        }

        if (const auto* nodeInput = std::get_if<NodeInputRef>(&edge.destination)) {
            if (nodeInput->nodeId == layerStack_.nodeId()) {
                result.add(ValidationCode::InvalidLayerStack, path + ".destination",
                           "Layer Stack inputs must address a stable slot and role");
            }
        } else {
            const auto& stackInput = std::get<LayerStackInputRef>(edge.destination);
            if (stackInput.stackNodeId != layerStack_.nodeId()) {
                result.add(ValidationCode::InvalidLayerStack, path + ".destination",
                           "Layer Stack edge targets a different stack node");
            }
            if (layerStack_.find(stackInput.slotId) == nullptr) {
                result.add(ValidationCode::MissingReference, path + ".destination.slotId",
                           "Layer Stack edge targets a missing stable slot");
            }
        }

        if (findNode(edge.source.nodeId) != nullptr && findNode(targetNodeId) != nullptr) {
            adjacency[edge.source.nodeId].push_back(targetNodeId);
            ++indegree[targetNodeId];
        }
    }

    for (const auto& entry : layerStack_.entries()) {
        const auto path = "layerStack.entries[" + std::to_string(entry.slotId.value()) + "]";
        const auto boundary = boundariesByLayer.find(entry.layerId);
        if (boundary == boundariesByLayer.end()) {
            result.add(ValidationCode::MissingReference, path + ".layerId",
                       "Layer Stack entry has no matching Layer Output boundary");
            continue;
        }

        const auto matchingContent =
            std::find_if(edges_.begin(), edges_.end(), [&](const EdgeRecord& edge) {
                const auto* input = std::get_if<LayerStackInputRef>(&edge.destination);
                return input != nullptr && input->stackNodeId == layerStack_.nodeId() &&
                       input->slotId == entry.slotId &&
                       input->role == kLayerStackContentInputRole &&
                       edge.source.nodeId == boundary->second->nodeId &&
                       edge.source.port == boundary->second->outputPort;
            });
        if (matchingContent == edges_.end()) {
            result.add(ValidationCode::InvalidLayerStack, path,
                       "Layer Stack slot must receive content from its matching Layer Output");
        }
    }

    if (!compositionOutput_.has_value()) {
        result.add(ValidationCode::MissingCompositionOutput, "compositionOutput",
                   "Composition graph requires one explicit output endpoint");
    } else if (!compositionOutput_->nodeId.isValid() || compositionOutput_->port.empty()) {
        result.add(ValidationCode::InvalidValue, "compositionOutput",
                   "Composition output endpoint is invalid");
    } else if (findNode(compositionOutput_->nodeId) == nullptr) {
        result.add(ValidationCode::MissingReference, "compositionOutput.nodeId",
                   "Composition output references a missing node");
    }

    std::deque<NodeId> ready;
    for (const auto& [nodeId, degree] : indegree) {
        if (degree == 0) {
            ready.push_back(nodeId);
        }
    }
    std::size_t visited = 0;
    while (!ready.empty()) {
        const auto nodeId = ready.front();
        ready.pop_front();
        ++visited;
        for (const auto target : adjacency[nodeId]) {
            auto& degree = indegree[target];
            --degree;
            if (degree == 0) {
                ready.push_back(target);
            }
        }
    }
    if (visited != indegree.size()) {
        result.add(ValidationCode::GraphCycle, "edges",
                   "Same-time processing graph must not contain a cycle");
    }

    return result;
}

} // namespace bloom::document
