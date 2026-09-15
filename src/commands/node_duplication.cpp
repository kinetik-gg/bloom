#include <bloom/commands/node_operations.hpp>

#include "node_operation_support.hpp"

#include <algorithm>
#include <type_traits>

namespace bloom::commands {
namespace {
template <typename Curve, typename Callback>
void forEachStoredKeyframe(Curve& curve, Callback&& callback) {
    using CurveType = std::remove_cvref_t<Curve>;
    if constexpr (std::is_same_v<CurveType, document::ScalarAnimationCurve>) {
        for (auto& keyframe : curve.keyframes)
            callback(keyframe);
    } else {
        if (std::ranges::all_of(curve.components, [](const auto& component) {
                return component.keyframes.empty();
            })) {
            for (auto& keyframe : curve.keyframes)
                callback(keyframe);
        } else {
            for (auto& component : curve.components)
                for (auto& keyframe : component.keyframes)
                    callback(keyframe);
        }
    }
}

std::optional<document::AnimationCurveId> copyCurve(document::Draft& draft,
                                                    document::Composition& composition,
                                                    const document::AnimationCurveId original) {
    const auto* record = composition.animationCurves().find(original);
    if (!record)
        return std::nullopt;
    auto copy = *record;
    const auto id = draft.ids().allocateAnimationCurve();
    if (!id)
        return std::nullopt;
    const bool allocated = std::visit(
        [&](auto& curve) {
            curve.id = *id;
            bool allAllocated = true;
            forEachStoredKeyframe(curve, [&](auto& key) {
                const auto keyId = draft.ids().allocateKeyframe();
                if (!keyId) {
                    allAllocated = false;
                    return;
                }
                key.id = *keyId;
            });
            return allAllocated;
        },
        copy);
    const auto copiedCurveId = document::animationCurveId(copy);
    if (!allocated || !composition.animationCurves().insert(std::move(copy)))
        return std::nullopt;
    static_cast<void>(
        composition.animationCurves().synchronizeCompatibilityProjection(copiedCurveId));
    return id;
}
} // namespace

std::string_view DuplicateNodes::typeId() const noexcept { return "bloom.node.duplicate"; }
OperationResult DuplicateNodes::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (!composition)
        return detail::invalidTarget();
    if (!detail::finite(offset_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Duplicate offset must be finite");
    if (nodes_.empty())
        return OperationResult::noChange();
    const auto original = *composition;
    for (const auto id : nodes_) {
        const auto* node = original.graph().findNode(id);
        if (!node)
            return detail::invalidTarget();
        if (node->typeId == document::kCompositionOutputNodeType)
            return OperationResult::rejected(OperationIssueCode::Unsupported,
                                             "Output cannot be duplicated");
        for (const auto& binding : node->parameters) {
            const auto* parameter = original.parameters().find(binding.parameterId);
            if (!parameter)
                return detail::invalidTarget();
            if (std::holds_alternative<document::DriverBindingSource>(parameter->source) &&
                node->typeId != document::kLayerOutputNodeType)
                return OperationResult::rejected(OperationIssueCode::Unsupported,
                                                 "Driven parameters cannot be deeply duplicated "
                                                 "until driver records are implemented");
        }
    }
    std::map<document::NodeId, document::NodeId> nodeIds;
    std::map<document::ParameterId, document::ParameterId> parameterIds;
    std::map<document::AnimationCurveId, document::AnimationCurveId> curveIds;
    std::map<document::LayerId, document::LayerId> layerIds;
    std::vector<OperationOutput> outputs;
    auto& graph = composition->graph();
    for (const auto id : nodes_) {
        auto node = *original.graph().findNode(id);
        const auto newId = draft.ids().allocateNode();
        if (!newId)
            return detail::exhaustedIds();
        nodeIds.emplace(id, *newId);
        node.id = *newId;
        for (auto& binding : node.parameters) {
            const auto oldId = binding.parameterId;
            if (!parameterIds.contains(oldId)) {
                auto parameter = *original.parameters().find(oldId);
                const auto parameterId = draft.ids().allocateParameter();
                if (!parameterId)
                    return detail::exhaustedIds();
                parameter.id = *parameterId;
                if (auto* source = std::get_if<document::AnimationCurveSource>(&parameter.source)) {
                    if (!curveIds.contains(source->curveId)) {
                        const auto curveId = copyCurve(draft, *composition, source->curveId);
                        if (!curveId)
                            return detail::exhaustedIds();
                        curveIds.emplace(source->curveId, *curveId);
                        outputs.push_back(
                            {"curve." + std::to_string(source->curveId.value()), *curveId});
                    }
                    source->curveId = curveIds.at(source->curveId);
                }
                if (!composition->parameters().insert(std::move(parameter)))
                    return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                     "Duplicated parameter could not be inserted");
                parameterIds.emplace(oldId, *parameterId);
                outputs.push_back({"parameter." + std::to_string(oldId.value()), *parameterId});
            }
            binding.parameterId = parameterIds.at(oldId);
        }
        if (!graph.addNode(std::move(node)))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Duplicated node could not be inserted");
        auto layout = detail::layoutFor(original, id);
        layout.position.x += offset_.x;
        layout.position.y += offset_.y;
        if (!detail::finite(layout.position))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Duplicate position overflowed");
        composition->nodeLayout()[*newId] = layout;
        outputs.push_back({"node." + std::to_string(id.value()), *newId});
    }
    for (const auto& boundary : original.graph().layerOutputs()) {
        if (!nodes_.contains(boundary.nodeId))
            continue;
        const auto layerId = draft.ids().allocateLayer();
        if (!layerId)
            return detail::exhaustedIds();
        auto copy = boundary;
        copy.nodeId = nodeIds.at(boundary.nodeId);
        copy.layerId = *layerId;
        copy.name += " copy";
        if (!graph.addLayerOutput(std::move(copy)))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Duplicated layer name or boundary is invalid");
        layerIds.emplace(boundary.layerId, *layerId);
        outputs.push_back({"layer." + std::to_string(boundary.layerId.value()), *layerId});
    }
    for (const auto& stack : original.graph().merges()) {
        const bool copyStack = nodeIds.contains(stack.nodeId());
        const auto targetId = copyStack ? nodeIds.at(stack.nodeId()) : stack.nodeId();
        auto& target = *graph.merge(targetId);
        if (copyStack)
            target.setEnabled(stack.enabled());
        const auto entries = stack.entries();
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& entry = entries[index];
            if (!copyStack && !layerIds.contains(entry.layerId))
                continue;
            const auto* originalEdge = detail::inputEdge(
                original.graph(),
                document::LayerStackInputRef{stack.nodeId(), entry.slotId,
                                             std::string(document::kLayerStackContentInputRole)});
            if (!originalEdge)
                return detail::invalidTarget();
            const auto slotId = draft.ids().allocateLayerSlot();
            const auto edgeId = draft.ids().allocateEdge();
            if (!slotId || !edgeId)
                return detail::exhaustedIds();
            const auto layerId =
                layerIds.contains(entry.layerId) ? layerIds.at(entry.layerId) : entry.layerId;
            if (!target.append({*slotId, layerId}))
                return detail::invalidTarget();
            if (!copyStack &&
                !target.moveBefore(*slotId, index + 1 < entries.size()
                                                ? std::optional(entries[index + 1].slotId)
                                                : std::nullopt))
                return detail::invalidTarget();
            auto source = originalEdge->source;
            if (nodeIds.contains(source.nodeId))
                source.nodeId = nodeIds.at(source.nodeId);
            if (!graph.addEdge(
                    {*edgeId, source,
                     document::LayerStackInputRef{
                         targetId, *slotId, std::string(document::kLayerStackContentInputRole)}}))
                return detail::invalidTarget();
            outputs.push_back({"slot." + std::to_string(entry.slotId.value()), *slotId});
        }
    }
    for (auto edge : original.graph().edges()) {
        if (std::holds_alternative<document::LayerStackInputRef>(edge.destination))
            continue;
        const auto destination = detail::destinationNode(edge.destination);
        if (!nodes_.contains(destination))
            continue;
        const bool internal = nodes_.contains(edge.source.nodeId);
        const auto* destinationRecord = original.graph().findNode(destination);
        if (!internal &&
            (!destinationRecord || destinationRecord->typeId != document::kLayerOutputNodeType))
            continue;
        const auto edgeId = draft.ids().allocateEdge();
        if (!edgeId)
            return detail::exhaustedIds();
        edge.id = *edgeId;
        if (internal)
            edge.source.nodeId = nodeIds.at(edge.source.nodeId);
        std::visit(
            [&](auto& input) {
                if constexpr (std::is_same_v<std::decay_t<decltype(input)>, document::NodeInputRef>)
                    input.nodeId = nodeIds.at(input.nodeId);
                else
                    input.stackNodeId = nodeIds.at(input.stackNodeId);
            },
            edge.destination);
        if (!graph.addEdge(edge))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Duplicated internal edge could not be inserted");
    }
    if (const auto failure = detail::validateGraph(*composition))
        return *failure;
    return OperationResult::applied(std::move(outputs));
}
} // namespace bloom::commands
