#include <bloom/document/graph.hpp>

#include <bloom/document/persisted_text.hpp>

#include <algorithm>
#include <array>
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

struct ExpectedParameterBinding final {
    std::string_view role;
    std::string_view schemaKey;
};

constexpr std::array kSolidSourceBindings{
    ExpectedParameterBinding{bloom::document::kSolidColorParameterRole,
                             bloom::document::kSolidColorParameterSchemaKey},
};
constexpr std::array kTextSourceBindings{
    ExpectedParameterBinding{bloom::document::kTextParameterRole,
                             bloom::document::kTextParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kTextSizeParameterRole,
                             bloom::document::kTextSizeParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kTextColorParameterRole,
                             bloom::document::kTextColorParameterSchemaKey},
};
constexpr std::array kLayerOutputBindings{
    ExpectedParameterBinding{bloom::document::kPositionParameterRole,
                             bloom::document::kPositionParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kAnchorParameterRole,
                             bloom::document::kAnchorParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kScaleParameterRole,
                             bloom::document::kScaleParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kRotationParameterRole,
                             bloom::document::kRotationParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kOpacityParameterRole,
                             bloom::document::kOpacityParameterSchemaKey},
    ExpectedParameterBinding{bloom::document::kBlendModeParameterRole,
                             bloom::document::kBlendModeParameterSchemaKey},
};

[[nodiscard]] std::span<const ExpectedParameterBinding>
expectedBindings(const bloom::document::NodeRecord& node) noexcept {
    using namespace bloom::document;
    if (node.typeId == kSolidSourceNodeType &&
        node.schemaVersion == kSolidSourceNodeSchemaVersion) {
        return kSolidSourceBindings;
    }
    if (node.typeId == kTextSourceNodeType && node.schemaVersion == kTextSourceNodeSchemaVersion) {
        return kTextSourceBindings;
    }
    if (node.typeId == kLayerOutputNodeType &&
        node.schemaVersion == kLayerOutputNodeSchemaVersion) {
        return kLayerOutputBindings;
    }
    return {};
}

void validateExpectedBindings(const bloom::document::NodeRecord& node,
                              const bloom::document::ParameterStore& parameters,
                              const std::string& path, bloom::document::ValidationResult& result) {
    using namespace bloom::document;
    for (const auto& expected : expectedBindings(node)) {
        const auto binding =
            std::find_if(node.parameters.begin(), node.parameters.end(),
                         [&](const auto& item) { return item.role == expected.role; });
        const auto bindingPath = path + ".parameters[" + std::string(expected.role) + "]";
        if (binding == node.parameters.end()) {
            result.add(ValidationCode::MissingReference, bindingPath,
                       "Known node schema requires this parameter binding");
            continue;
        }
        const auto* parameter = parameters.find(binding->parameterId);
        if (parameter != nullptr && parameter->schemaKey != expected.schemaKey) {
            result.add(ValidationCode::InvalidValue, bindingPath,
                       "Parameter binding uses the wrong schema for this node role");
        }
    }
}

// Whether this input port is backed by a parameter -- an operand socket rather than image
// transport. An operand's link is recorded as its PARAMETER's driver binding, never as an edge: one
// authored value has one durable record of where it comes from, so an edge and a binding can never
// disagree.
[[nodiscard]] bool isParameterSocket(const bloom::document::NodeRecord& node,
                                     const InputPortRef& destination) {
    const auto* fixed = std::get_if<NodeInputRef>(&destination);
    if (fixed == nullptr) {
        return false;
    }
    return std::ranges::any_of(node.parameters,
                               [&](const auto& binding) { return binding.role == fixed->port; });
}

[[nodiscard]] NodeId destinationNode(const InputPortRef& destination) {
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

[[nodiscard]] bool validDestination(const InputPortRef& destination) {
    return std::visit(
        [](const auto& input) {
            using Input = std::decay_t<decltype(input)>;
            if constexpr (std::is_same_v<Input, NodeInputRef>) {
                return input.nodeId.isValid() && bloom::document::isValidStructuralText(input.port);
            } else {
                return input.stackNodeId.isValid() && input.slotId.isValid() &&
                       bloom::document::isValidStructuralText(input.role);
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
    if (!node.id.isValid() || !isValidNamespacedIdentifier(node.typeId) ||
        node.schemaVersion == 0 || findNode(node.id) != nullptr) {
        return false;
    }

    std::unordered_set<std::string> roles;
    for (const auto& binding : node.parameters) {
        if (!isValidStructuralText(binding.role) || !binding.parameterId.isValid() ||
            !roles.insert(binding.role).second) {
            return false;
        }
    }

    nodes_.push_back(std::move(node));
    return true;
}

bool CanonicalGraph::addEdge(EdgeRecord edge, const NodeDefinitionRegistry& registry) {
    const auto* destination = findNode(destinationNode(edge.destination));
    if (destination != nullptr && isParameterSocket(*destination, edge.destination)) {
        return false;
    }
    const auto sourceKind = outputKind(edge.source, registry);
    const auto targetKind = inputKind(edge.destination, registry);
    // Nothing connects FROM a SINK. Task FIX1, item H made the composition Output one -- it
    // declares no output port at all -- and this is the rule that says so for every connect path at
    // once. It asks whether the registered definition has ANY output, not whether this particular
    // port is declared: an unknown port on a node that does have outputs is still the compiler's
    // UnknownPort diagnostic to report, which is a different mistake with a different message.
    if (const auto* source = findNode(edge.source.nodeId); source != nullptr) {
        const auto* definition = registry.find(source->typeId, source->schemaVersion);
        if (definition != nullptr && definition->outputs.empty()) {
            return false;
        }
    }
    // Task S7: equal kinds, or one of the whitelisted promotions. The ONE predicate every connect
    // path asks (node_definition_registry.hpp), so this, validate() below, ConnectPorts and the
    // compiler's edge check cannot disagree about which links exist.
    if (sourceKind && targetKind && !isAcceptedSocketConnection(*sourceKind, *targetKind)) {
        return false;
    }
    if (!edge.id.isValid() || !edge.source.nodeId.isValid() ||
        !isValidStructuralText(edge.source.port) || !validDestination(edge.destination)) {
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

const LayerOutputBoundary* CanonicalGraph::findLayer(const LayerId id) const noexcept {
    const auto found = std::ranges::find(layerOutputs_, id, &LayerOutputBoundary::layerId);
    return found == layerOutputs_.end() ? nullptr : &*found;
}
LayerOutputBoundary* CanonicalGraph::findLayer(const LayerId id) noexcept {
    return const_cast<LayerOutputBoundary*>(std::as_const(*this).findLayer(id));
}

bool CanonicalGraph::addLayerOutput(LayerOutputBoundary boundary) {
    if (!boundary.nodeId.isValid() || !boundary.layerId.isValid() ||
        !isValidHumanFacingName(boundary.name) || !isValidStructuralText(boundary.outputPort)) {
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

bool CanonicalGraph::eraseEdge(const EdgeId id) {
    return std::erase_if(edges_, [id](const auto& edge) { return edge.id == id; }) != 0;
}

bool CanonicalGraph::eraseNode(const NodeId id) {
    if (findNode(id) == nullptr)
        return false;
    std::vector<LayerSlotId> removedSlots;
    for (const auto& boundary : layerOutputs_) {
        if (boundary.nodeId != id)
            continue;
        for (const auto& entry : layerStack_.entries()) {
            if (entry.layerId == boundary.layerId)
                removedSlots.push_back(entry.slotId);
        }
    }
    for (const auto slot : removedSlots)
        (void)layerStack_.erase(slot);
    std::erase_if(edges_, [&](const auto& edge) {
        const auto* slot = std::get_if<LayerStackInputRef>(&edge.destination);
        return edge.source.nodeId == id || destinationNode(edge.destination) == id ||
               (slot && std::ranges::find(removedSlots, slot->slotId) != removedSlots.end());
    });
    std::erase_if(layerOutputs_, [id](const auto& boundary) { return boundary.nodeId == id; });
    std::erase_if(nodes_, [id](const auto& node) { return node.id == id; });
    return true;
}

bool CanonicalGraph::renameLayer(const LayerId id, std::string name) {
    if (!isValidHumanFacingName(name))
        return false;
    for (auto& boundary : layerOutputs_) {
        if (boundary.layerId == id) {
            boundary.name = std::move(name);
            return true;
        }
    }
    return false;
}

ValidationResult CanonicalGraph::validate(const ParameterStore& parameters,
                                          const NodeDefinitionRegistry& registry) const {
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
        validateNamespacedIdentifier(node.typeId, path + ".typeId", "Node type ID", result);
        if (node.schemaVersion == 0) {
            result.add(ValidationCode::InvalidValue, path + ".schemaVersion",
                       "Node schema version must not be zero");
        }

        std::unordered_set<std::string> roles;
        for (std::size_t bindingIndex = 0; bindingIndex < node.parameters.size(); ++bindingIndex) {
            const auto& binding = node.parameters[bindingIndex];
            const bool roleIsValid = isValidStructuralText(binding.role);
            const bool roleCanIdentifyPath = roleIsValid || binding.role.empty();
            const auto bindingIdentity =
                roleCanIdentifyPath ? binding.role : std::to_string(bindingIndex);
            auto bindingPath = path;
            bindingPath.append(".parameters[").append(bindingIdentity).push_back(']');
            validateStructuralText(binding.role, bindingPath, "Parameter binding role", result);
            if (roleIsValid && !roles.insert(binding.role).second) {
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
        validateExpectedBindings(node, parameters, path, result);
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
        validateStructuralText(boundary.outputPort, path + ".outputPort", "Layer Output port",
                               result);
        validateHumanFacingName(boundary.name, path + ".name", "Layer name", result);
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
        if (!edge.source.nodeId.isValid() || !isValidStructuralText(edge.source.port)) {
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

        const auto sourceKind = outputKind(edge.source, registry);
        const auto targetKind = inputKind(edge.destination, registry);
        const auto* sourceDefinition = [&]() -> const NodeDefinition* {
            const auto* sourceNode = findNode(edge.source.nodeId);
            return sourceNode == nullptr
                       ? nullptr
                       : registry.find(sourceNode->typeId, sourceNode->schemaVersion);
        }();
        if (sourceDefinition != nullptr && sourceDefinition->outputs.empty()) {
            result.add(ValidationCode::InvalidValue, path + ".source",
                       "This node is a sink and declares no output to connect from");
        } else if (sourceKind && targetKind &&
                   !isAcceptedSocketConnection(*sourceKind, *targetKind)) {
            result.add(ValidationCode::SocketKindMismatch, path,
                       "Connected socket kinds do not match");
        }
        const auto inputKey = destinationKey(edge.destination);
        if (!destinations.insert(inputKey).second) {
            result.add(ValidationCode::DuplicateInput, path + ".destination",
                       "More than one edge targets the same input");
        }

        const auto targetNodeId = destinationNode(edge.destination);
        const auto* targetNode = findNode(targetNodeId);
        if (targetNode == nullptr) {
            result.add(ValidationCode::MissingReference, path + ".destination",
                       "Edge destination references a missing node");
        } else if (isParameterSocket(*targetNode, edge.destination)) {
            result.add(
                ValidationCode::InvalidValue, path + ".destination",
                "An operand socket is linked by its parameter's driver binding, not an edge");
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

    // Driver bindings (task S7). A driven parameter is a reference from a value-graph node's output
    // into parameter-address space, so it is validated on exactly an edge's terms -- the target
    // must exist, the port must be declared, and the kinds must be connectable -- and it feeds the
    // SAME adjacency the cycle check below walks. A driver that closes a loop is therefore refused
    // by the one existing same-time cycle rule rather than by a second rule that could disagree
    // with it.
    for (const auto& node : nodes_) {
        const auto* definition = registry.find(node.typeId, node.schemaVersion);
        for (const auto& binding : node.parameters) {
            const auto* parameter = parameters.find(binding.parameterId);
            const auto* driver = parameter == nullptr
                                     ? nullptr
                                     : std::get_if<DriverBindingSource>(&parameter->source);
            if (driver == nullptr) {
                continue;
            }
            const auto path = "nodes[" + std::to_string(node.id.value()) + "].parameters[" +
                              binding.role + "].driver";
            if (findNode(driver->sourceNodeId) == nullptr) {
                result.add(ValidationCode::MissingReference, path + ".sourceNodeId",
                           "Driver binding references a missing node");
                continue;
            }
            const auto sourceKind =
                outputKind(OutputPortRef{driver->sourceNodeId, driver->outputPort}, registry);
            const InputPortDefinition* socket = nullptr;
            if (definition != nullptr) {
                const auto match =
                    std::ranges::find(definition->inputs, binding.role, &InputPortDefinition::name);
                socket = match == definition->inputs.end() ? nullptr : &*match;
            }
            if (definition != nullptr && socket == nullptr) {
                // An inline selector decides which kernel the plan compiles, so it cannot be
                // delivered per frame; it declares no socket, and a driver on it is not a link the
                // editor could have made.
                result.add(ValidationCode::InvalidValue, path,
                           "This parameter role has no linkable socket to drive");
            } else if (sourceKind.has_value() && socket != nullptr &&
                       !isAcceptedSocketConnection(*sourceKind, socket->valueKind)) {
                result.add(ValidationCode::SocketKindMismatch, path,
                           "Driver binding socket kinds do not match");
            }
            adjacency[driver->sourceNodeId].push_back(node.id);
            ++indegree[node.id];
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
    } else if (!compositionOutput_->nodeId.isValid() ||
               !isValidStructuralText(compositionOutput_->port)) {
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

// A reroute takes the kind of whatever feeds it, following a chain of reroutes to whatever feeds
// the first one (task FIX1, item I). An UNCONNECTED reroute has no kind: std::nullopt is what every
// kind check reads as "nothing to disagree with", so the first link into a fresh reroute is
// accepted whatever it carries and every link after it is checked against what the reroute now
// holds.
//
// `seen` stops a chain that closes on itself. Such a graph is refused by the acyclic rule anyway,
// but this function is also asked about DRAFT graphs mid-edit, where the cycle exists for one call.
std::optional<SocketValueKind>
CanonicalGraph::rerouteKind(const NodeId id, const NodeDefinitionRegistry& registry,
                            std::unordered_set<std::uint64_t>& seen) const {
    if (!seen.insert(id.value()).second) {
        return std::nullopt;
    }
    const auto incoming = std::ranges::find_if(edges_, [id](const auto& edge) {
        const auto* fixed = std::get_if<NodeInputRef>(&edge.destination);
        return fixed != nullptr && fixed->nodeId == id && fixed->port == kValuePortName;
    });
    if (incoming == edges_.end()) {
        return std::nullopt;
    }
    const auto* source = findNode(incoming->source.nodeId);
    if (source == nullptr) {
        return std::nullopt;
    }
    if (isRerouteNodeType(source->typeId)) {
        return rerouteKind(source->id, registry, seen);
    }
    const auto* definition = registry.find(source->typeId, source->schemaVersion);
    if (definition == nullptr) {
        return std::nullopt;
    }
    for (const auto& port : definition->outputs) {
        if (port.name == incoming->source.port) {
            return port.valueKind;
        }
    }
    return std::nullopt;
}

std::optional<SocketValueKind>
CanonicalGraph::rerouteKind(const NodeId id, const NodeDefinitionRegistry& registry) const {
    std::unordered_set<std::uint64_t> seen;
    return rerouteKind(id, registry, seen);
}

std::optional<SocketValueKind>
CanonicalGraph::outputKind(const OutputPortRef& output,
                           const NodeDefinitionRegistry& registry) const {
    const auto* node = findNode(output.nodeId);
    if (node != nullptr && isRerouteNodeType(node->typeId)) {
        return rerouteKind(output.nodeId, registry);
    }
    const auto* definition = node ? registry.find(node->typeId, node->schemaVersion) : nullptr;
    if (definition) {
        for (const auto& port : definition->outputs) {
            if (port.name == output.port)
                return port.valueKind;
        }
    }
    return std::nullopt;
}

std::optional<SocketValueKind>
CanonicalGraph::inputKind(const InputPortRef& input, const NodeDefinitionRegistry& registry) const {
    const auto* node = findNode(destinationNode(input));
    if (node != nullptr && isRerouteNodeType(node->typeId)) {
        return rerouteKind(node->id, registry);
    }
    const auto* definition = node ? registry.find(node->typeId, node->schemaVersion) : nullptr;
    if (definition == nullptr)
        return std::nullopt;
    if (const auto* fixed = std::get_if<NodeInputRef>(&input)) {
        for (const auto& port : definition->inputs) {
            if (port.name == fixed->port)
                return port.valueKind;
        }
    } else if (definition->layerSlotInput &&
               definition->layerSlotInput->role == std::get<LayerStackInputRef>(input).role) {
        return definition->layerSlotInput->valueKind;
    }
    return std::nullopt;
}

} // namespace bloom::document
