#include <bloom/project/manifest_requirements.hpp>

#include <bloom/core/utf8.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/persisted_text.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using bloom::document::ValidationCode;
using bloom::document::ValidationResult;
using bloom::project::ManifestRequirement;

struct RequirementKey final {
    std::string_view providerId;
    std::string_view capabilityId;

    friend bool operator==(const RequirementKey&, const RequirementKey&) = default;
};

struct RequirementKeyHash final {
    [[nodiscard]] std::size_t operator()(const RequirementKey& key) const noexcept {
        const auto first = std::hash<std::string_view>{}(key.providerId);
        const auto second = std::hash<std::string_view>{}(key.capabilityId);
        return first ^
               (second + static_cast<std::size_t>(0x9E3779B9U) + (first << 6U) + (first >> 2U));
    }
};

[[nodiscard]] bool requirementLess(const ManifestRequirement& left,
                                   const ManifestRequirement& right) noexcept {
    const auto providerOrder = bloom::core::compareUtf8Bytes(left.providerId, right.providerId);
    if (providerOrder != std::strong_ordering::equal) {
        return providerOrder == std::strong_ordering::less;
    }
    const auto capabilityOrder =
        bloom::core::compareUtf8Bytes(left.capabilityId, right.capabilityId);
    if (capabilityOrder != std::strong_ordering::equal) {
        return capabilityOrder == std::strong_ordering::less;
    }
    return left.schemaVersion < right.schemaVersion;
}

[[nodiscard]] std::string requirementPath(const std::size_t index) {
    return "requirements[" + std::to_string(index) + "]";
}

void sortAndDeduplicate(std::vector<std::string_view>& values) {
    std::ranges::sort(values, [](const std::string_view left, const std::string_view right) {
        return bloom::core::compareUtf8Bytes(left, right) == std::strong_ordering::less;
    });
    values.erase(std::ranges::unique(values).begin(), values.end());
}

} // namespace

namespace bloom::project {

bool isFoundationNodeType(const std::string_view typeId) noexcept {
    constexpr std::array foundationTypes{
        document::kCompositionOutputNodeType, document::kLayerOutputNodeType,
        document::kLayerStackNodeType,        document::kSolidSourceNodeType,
        document::kTextSourceNodeType,
    };
    for (const auto foundationType : foundationTypes) {
        if (typeId == foundationType) {
            return true;
        }
    }
    return false;
}

document::ValidationResult
validateManifestRequirements(const document::Project& project,
                             const std::span<const ManifestRequirement> requirements) {
    ValidationResult result;
    if (requirements.size() > kMaxManifestRequirementCount) {
        result.add(ValidationCode::InvalidValue, "requirements",
                   "Manifest requirement count exceeds the project array limit");
        return result;
    }

    std::vector<std::string_view> requiredNodeTypes;
    for (const auto& composition : project.compositions()) {
        for (const auto& node : composition.graph().nodes()) {
            if (!isFoundationNodeType(node.typeId)) {
                requiredNodeTypes.push_back(node.typeId);
            }
        }
    }
    sortAndDeduplicate(requiredNodeTypes);

    std::vector<std::string_view> requiredExtensionOwners;
    for (const auto& record : project.extensionRecords()) {
        requiredExtensionOwners.push_back(record.ownerId);
    }
    sortAndDeduplicate(requiredExtensionOwners);

    std::unordered_set<RequirementKey, RequirementKeyHash> requirementKeys;
    std::unordered_set<std::string_view> declaredProviders;
    std::unordered_map<std::string_view, std::size_t> nodeProviders;
    const ManifestRequirement* previous = nullptr;

    for (std::size_t requirementIndex = 0; requirementIndex < requirements.size();
         ++requirementIndex) {
        const auto& requirement = requirements[requirementIndex];
        const auto path = requirementPath(requirementIndex);
        document::validateNamespacedIdentifier(requirement.providerId, path + ".providerId",
                                               "Requirement provider ID", result);
        document::validateNamespacedIdentifier(requirement.capabilityId, path + ".capabilityId",
                                               "Requirement capability ID", result);
        if (!requirement.schemaVersion.isValid()) {
            result.add(ValidationCode::InvalidValue, path + ".schemaVersion",
                       "Requirement schema major version must not be zero");
        }
        if (previous != nullptr && !requirementLess(*previous, requirement)) {
            result.add(
                ValidationCode::InvalidOrder, path,
                "Requirements must be strictly ordered by provider, capability, and version");
        }
        previous = &requirement;

        if (!requirementKeys
                 .insert(RequirementKey{requirement.providerId, requirement.capabilityId})
                 .second) {
            result.add(ValidationCode::DuplicateId, path,
                       "Requirement provider and capability pair is duplicated");
        }
        if (document::isValidNamespacedIdentifier(requirement.providerId)) {
            declaredProviders.insert(requirement.providerId);
        }
        if (requirement.providedNodeTypeIds.size() > kMaxProvidedNodeTypeCount) {
            result.add(ValidationCode::InvalidValue, path + ".providedNodeTypeIds",
                       "Provided node-type count exceeds the project array limit");
            continue;
        }

        std::string_view previousType;
        bool hasPreviousType = false;
        for (std::size_t typeIndex = 0; typeIndex < requirement.providedNodeTypeIds.size();
             ++typeIndex) {
            const auto& typeId = requirement.providedNodeTypeIds[typeIndex];
            const auto typePath = path + ".providedNodeTypeIds[" + std::to_string(typeIndex) + "]";
            document::validateNamespacedIdentifier(typeId, typePath, "Provided node type ID",
                                                   result);
            if (hasPreviousType &&
                core::compareUtf8Bytes(previousType, typeId) != std::strong_ordering::less) {
                result.add(ValidationCode::InvalidOrder, typePath,
                           "Provided node type IDs must be unique and sorted by UTF-8 bytes");
            }
            previousType = typeId;
            hasPreviousType = true;

            if (isFoundationNodeType(typeId)) {
                result.add(ValidationCode::InvalidValue, typePath,
                           "Foundation node types must not appear in manifest requirements");
                continue;
            }
            if (!std::ranges::binary_search(
                    requiredNodeTypes, typeId,
                    [](const std::string_view left, const std::string_view right) {
                        return core::compareUtf8Bytes(left, right) == std::strong_ordering::less;
                    })) {
                result.add(ValidationCode::OrphanObject, typePath,
                           "Provided node type does not occur in project truth");
                continue;
            }
            if (!nodeProviders.emplace(typeId, requirementIndex).second) {
                result.add(ValidationCode::SharedReference, typePath,
                           "Node type is provided by more than one requirement");
            }
        }
    }

    for (const auto nodeType : requiredNodeTypes) {
        if (!nodeProviders.contains(nodeType)) {
            result.add(ValidationCode::MissingReference, "requirements",
                       "No manifest requirement provides node type " + std::string(nodeType));
        }
    }
    for (const auto ownerId : requiredExtensionOwners) {
        if (!declaredProviders.contains(ownerId)) {
            result.add(ValidationCode::MissingReference, "requirements",
                       "No manifest requirement covers extension owner " + std::string(ownerId));
        }
    }
    return result;
}

} // namespace bloom::project
