#include <bloom/runtime/snapshot_compiler.hpp>

#include "snapshot_compiler_support.hpp"

#include <bloom/document/animation.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace bloom;
using runtime::detail::destinationNode;
using runtime::detail::findParameterBinding;
using runtime::detail::hasValueKind;

using FixedInputKey = std::pair<document::NodeId, std::string>;
using LayerSlotInputKey = std::tuple<document::NodeId, document::LayerSlotId, std::string>;
using DiagnosticKey = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                 std::uint64_t, std::uint64_t, std::string, int>;

struct CompiledCurveTables final {
    std::vector<runtime::CompiledScalarCurve> scalar;
    std::vector<runtime::CompiledVec2Curve> vec2;
};

[[nodiscard]] DiagnosticKey diagnosticKey(const runtime::CompileDiagnostic& diagnostic) {
    const auto value = [](const auto& id) {
        return id.has_value() ? id->value() : std::uint64_t{0};
    };
    return {diagnostic.subject.compositionId.value(),
            value(diagnostic.subject.nodeId),
            value(diagnostic.subject.edgeId),
            value(diagnostic.subject.parameterId),
            value(diagnostic.subject.layerId),
            value(diagnostic.subject.layerSlotId),
            diagnostic.subject.field,
            static_cast<int>(diagnostic.code)};
}

class CompilePass final {
  public:
    CompilePass(const runtime::NodeDefinitionRegistry& registry,
                const runtime::SnapshotCompileRequest& request,
                const runtime::CancellationToken& cancellation,
                runtime::detail::CompileCheckpointObserver* checkpointObserver)
        : registry_(registry), request_(request), cancellation_(cancellation),
          checkpointObserver_(checkpointObserver) {}

    [[nodiscard]] runtime::SnapshotCompileResult run() {
        if (cancelled()) {
            return result(runtime::SnapshotCompileStatus::Cancelled);
        }
        if (!registry_.isFrozen()) {
            addFailure(runtime::CompileDiagnosticCode::RegistryNotFrozen, {},
                       "Node definition registry is not frozen",
                       "Compilation requires an immutable startup registry.");
            return finishWithoutPlan();
        }

        for (const auto& composition : request_.snapshot.project().compositions()) {
            if (cancelled()) {
                return result(runtime::SnapshotCompileStatus::Cancelled);
            }
            if (composition.id() == request_.compositionId) {
                composition_ = &composition;
                break;
            }
        }
        if (composition_ == nullptr) {
            addFailure(runtime::CompileDiagnosticCode::CompositionNotFound, {},
                       "Composition was not found",
                       "The requested composition does not exist in this document snapshot.");
            return finishWithoutPlan();
        }
        if (!indexSourceGraph() || !collectReachableGraph() || !indexReachableGraph() ||
            cancelled()) {
            return cancelled() ? result(runtime::SnapshotCompileStatus::Cancelled)
                               : finishWithoutPlan();
        }

        resolveDefinitions();
        validateCompositionOutput();
        validateEdges();
        validateInputs();
        validateParameters();
        validateParameterOverride();
        if (cancelled()) {
            return result(runtime::SnapshotCompileStatus::Cancelled);
        }
        if (hasFailure_ || hasUnsupported_) {
            return finishWithoutPlan();
        }

        const auto topologicalOrder = makeTopologicalOrder();
        if (!topologicalOrder.has_value()) {
            return finishWithoutPlan();
        }
        auto plan = lower(*topologicalOrder);
        if (cancelled()) {
            return result(runtime::SnapshotCompileStatus::Cancelled);
        }
        if (!plan) {
            return finishWithoutPlan();
        }

        auto diagnostics = takeDiagnostics();
        if (cancelled()) {
            return {
                .status = runtime::SnapshotCompileStatus::Cancelled, .plan = {}, .diagnostics = {}};
        }
        return {.status = runtime::SnapshotCompileStatus::Compiled,
                .plan = std::move(plan),
                .diagnostics = std::move(diagnostics)};
    }

  private:
    [[nodiscard]] bool indexSourceGraph() {
        for (const auto& node : composition_->graph().nodes()) {
            if (cancelled()) {
                return false;
            }
            nodes_.emplace(node.id, &node);
        }
        for (const auto& parameter : composition_->parameters().records()) {
            if (cancelled()) {
                return false;
            }
            parameters_.emplace(parameter.id, &parameter);
        }
        for (const auto& boundary : composition_->graph().layerOutputs()) {
            if (cancelled()) {
                return false;
            }
            layerOutputs_.emplace(boundary.nodeId, &boundary);
        }
        return true;
    }

    [[nodiscard]] const document::NodeRecord*
    findNode(const document::NodeId nodeId) const noexcept {
        const auto node = nodes_.find(nodeId);
        return node == nodes_.end() ? nullptr : node->second;
    }

    [[nodiscard]] const document::ParameterRecord*
    findParameter(const document::ParameterId parameterId) const noexcept {
        const auto parameter = parameters_.find(parameterId);
        return parameter == parameters_.end() ? nullptr : parameter->second;
    }

    [[nodiscard]] bool cancelled(const runtime::detail::CompileCheckpointPhase phase =
                                     runtime::detail::CompileCheckpointPhase::General) const {
        if (checkpointObserver_ != nullptr) {
            checkpointObserver_->checkpoint(phase);
        }
        return cancellation_.isCancellationRequested();
    }

    [[nodiscard]] runtime::CompileSubject subject(const document::NodeId nodeId = {},
                                                  std::string field = {}) const {
        return {.compositionId = request_.compositionId,
                .nodeId = nodeId.isValid() ? std::optional(nodeId) : std::nullopt,
                .edgeId = std::nullopt,
                .parameterId = std::nullopt,
                .layerId = std::nullopt,
                .layerSlotId = std::nullopt,
                .field = std::move(field)};
    }

    void addFailure(const runtime::CompileDiagnosticCode code,
                    runtime::CompileSubject diagnosticSubject, std::string summary,
                    std::string detail) {
        diagnosticSubject.compositionId = request_.compositionId;
        runtime::CompileDiagnostic diagnostic{code, runtime::DiagnosticSeverity::Error,
                                              std::move(diagnosticSubject), std::move(summary),
                                              std::move(detail)};
        diagnostics_.emplace(diagnosticKey(diagnostic), std::move(diagnostic));
        hasFailure_ = true;
    }

    void addUnsupported(const runtime::CompileDiagnosticCode code,
                        runtime::CompileSubject diagnosticSubject, std::string summary,
                        std::string detail) {
        diagnosticSubject.compositionId = request_.compositionId;
        runtime::CompileDiagnostic diagnostic{code, runtime::DiagnosticSeverity::Error,
                                              std::move(diagnosticSubject), std::move(summary),
                                              std::move(detail)};
        diagnostics_.emplace(diagnosticKey(diagnostic), std::move(diagnostic));
        hasUnsupported_ = true;
    }

    [[nodiscard]] bool collectReachableGraph() {
        const auto& graph = composition_->graph();
        if (!graph.compositionOutput().has_value()) {
            addFailure(runtime::CompileDiagnosticCode::InvalidCompositionOutput, {},
                       "Composition output is missing",
                       "A published composition must provide one explicit output endpoint.");
            return false;
        }

        const auto rootId = graph.compositionOutput()->nodeId;
        if (findNode(rootId) == nullptr) {
            addFailure(runtime::CompileDiagnosticCode::TopologyInvariant,
                       subject(rootId, "compositionOutput.nodeId"),
                       "Composition output references a missing node",
                       "The published graph violates its document topology invariant.");
            return false;
        }

        std::unordered_map<document::NodeId, std::vector<const document::EdgeRecord*>> incoming;
        for (const auto& edge : graph.edges()) {
            if (cancelled(runtime::detail::CompileCheckpointPhase::IncomingEdgeIndex)) {
                return false;
            }
            incoming[destinationNode(edge.destination)].push_back(&edge);
        }

        std::vector<document::NodeId> pending{rootId};
        std::unordered_set<document::NodeId> visited;
        std::map<document::NodeId, const document::NodeRecord*> reachableNodes;
        std::map<document::EdgeId, const document::EdgeRecord*> reachableEdges;
        while (!pending.empty()) {
            if (cancelled()) {
                return false;
            }
            const auto nodeId = pending.back();
            pending.pop_back();
            if (!visited.insert(nodeId).second) {
                continue;
            }
            const auto* node = findNode(nodeId);
            if (node == nullptr) {
                addFailure(runtime::CompileDiagnosticCode::TopologyInvariant,
                           subject(nodeId, "edges"), "Reachable edge references a missing node",
                           "The published graph violates its document topology invariant.");
                continue;
            }
            reachableNodes.emplace(nodeId, node);
            if (const auto iterator = incoming.find(nodeId); iterator != incoming.end()) {
                for (const auto* edge : iterator->second) {
                    if (cancelled()) {
                        return false;
                    }
                    reachableEdges.emplace(edge->id, edge);
                    pending.push_back(edge->source.nodeId);
                }
            }
        }

        reachableNodes_.reserve(reachableNodes.size());
        for (const auto& [nodeId, node] : reachableNodes) {
            static_cast<void>(nodeId);
            if (cancelled()) {
                return false;
            }
            reachableNodes_.push_back(node);
        }
        reachableEdges_.reserve(reachableEdges.size());
        for (const auto& [edgeId, edge] : reachableEdges) {
            static_cast<void>(edgeId);
            if (cancelled()) {
                return false;
            }
            reachableEdges_.push_back(edge);
        }
        return !hasFailure_;
    }

    [[nodiscard]] bool indexReachableGraph() {
        for (const auto* edge : reachableEdges_) {
            if (cancelled()) {
                return false;
            }
            std::visit(
                [&](const auto& input) {
                    using Input = std::decay_t<decltype(input)>;
                    if constexpr (std::is_same_v<Input, document::NodeInputRef>) {
                        fixedInputEdges_.emplace(FixedInputKey{input.nodeId, input.port}, edge);
                    } else {
                        layerSlotInputEdges_.emplace(
                            LayerSlotInputKey{input.stackNodeId, input.slotId, input.role}, edge);
                    }
                },
                edge->destination);
        }
        return true;
    }

    [[nodiscard]] const document::EdgeRecord* fixedInputEdge(const document::NodeId nodeId,
                                                             const std::string_view port) const {
        const auto edge = fixedInputEdges_.find(FixedInputKey{nodeId, std::string(port)});
        return edge == fixedInputEdges_.end() ? nullptr : edge->second;
    }

    [[nodiscard]] const document::EdgeRecord*
    layerSlotInputEdge(const document::NodeId nodeId, const document::LayerSlotId slotId,
                       const std::string_view role) const {
        const auto edge =
            layerSlotInputEdges_.find(LayerSlotInputKey{nodeId, slotId, std::string(role)});
        return edge == layerSlotInputEdges_.end() ? nullptr : edge->second;
    }

    [[nodiscard]] const runtime::InputPortDefinition*
    findInput(const runtime::NodeDefinition& definition, const std::string_view name) const {
        for (const auto& input : definition.inputs) {
            if (cancelled()) {
                return nullptr;
            }
            if (input.name == name) {
                return &input;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const runtime::OutputPortDefinition*
    findOutput(const runtime::NodeDefinition& definition, const std::string_view name) const {
        for (const auto& output : definition.outputs) {
            if (cancelled()) {
                return nullptr;
            }
            if (output.name == name) {
                return &output;
            }
        }
        return nullptr;
    }

    void resolveDefinitions() {
        for (const auto* node : reachableNodes_) {
            if (cancelled(runtime::detail::CompileCheckpointPhase::DefinitionResolution)) {
                return;
            }
            const auto* definition = registry_.find(node->typeId, node->schemaVersion);
            if (cancelled(runtime::detail::CompileCheckpointPhase::DefinitionResolution)) {
                return;
            }
            if (definition == nullptr) {
                if (registry_.containsType(node->typeId)) {
                    addUnsupported(runtime::CompileDiagnosticCode::UnsupportedNodeVersion,
                                   subject(node->id, "schemaVersion"),
                                   "Node schema version is unsupported",
                                   "No registered evaluator definition matches this node version.");
                } else {
                    addUnsupported(runtime::CompileDiagnosticCode::UnknownNodeType,
                                   subject(node->id, "typeId"), "Node type is unavailable",
                                   "No registered node definition matches this node type.");
                }
                continue;
            }
            definitions_.emplace(node->id, definition);
            if (definition->lowering == runtime::NodeLoweringKind::Unsupported) {
                addUnsupported(
                    runtime::CompileDiagnosticCode::UnsupportedNode, subject(node->id, "typeId"),
                    "Node cannot be evaluated yet",
                    "This registered node type has no compiled operation in this build.");
            }
        }
    }

    void validateCompositionOutput() {
        const auto& endpoint = *composition_->graph().compositionOutput();
        const auto* endpointNode = findNode(endpoint.nodeId);
        if (endpointNode == nullptr ||
            endpointNode->typeId != document::kCompositionOutputNodeType ||
            endpoint.port != document::kCompositionOutputOutputPort) {
            addFailure(runtime::CompileDiagnosticCode::InvalidCompositionOutput,
                       subject(endpoint.nodeId, "compositionOutput"),
                       "Composition output endpoint is invalid",
                       "The endpoint must name a Composition Output image port.");
            return;
        }
        const auto definition = definitions_.find(endpoint.nodeId);
        if (definition != definitions_.end() &&
            (definition->second->lowering != runtime::NodeLoweringKind::CompositionOutput ||
             findOutput(*definition->second, endpoint.port) == nullptr)) {
            addFailure(runtime::CompileDiagnosticCode::InvalidCompositionOutput,
                       subject(endpoint.nodeId, "compositionOutput"),
                       "Composition output endpoint is invalid",
                       "The endpoint must name a registered Composition Output image port.");
        }
    }

    void validateEdges() {
        for (const auto* edge : reachableEdges_) {
            if (cancelled()) {
                return;
            }
            const auto sourceDefinition = definitions_.find(edge->source.nodeId);
            const auto destinationDefinition =
                definitions_.find(destinationNode(edge->destination));
            if (sourceDefinition == definitions_.end() ||
                destinationDefinition == definitions_.end()) {
                continue;
            }

            const auto* output = findOutput(*sourceDefinition->second, edge->source.port);
            if (output == nullptr) {
                auto diagnosticSubject = subject(edge->source.nodeId, "source.port");
                diagnosticSubject.edgeId = edge->id;
                addFailure(runtime::CompileDiagnosticCode::UnknownPort,
                           std::move(diagnosticSubject), "Edge source port is unknown",
                           "The source port is not declared by its registered node definition.");
                continue;
            }

            const runtime::SocketValueKind* inputKind = nullptr;
            std::visit(
                [&](const auto& input) {
                    using Input = std::decay_t<decltype(input)>;
                    if constexpr (std::is_same_v<Input, document::NodeInputRef>) {
                        if (const auto* definition =
                                findInput(*destinationDefinition->second, input.port)) {
                            inputKind = &definition->valueKind;
                        }
                    } else if (destinationDefinition->second->layerSlotInput.has_value() &&
                               destinationDefinition->second->layerSlotInput->role == input.role) {
                        inputKind = &destinationDefinition->second->layerSlotInput->valueKind;
                    }
                },
                edge->destination);

            if (inputKind == nullptr) {
                auto diagnosticSubject =
                    subject(destinationNode(edge->destination), "destination.port");
                diagnosticSubject.edgeId = edge->id;
                addFailure(runtime::CompileDiagnosticCode::UnknownPort,
                           std::move(diagnosticSubject), "Edge destination port is unknown",
                           "The destination is not declared by its registered node definition.");
            } else if (*inputKind != output->valueKind) {
                auto diagnosticSubject =
                    subject(destinationNode(edge->destination), "destination.port");
                diagnosticSubject.edgeId = edge->id;
                addFailure(runtime::CompileDiagnosticCode::PortTypeMismatch,
                           std::move(diagnosticSubject), "Connected port types do not match",
                           "The edge connects incompatible typed sockets.");
            }
        }
    }

    void validateInputs() {
        const auto& graph = composition_->graph();
        for (const auto* node : reachableNodes_) {
            if (cancelled()) {
                return;
            }
            const auto definition = definitions_.find(node->id);
            if (definition == definitions_.end()) {
                continue;
            }
            for (const auto& input : definition->second->inputs) {
                if (cancelled()) {
                    return;
                }
                if (!input.required) {
                    continue;
                }
                if (fixedInputEdge(node->id, input.name) == nullptr) {
                    addFailure(runtime::CompileDiagnosticCode::MissingInput,
                               subject(node->id, "input." + input.name),
                               "Required node input is not connected",
                               "The registered node definition requires this input.");
                }
            }

            if (!definition->second->layerSlotInput.has_value() ||
                !definition->second->layerSlotInput->requiredPerSlot) {
                continue;
            }
            for (const auto& entry : graph.layerStack().entries()) {
                if (cancelled()) {
                    return;
                }
                if (layerSlotInputEdge(node->id, entry.slotId,
                                       definition->second->layerSlotInput->role) == nullptr) {
                    auto diagnosticSubject = subject(node->id, "layerSlotInput");
                    diagnosticSubject.layerId = entry.layerId;
                    diagnosticSubject.layerSlotId = entry.slotId;
                    addFailure(runtime::CompileDiagnosticCode::MissingInput,
                               std::move(diagnosticSubject),
                               "Layer Stack slot has no content input",
                               "Every visible stack slot requires one typed content connection.");
                }
            }
        }
    }

    void validateParameters() {
        for (const auto* node : reachableNodes_) {
            if (cancelled()) {
                return;
            }
            const auto definition = definitions_.find(node->id);
            if (definition == definitions_.end()) {
                continue;
            }
            std::unordered_set<std::string_view> declaredRoles;
            for (const auto& parameterDefinition : definition->second->parameters) {
                if (cancelled()) {
                    return;
                }
                declaredRoles.insert(parameterDefinition.role);
            }
            std::unordered_map<std::string_view, const document::ParameterBinding*> bindings;
            for (const auto& binding : node->parameters) {
                if (cancelled()) {
                    return;
                }
                bindings.emplace(binding.role, &binding);
                if (!declaredRoles.contains(binding.role)) {
                    auto diagnosticSubject = subject(node->id, "parameter." + binding.role);
                    diagnosticSubject.parameterId = binding.parameterId;
                    addFailure(runtime::CompileDiagnosticCode::UnexpectedParameter,
                               std::move(diagnosticSubject),
                               "Node has an unexpected parameter binding",
                               "The binding role is not declared by the registered definition.");
                }
            }
            for (const auto& parameterDefinition : definition->second->parameters) {
                if (cancelled()) {
                    return;
                }
                const auto binding = bindings.find(parameterDefinition.role);
                validateParameter(*node, parameterDefinition,
                                  binding == bindings.end() ? nullptr : binding->second);
            }
        }
    }

    void validateParameterOverride() {
        if (!request_.parameterOverride.has_value()) {
            return;
        }
        const auto& parameterOverride = *request_.parameterOverride;
        auto diagnosticSubject = subject({}, "parameterOverride");
        diagnosticSubject.parameterId = parameterOverride.parameterId;
        const auto reject = [&](const runtime::CompileDiagnosticCode code, std::string summary,
                                std::string detail) {
            auto overrideSubject = diagnosticSubject;
            addFailure(code, std::move(overrideSubject), std::move(summary), std::move(detail));
        };

        if (parameterOverride.sourceRevision != request_.snapshot.revision()) {
            reject(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                   "Parameter override revision does not match the snapshot",
                   "Recreate the interaction from the current immutable snapshot.");
            return;
        }
        const auto* parameter = findParameter(parameterOverride.parameterId);
        if (parameter == nullptr) {
            reject(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                   "Parameter override target does not exist",
                   "The target parameter is absent from the requested composition.");
            return;
        }

        const document::NodeRecord* ownerNode = nullptr;
        const runtime::NodeDefinition* ownerDefinition = nullptr;
        const runtime::ParameterDefinition* parameterDefinition = nullptr;
        for (const auto* node : reachableNodes_) {
            if (cancelled()) {
                return;
            }
            const auto binding = std::ranges::find(node->parameters, parameterOverride.parameterId,
                                                   &document::ParameterBinding::parameterId);
            if (binding == node->parameters.end()) {
                continue;
            }
            if (ownerNode != nullptr) {
                reject(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                       "Parameter override target has multiple reachable owners",
                       "Version-one parameters must have one canonical node binding.");
                return;
            }
            const auto definition = definitions_.find(node->id);
            if (definition == definitions_.end()) {
                return;
            }
            const auto declaredParameter = std::ranges::find(
                definition->second->parameters, binding->role, &runtime::ParameterDefinition::role);
            if (declaredParameter == definition->second->parameters.end()) {
                return;
            }
            ownerNode = node;
            ownerDefinition = definition->second;
            parameterDefinition = &*declaredParameter;
            diagnosticSubject.nodeId = node->id;
        }
        if (ownerNode == nullptr || ownerDefinition == nullptr || parameterDefinition == nullptr) {
            reject(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                   "Parameter override target is not reachable",
                   "Overrides may affect only parameters on the requested output path.");
            return;
        }
        const bool isPosition =
            ownerDefinition->lowering == runtime::NodeLoweringKind::LayerOutput &&
            parameterDefinition->role == document::kPositionParameterRole &&
            parameterDefinition->schemaKey == document::kPositionParameterSchemaKey;
        const bool isOpacity =
            ownerDefinition->lowering == runtime::NodeLoweringKind::LayerOutput &&
            parameterDefinition->role == document::kOpacityParameterRole &&
            parameterDefinition->schemaKey == document::kOpacityParameterSchemaKey;
        const auto* scalar = std::get_if<double>(&parameterOverride.value);
        const auto* vector = std::get_if<document::Vec2d>(&parameterOverride.value);
        const bool kindMatches =
            (isPosition && parameterDefinition->valueKind == runtime::ParameterValueKind::Vec2d &&
             vector != nullptr) ||
            (isOpacity && parameterDefinition->valueKind == runtime::ParameterValueKind::Float64 &&
             scalar != nullptr);
        if (parameter->schemaKey != parameterDefinition->schemaKey || !kindMatches) {
            reject(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                   "Parameter override type does not match its target",
                   "Version one accepts only typed Layer Output position and opacity overrides.");
            return;
        }
        if ((vector != nullptr && (!std::isfinite(vector->x) || !std::isfinite(vector->y))) ||
            (scalar != nullptr && (!std::isfinite(*scalar) || *scalar < 0.0 || *scalar > 1.0))) {
            reject(runtime::CompileDiagnosticCode::InvalidParameterOverride,
                   "Parameter override value is outside its schema domain",
                   "Position must be finite and opacity must be finite within zero and one.");
            return;
        }
        if (std::holds_alternative<document::DriverBindingSource>(parameter->source)) {
            auto overrideSubject = diagnosticSubject;
            addUnsupported(runtime::CompileDiagnosticCode::UnsupportedParameterOverride,
                           std::move(overrideSubject),
                           "Driven parameters cannot be overridden interactively",
                           "Disconnect or explicitly transition the driver before editing.");
        }
    }

    void validateParameter(const document::NodeRecord& node,
                           const runtime::ParameterDefinition& definition,
                           const document::ParameterBinding* binding) {
        if (binding == nullptr) {
            if (definition.required) {
                addFailure(runtime::CompileDiagnosticCode::MissingParameter,
                           subject(node.id, "parameter." + definition.role),
                           "Required parameter binding is missing",
                           "The registered node definition requires this parameter role.");
            }
            return;
        }
        const auto* parameter = findParameter(binding->parameterId);
        if (parameter == nullptr) {
            auto diagnosticSubject = subject(node.id, "parameter." + definition.role);
            diagnosticSubject.parameterId = binding->parameterId;
            addFailure(runtime::CompileDiagnosticCode::TopologyInvariant,
                       std::move(diagnosticSubject), "Parameter record is missing",
                       "The published graph violates its document topology invariant.");
            return;
        }
        if (parameter->schemaKey != definition.schemaKey) {
            auto diagnosticSubject = subject(node.id, "parameter." + definition.role);
            diagnosticSubject.parameterId = parameter->id;
            addFailure(runtime::CompileDiagnosticCode::ParameterSchemaMismatch,
                       std::move(diagnosticSubject), "Parameter schema does not match",
                       "The parameter record uses a different schema than the node role.");
            return;
        }
        if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source)) {
            if (!hasValueKind(constant->value, definition.valueKind)) {
                auto diagnosticSubject = subject(node.id, "parameter." + definition.role);
                diagnosticSubject.parameterId = parameter->id;
                addFailure(runtime::CompileDiagnosticCode::ParameterValueKindMismatch,
                           std::move(diagnosticSubject), "Parameter value type does not match",
                           "The constant value has a different type than the registered schema.");
            }
            return;
        }

        const auto* animation = std::get_if<document::AnimationCurveSource>(&parameter->source);
        if (animation != nullptr && definition.supportsAnimation) {
            const auto* curve = composition_->animationCurves().find(animation->curveId);
            const bool kindMatches =
                curve != nullptr &&
                ((definition.valueKind == runtime::ParameterValueKind::Float64 &&
                  std::holds_alternative<document::ScalarAnimationCurve>(*curve)) ||
                 (definition.valueKind == runtime::ParameterValueKind::Vec2d &&
                  std::holds_alternative<document::Vec2AnimationCurve>(*curve)));
            if (!kindMatches) {
                auto diagnosticSubject = subject(node.id, "parameter." + definition.role);
                diagnosticSubject.parameterId = parameter->id;
                addFailure(runtime::CompileDiagnosticCode::ParameterValueKindMismatch,
                           std::move(diagnosticSubject), "Animation curve type does not match",
                           "The curve value kind differs from the registered parameter schema.");
            }
            return;
        }

        {
            auto diagnosticSubject = subject(node.id, "parameter." + definition.role);
            diagnosticSubject.parameterId = parameter->id;
            addUnsupported(runtime::CompileDiagnosticCode::UnsupportedParameterSource,
                           std::move(diagnosticSubject), "Parameter source cannot be evaluated yet",
                           animation != nullptr
                               ? "This registered parameter does not support animation."
                               : "Driver evaluation is not implemented in this semantics version.");
        }
    }

#include "snapshot_compiler_lowering.ipp"

    void addTopologyFailure(const document::NodeId nodeId, std::string detail) {
        addFailure(runtime::CompileDiagnosticCode::TopologyInvariant, subject(nodeId, "graph"),
                   "Validated graph could not be lowered", std::move(detail));
    }

    [[nodiscard]] std::vector<runtime::CompileDiagnostic> takeDiagnostics() {
        std::vector<runtime::CompileDiagnostic> result;
        result.reserve(diagnostics_.size());
        for (auto& [key, diagnostic] : diagnostics_) {
            static_cast<void>(key);
            if (cancelled()) {
                return {};
            }
            result.push_back(std::move(diagnostic));
        }
        return result;
    }

    [[nodiscard]] runtime::SnapshotCompileResult
    result(const runtime::SnapshotCompileStatus status) {
        if (status == runtime::SnapshotCompileStatus::Cancelled) {
            return {.status = status, .plan = {}, .diagnostics = {}};
        }
        auto diagnostics = takeDiagnostics();
        if (cancelled()) {
            return {
                .status = runtime::SnapshotCompileStatus::Cancelled, .plan = {}, .diagnostics = {}};
        }
        return {.status = status, .plan = {}, .diagnostics = std::move(diagnostics)};
    }

    [[nodiscard]] runtime::SnapshotCompileResult finishWithoutPlan() {
        return result(hasFailure_ ? runtime::SnapshotCompileStatus::Failed
                                  : runtime::SnapshotCompileStatus::Unsupported);
    }

    const runtime::NodeDefinitionRegistry& registry_;
    const runtime::SnapshotCompileRequest& request_;
    const runtime::CancellationToken& cancellation_;
    runtime::detail::CompileCheckpointObserver* checkpointObserver_ = nullptr;
    const document::Composition* composition_ = nullptr;
    std::unordered_map<document::NodeId, const document::NodeRecord*> nodes_;
    std::unordered_map<document::ParameterId, const document::ParameterRecord*> parameters_;
    std::vector<const document::NodeRecord*> reachableNodes_;
    std::vector<const document::EdgeRecord*> reachableEdges_;
    std::map<FixedInputKey, const document::EdgeRecord*> fixedInputEdges_;
    std::map<LayerSlotInputKey, const document::EdgeRecord*> layerSlotInputEdges_;
    std::map<document::NodeId, const document::LayerOutputBoundary*> layerOutputs_;
    std::unordered_map<document::NodeId, const runtime::NodeDefinition*> definitions_;
    std::unordered_map<document::AnimationCurveId, runtime::ScalarCurveIndex> scalarCurveIndices_;
    std::unordered_map<document::AnimationCurveId, runtime::Vec2CurveIndex> vec2CurveIndices_;
    std::multimap<DiagnosticKey, runtime::CompileDiagnostic> diagnostics_;
    bool hasFailure_ = false;
    bool hasUnsupported_ = false;
};

} // namespace

namespace bloom::runtime {

SnapshotCompileResult detail::compileSnapshot(
    const NodeDefinitionRegistry& registry, const SnapshotCompileRequest& request,
    const CancellationToken& cancellation, detail::CompileCheckpointObserver* checkpointObserver) {
    return CompilePass(registry, request, cancellation, checkpointObserver).run();
}

SnapshotCompileResult SnapshotCompiler::compile(const SnapshotCompileRequest& request,
                                                const CancellationToken& cancellation) const {
    return detail::compileSnapshot(registry_, request, cancellation, nullptr);
}

} // namespace bloom::runtime
