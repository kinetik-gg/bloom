#include <bloom/document/extension_records.hpp>

#include <bloom/core/utf8.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/project.hpp>

#include <compare>
#include <concepts>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <variant>

namespace {

using namespace bloom::document;

struct TargetDeclarations final {
    ProjectId project;
    std::unordered_set<CompositionId> compositions;
    std::unordered_set<NodeId> nodes;
    std::unordered_set<EdgeId> edges;
    std::unordered_set<LayerId> layers;
    std::unordered_set<LayerSlotId> layerSlots;
    std::unordered_set<ParameterId> parameters;
    std::unordered_set<AnimationCurveId> animationCurves;
    std::unordered_set<KeyframeId> keyframes;

    [[nodiscard]] bool contains(const ExtensionTarget& target) const {
        return std::visit(
            [this](const auto id) {
                using Id = std::remove_cvref_t<decltype(id)>;
                if constexpr (std::same_as<Id, ProjectId>) {
                    return id.isValid() && id == project;
                } else if constexpr (std::same_as<Id, CompositionId>) {
                    return compositions.contains(id);
                } else if constexpr (std::same_as<Id, NodeId>) {
                    return nodes.contains(id);
                } else if constexpr (std::same_as<Id, EdgeId>) {
                    return edges.contains(id);
                } else if constexpr (std::same_as<Id, LayerId>) {
                    return layers.contains(id);
                } else if constexpr (std::same_as<Id, LayerSlotId>) {
                    return layerSlots.contains(id);
                } else if constexpr (std::same_as<Id, ParameterId>) {
                    return parameters.contains(id);
                } else if constexpr (std::same_as<Id, AnimationCurveId>) {
                    return animationCurves.contains(id);
                } else {
                    return keyframes.contains(id);
                }
            },
            target);
    }
};

[[nodiscard]] TargetDeclarations collectTargetDeclarations(const Project& project) {
    TargetDeclarations declarations;
    declarations.project = project.id();
    for (const auto& composition : project.compositions()) {
        declarations.compositions.insert(composition.id());
        for (const auto& node : composition.graph().nodes()) {
            declarations.nodes.insert(node.id);
        }
        for (const auto& edge : composition.graph().edges()) {
            declarations.edges.insert(edge.id);
        }
        for (const auto& boundary : composition.graph().layerOutputs()) {
            declarations.layers.insert(boundary.layerId);
        }
        for (const auto& entry : composition.graph().layerStack().entries()) {
            declarations.layerSlots.insert(entry.slotId);
        }
        for (const auto& parameter : composition.parameters().records()) {
            declarations.parameters.insert(parameter.id);
        }
        for (const auto& record : composition.animationCurves().records()) {
            declarations.animationCurves.insert(animationCurveId(record));
            std::visit(
                [&declarations](const auto& curve) {
                    for (const auto& keyframe : curve.keyframes) {
                        declarations.keyframes.insert(keyframe.id);
                    }
                },
                record);
        }
    }
    return declarations;
}

} // namespace

namespace bloom::document {

ValidationResult validateExtensionRecords(const Project& project) {
    ValidationResult result;
    if (project.extensionRecords().empty()) {
        return result;
    }

    std::optional<TargetDeclarations> declarations;
    const auto targetExists = [&declarations, &project](const ExtensionTarget& target) {
        if (!declarations.has_value()) {
            declarations.emplace(collectTargetDeclarations(project));
        }
        return declarations->contains(target);
    };
    std::unordered_set<ExtensionRecordId> recordIds;
    std::size_t aggregatePayloadBytes = 0;
    bool aggregatePayloadLimitExceeded = false;
    ExtensionRecordId previousId;

    for (const auto& record : project.extensionRecords()) {
        const auto path = "extensionRecords[" + std::to_string(record.id.value()) + "]";
        if (!record.id.isValid()) {
            result.add(ValidationCode::InvalidId, path + ".id",
                       "Extension record ID must not be zero");
        } else if (!recordIds.insert(record.id).second) {
            result.add(ValidationCode::DuplicateId, path + ".id",
                       "Extension record ID is duplicated");
        }
        if (previousId.isValid() && record.id <= previousId) {
            result.add(ValidationCode::InvalidOrder, path + ".id",
                       "Extension records must be ordered by numeric ID");
        }
        previousId = record.id;

        validateNamespacedIdentifier(record.ownerId, path + ".ownerId", "Extension owner ID",
                                     result);
        validateNamespacedIdentifier(record.typeId, path + ".typeId", "Extension type ID", result);
        if (!record.schemaVersion.isValid()) {
            result.add(ValidationCode::InvalidValue, path + ".schemaVersion",
                       "Extension schema major version must not be zero");
        }
        if (record.subject.has_value() && !targetExists(*record.subject)) {
            result.add(ValidationCode::MissingReference, path + ".subject",
                       "Extension subject must resolve in current project truth");
        }
        validateStructuralText(record.mediaType, path + ".mediaType", "Media type", result);

        if (record.payload.size() > kMaxOpaqueExtensionPayloadBytes) {
            result.add(ValidationCode::InvalidValue, path + ".payload",
                       "Opaque extension payload exceeds the 64 MiB record limit");
        }
        if (!aggregatePayloadLimitExceeded &&
            record.payload.size() >
                kMaxAggregateOpaqueExtensionPayloadBytes - aggregatePayloadBytes) {
            aggregatePayloadLimitExceeded = true;
            result.add(ValidationCode::InvalidValue, "extensionRecords",
                       "Opaque extension payloads exceed the 128 MiB project limit");
        } else if (!aggregatePayloadLimitExceeded) {
            aggregatePayloadBytes += record.payload.size();
        }

        if (const auto* table = std::get_if<ExtensionHostReferenceTable>(&record.referencePolicy)) {
            std::string_view previousKey;
            bool hasPreviousKey = false;
            for (std::size_t index = 0; index < table->references.size(); ++index) {
                const auto& reference = table->references[index];
                const auto referencePath =
                    path + ".referencePolicy.references[" + std::to_string(index) + "]";
                validateStructuralText(reference.key, referencePath + ".key", "Host reference key",
                                       result);
                if (hasPreviousKey && core::compareUtf8Bytes(previousKey, reference.key) !=
                                          std::strong_ordering::less) {
                    result.add(ValidationCode::InvalidOrder, referencePath + ".key",
                               "Host reference keys must be unique and sorted by UTF-8 bytes");
                }
                previousKey = reference.key;
                hasPreviousKey = true;
                if (!targetExists(reference.target)) {
                    result.add(ValidationCode::MissingReference, referencePath + ".target",
                               "Host reference target must resolve in current project truth");
                }
            }
        } else if (const auto* remapper =
                       std::get_if<ExtensionOwnerRemapper>(&record.referencePolicy)) {
            validateNamespacedIdentifier(remapper->remapperId, path + ".referencePolicy.remapperId",
                                         "Extension remapper ID", result);
            if (!remapper->version.isValid()) {
                result.add(ValidationCode::InvalidValue, path + ".referencePolicy.version",
                           "Extension remapper major version must not be zero");
            }
        }
    }
    return result;
}

} // namespace bloom::document
