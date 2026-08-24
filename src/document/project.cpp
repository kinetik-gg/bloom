#include <bloom/document/project.hpp>

#include <bloom/document/persisted_text.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

template <typename Id>
void validateProjectUniqueDeclaration(const Id id, const std::size_t compositionOrdinal,
                                      std::string path, const std::string_view typeName,
                                      std::unordered_map<Id, std::size_t>& declarationOwners,
                                      bloom::document::ValidationResult& result) {
    if (!id.isValid()) {
        return;
    }

    const auto [existing, inserted] = declarationOwners.try_emplace(id, compositionOrdinal);
    if (!inserted && existing->second != compositionOrdinal) {
        result.add(bloom::document::ValidationCode::DuplicateId, std::move(path),
                   std::string(typeName) + " ID is declared by more than one composition");
    }
}

} // namespace

namespace bloom::document {

bool Composition::setDuration(const core::RationalTime duration) noexcept {
    if (duration <= core::RationalTime{}) {
        return false;
    }
    duration_ = duration;
    return true;
}

ValidationResult Composition::validate() const {
    ValidationResult result;
    if (!id_.isValid()) {
        result.add(ValidationCode::InvalidId, "id", "Composition ID must not be zero");
    }
    validateHumanFacingName(name_, "name", "Composition name", result);
    if (duration_ <= core::RationalTime{}) {
        result.add(ValidationCode::InvalidValue, "duration",
                   "Composition duration must be greater than zero");
    }

    result.append("parameters", parameters_.validate());
    result.append("animationCurves", animationCurves_.validate());
    result.append("", validateAnimationCurveReferences(parameters_, animationCurves_));
    result.append("graph", graph_.validate(parameters_));
    return result;
}

const Composition* Project::findComposition(const CompositionId id) const noexcept {
    const auto iterator =
        std::find_if(compositions_.begin(), compositions_.end(),
                     [id](const auto& composition) { return composition.id() == id; });
    return iterator == compositions_.end() ? nullptr : &*iterator;
}

Composition* Project::findComposition(const CompositionId id) noexcept {
    return const_cast<Composition*>(std::as_const(*this).findComposition(id));
}

bool Project::addComposition(Composition composition) {
    if (!composition.id().isValid() || !isValidHumanFacingName(composition.name()) ||
        findComposition(composition.id()) != nullptr) {
        return false;
    }
    compositions_.push_back(std::move(composition));
    return true;
}

bool Project::removeComposition(const CompositionId id) {
    const auto iterator =
        std::find_if(compositions_.begin(), compositions_.end(),
                     [id](const auto& composition) { return composition.id() == id; });
    if (iterator == compositions_.end()) {
        return false;
    }
    compositions_.erase(iterator);
    return true;
}

const ExtensionRecord* Project::findExtensionRecord(const ExtensionRecordId id) const noexcept {
    const auto iterator = std::ranges::lower_bound(extensionRecords_, id, {}, &ExtensionRecord::id);
    return iterator == extensionRecords_.end() || iterator->id != id ? nullptr : &*iterator;
}

ExtensionRecord* Project::findExtensionRecord(const ExtensionRecordId id) noexcept {
    return const_cast<ExtensionRecord*>(std::as_const(*this).findExtensionRecord(id));
}

bool Project::addExtensionRecord(ExtensionRecord record) {
    if (!record.id.isValid()) {
        return false;
    }
    const auto insertion =
        std::ranges::lower_bound(extensionRecords_, record.id, {}, &ExtensionRecord::id);
    if (insertion != extensionRecords_.end() && insertion->id == record.id) {
        return false;
    }
    extensionRecords_.insert(insertion, std::move(record));
    return true;
}

bool Project::removeExtensionRecord(const ExtensionRecordId id) {
    const auto iterator = std::ranges::lower_bound(extensionRecords_, id, {}, &ExtensionRecord::id);
    if (iterator == extensionRecords_.end() || iterator->id != id) {
        return false;
    }
    extensionRecords_.erase(iterator);
    return true;
}

ValidationResult Project::validate() const {
    ValidationResult result;
    if (!id_.isValid()) {
        result.add(ValidationCode::InvalidId, "id", "Project ID must not be zero");
    }
    validateHumanFacingName(name_, "name", "Project name", result);

    std::unordered_set<CompositionId> compositionIds;
    std::unordered_map<NodeId, std::size_t> nodeDeclarations;
    std::unordered_map<EdgeId, std::size_t> edgeDeclarations;
    std::unordered_map<ParameterId, std::size_t> parameterDeclarations;
    std::unordered_map<AnimationCurveId, std::size_t> animationCurveDeclarations;
    std::unordered_map<KeyframeId, std::size_t> keyframeDeclarations;
    std::unordered_map<LayerId, std::size_t> layerDeclarations;
    std::unordered_map<LayerSlotId, std::size_t> layerSlotDeclarations;

    for (std::size_t compositionOrdinal = 0; compositionOrdinal < compositions_.size();
         ++compositionOrdinal) {
        const auto& composition = compositions_[compositionOrdinal];
        const auto path = "compositions[" + std::to_string(composition.id().value()) + "]";
        if (!composition.id().isValid()) {
            result.add(ValidationCode::InvalidId, path + ".id", "Composition ID must not be zero");
        } else if (!compositionIds.insert(composition.id()).second) {
            result.add(ValidationCode::DuplicateId, path + ".id", "Composition ID is duplicated");
        }
        result.append(path, composition.validate());

        for (const auto& node : composition.graph().nodes()) {
            validateProjectUniqueDeclaration(node.id, compositionOrdinal,
                                             path + ".graph.nodes[" +
                                                 std::to_string(node.id.value()) + "].id",
                                             "Node", nodeDeclarations, result);
        }
        for (const auto& edge : composition.graph().edges()) {
            validateProjectUniqueDeclaration(edge.id, compositionOrdinal,
                                             path + ".graph.edges[" +
                                                 std::to_string(edge.id.value()) + "].id",
                                             "Edge", edgeDeclarations, result);
        }
        for (const auto& parameter : composition.parameters().records()) {
            validateProjectUniqueDeclaration(parameter.id, compositionOrdinal,
                                             path + ".parameters[" +
                                                 std::to_string(parameter.id.value()) + "].id",
                                             "Parameter", parameterDeclarations, result);
        }
        for (const auto& record : composition.animationCurves().records()) {
            const auto curveId = animationCurveId(record);
            const auto curvePath =
                path + ".animationCurves[" + std::to_string(curveId.value()) + "]";
            validateProjectUniqueDeclaration(curveId, compositionOrdinal, curvePath + ".id",
                                             "Animation curve", animationCurveDeclarations, result);
            std::visit(
                [&](const auto& curve) {
                    for (const auto& keyframe : curve.keyframes) {
                        validateProjectUniqueDeclaration(keyframe.id, compositionOrdinal,
                                                         curvePath + ".keyframes[" +
                                                             std::to_string(keyframe.id.value()) +
                                                             "].id",
                                                         "Keyframe", keyframeDeclarations, result);
                    }
                },
                record);
        }
        for (const auto& boundary : composition.graph().layerOutputs()) {
            validateProjectUniqueDeclaration(boundary.layerId, compositionOrdinal,
                                             path + ".graph.layerOutputs[" +
                                                 std::to_string(boundary.layerId.value()) +
                                                 "].layerId",
                                             "Layer", layerDeclarations, result);
        }
        for (const auto& entry : composition.graph().layerStack().entries()) {
            validateProjectUniqueDeclaration(entry.slotId, compositionOrdinal,
                                             path + ".graph.layerStack.entries[" +
                                                 std::to_string(entry.slotId.value()) + "].slotId",
                                             "Layer Stack slot", layerSlotDeclarations, result);
        }
    }
    result.append("", validateExtensionRecords(*this));
    return result;
}

} // namespace bloom::document
