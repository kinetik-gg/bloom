#pragma once

#include <bloom/commands/node_operations.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <type_traits>

namespace bloom::commands::detail {
inline OperationResult invalidTarget() {
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Composition or node target does not exist");
}
inline OperationResult exhaustedIds() {
    return OperationResult::rejected(OperationIssueCode::Unsupported,
                                     "Document ID space is exhausted");
}
inline bool finite(const document::Vec2d value) {
    return std::isfinite(value.x) && std::isfinite(value.y);
}
inline document::NodeId destinationNode(const document::InputPortRef& input) {
    return std::visit(
        [](const auto& port) {
            if constexpr (std::is_same_v<std::decay_t<decltype(port)>, document::NodeInputRef>)
                return port.nodeId;
            else
                return port.stackNodeId;
        },
        input);
}
inline const document::EdgeRecord* inputEdge(const document::CanonicalGraph& graph,
                                             const document::InputPortRef& input) {
    const auto edge = std::ranges::find(graph.edges(), input, &document::EdgeRecord::destination);
    return edge == graph.edges().end() ? nullptr : &*edge;
}
inline bool protectedNode(const document::CanonicalGraph& graph, const document::NodeId id) {
    const auto* node = graph.findNode(id);
    return id == graph.layerStack().nodeId() ||
           (graph.compositionOutput() && graph.compositionOutput()->nodeId == id) ||
           (node && (node->typeId == document::kLayerStackNodeType ||
                     node->typeId == document::kCompositionOutputNodeType));
}
inline document::NodeLayoutRecord layoutFor(const document::Composition& composition,
                                            const document::NodeId id) {
    const auto record = composition.nodeLayout().find(id);
    if (record != composition.nodeLayout().end())
        return record->second;
    return document::defaultNodeLayout(composition.graph().nodes()).at(id);
}
inline std::optional<OperationResult> validateGraph(
    const document::Composition& composition,
    const document::NodeDefinitionRegistry& registry = document::builtInNodeDefinitions()) {
    const auto validation = composition.graph().validate(composition.parameters(), registry);
    for (const auto& issue : validation.issues()) {
        if (issue.severity == document::ValidationSeverity::Warning)
            continue;
        const auto code = issue.code == document::ValidationCode::GraphCycle
                              ? OperationIssueCode::GraphCycle
                          : issue.code == document::ValidationCode::SocketKindMismatch
                              ? OperationIssueCode::SocketKindMismatch
                              : OperationIssueCode::InvalidValue;
        return OperationResult::rejected(code, issue.message);
    }
    return std::nullopt;
}
inline void eraseOrphanedParameters(document::Composition& composition,
                                    const std::set<document::ParameterId>& candidates) {
    for (const auto id : candidates) {
        const bool referenced =
            std::ranges::any_of(composition.graph().nodes(), [id](const auto& node) {
                return std::ranges::any_of(node.parameters, [id](const auto& binding) {
                    return binding.parameterId == id;
                });
            });
        if (referenced)
            continue;
        const auto* parameter = composition.parameters().find(id);
        const auto* animation =
            parameter ? std::get_if<document::AnimationCurveSource>(&parameter->source) : nullptr;
        const auto curve = animation ? std::optional(animation->curveId) : std::nullopt;
        (void)composition.parameters().erase(id);
        if (curve &&
            std::ranges::none_of(composition.parameters().records(), [&](const auto& record) {
                const auto* source = std::get_if<document::AnimationCurveSource>(&record.source);
                return source && source->curveId == *curve;
            }))
            (void)composition.animationCurves().erase(*curve);
    }
}
} // namespace bloom::commands::detail
