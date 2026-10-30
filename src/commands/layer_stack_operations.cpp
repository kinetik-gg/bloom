#include <bloom/commands/layer_stack_operations.hpp>

#include <bloom/commands/node_operations.hpp>
#include <bloom/document/project.hpp>

#include <set>
#include <string>

namespace bloom::commands {
namespace {

// The Layer Output boundary node of every layer, or the rejection that names the first one the
// composition does not hold.
[[nodiscard]] std::optional<OperationResult>
boundaryNodes(const document::Draft& draft, const document::CompositionId compositionId,
              const std::vector<document::LayerId>& layers, std::set<document::NodeId>& nodes) {
    const auto* composition = draft.project().findComposition(compositionId);
    if (composition == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "The composition does not exist");
    if (layers.empty())
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "No layers were named");
    for (const auto layer : layers) {
        const auto* boundary = composition->graph().findLayer(layer);
        if (boundary == nullptr)
            return OperationResult::rejected(OperationIssueCode::MissingReference,
                                             "Layer " + std::to_string(layer.value()) +
                                                 " is not in the composition");
        nodes.insert(boundary->nodeId);
    }
    return std::nullopt;
}

} // namespace

OperationResult DeleteLayers::apply(document::Draft& draft) const {
    std::set<document::NodeId> nodes;
    if (auto failure = boundaryNodes(draft, composition_, layers_, nodes))
        return std::move(*failure);
    return RemoveNodes(composition_, std::move(nodes)).apply(draft);
}

OperationResult DuplicateLayers::apply(document::Draft& draft) const {
    std::set<document::NodeId> nodes;
    if (auto failure = boundaryNodes(draft, composition_, layers_, nodes))
        return std::move(*failure);
    auto result =
        DuplicateNodes(composition_, std::move(nodes), document::Vec2d{0, 0}).apply(draft);
    if (result.status == OperationStatus::Rejected)
        return result;
    std::vector<OperationOutput> copies;
    for (const auto layer : layers_) {
        const auto name = "layer." + std::to_string(layer.value());
        for (const auto& output : result.outputs)
            if (output.name == name)
                copies.push_back({"layer", output.id});
    }
    result.outputs.insert(result.outputs.end(), copies.begin(), copies.end());
    return result;
}

} // namespace bloom::commands
