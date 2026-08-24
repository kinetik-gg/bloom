#include <bloom/document/extension_records.hpp>

#include <bloom/document/project.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <variant>

namespace {

using namespace bloom::document;

[[nodiscard]] bool isIdentifierCharacter(const char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
           character == '.' || character == '_' || character == '-';
}

[[nodiscard]] bool isNamespacedIdentifier(const std::string_view value) noexcept {
    return !value.empty() && value.size() <= kMaxExtensionIdentifierBytes &&
           ((value.front() >= 'a' && value.front() <= 'z') ||
            (value.front() >= '0' && value.front() <= '9')) &&
           std::ranges::all_of(value, isIdentifierCharacter);
}

[[nodiscard]] bool isContinuationByte(const std::uint8_t byte) noexcept {
    return (byte & 0xC0U) == 0x80U;
}

[[nodiscard]] bool isValidUtf8(const std::string_view value) noexcept {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data());
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto lead = bytes[offset];
        if (lead <= 0x7FU) {
            ++offset;
            continue;
        }

        if (lead >= 0xC2U && lead <= 0xDFU) {
            if (offset + 1 >= value.size() || !isContinuationByte(bytes[offset + 1])) {
                return false;
            }
            offset += 2;
            continue;
        }

        if (lead >= 0xE0U && lead <= 0xEFU) {
            if (offset + 2 >= value.size() || !isContinuationByte(bytes[offset + 1]) ||
                !isContinuationByte(bytes[offset + 2])) {
                return false;
            }
            if ((lead == 0xE0U && bytes[offset + 1] < 0xA0U) ||
                (lead == 0xEDU && bytes[offset + 1] >= 0xA0U)) {
                return false;
            }
            offset += 3;
            continue;
        }

        if (lead >= 0xF0U && lead <= 0xF4U) {
            if (offset + 3 >= value.size() || !isContinuationByte(bytes[offset + 1]) ||
                !isContinuationByte(bytes[offset + 2]) || !isContinuationByte(bytes[offset + 3])) {
                return false;
            }
            if ((lead == 0xF0U && bytes[offset + 1] < 0x90U) ||
                (lead == 0xF4U && bytes[offset + 1] >= 0x90U)) {
                return false;
            }
            offset += 4;
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool utf8BytesLess(const std::string_view left,
                                 const std::string_view right) noexcept {
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
                                        [](const char leftByte, const char rightByte) {
                                            return static_cast<unsigned char>(leftByte) <
                                                   static_cast<unsigned char>(rightByte);
                                        });
}

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

void validateIdentifier(const std::string_view value, std::string path,
                        const std::string_view label, ValidationResult& result) {
    if (!isNamespacedIdentifier(value)) {
        result.add(ValidationCode::InvalidValue, std::move(path),
                   std::string(label) +
                       " must be a lowercase ASCII identifier of at most 128 bytes");
    }
}

void validateStructuralString(const std::string_view value, std::string path,
                              const std::string_view label, ValidationResult& result) {
    if (value.empty()) {
        result.add(ValidationCode::EmptyKey, std::move(path),
                   std::string(label) + " must not be empty");
    } else if (value.size() > kMaxExtensionStructuralStringBytes || !isValidUtf8(value)) {
        result.add(ValidationCode::InvalidValue, std::move(path),
                   std::string(label) + " must be valid UTF-8 of at most 256 bytes");
    }
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

        validateIdentifier(record.ownerId, path + ".ownerId", "Extension owner ID", result);
        validateIdentifier(record.typeId, path + ".typeId", "Extension type ID", result);
        if (!record.schemaVersion.isValid()) {
            result.add(ValidationCode::InvalidValue, path + ".schemaVersion",
                       "Extension schema major version must not be zero");
        }
        if (record.subject.has_value() && !targetExists(*record.subject)) {
            result.add(ValidationCode::MissingReference, path + ".subject",
                       "Extension subject must resolve in current project truth");
        }
        validateStructuralString(record.mediaType, path + ".mediaType", "Media type", result);

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
                validateStructuralString(reference.key, referencePath + ".key",
                                         "Host reference key", result);
                if (hasPreviousKey && !utf8BytesLess(previousKey, reference.key)) {
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
            validateIdentifier(remapper->remapperId, path + ".referencePolicy.remapperId",
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
