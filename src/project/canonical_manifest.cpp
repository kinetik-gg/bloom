#include <bloom/project/canonical_manifest.hpp>

#include <bloom/core/utf8.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/project/canonical_decimal.hpp>
#include <bloom/project/canonical_json_string.hpp>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <string_view>

namespace {

using bloom::project::CanonicalJsonWriter;
using bloom::project::CanonicalJsonWriterResult;
using bloom::project::CanonicalManifestError;
using bloom::project::CanonicalManifestLimits;
using bloom::project::CanonicalManifestSizeResult;
using bloom::project::CanonicalManifestV1;
using bloom::project::kCanonicalManifestNoIndex;
using bloom::project::ManifestRequirement;

struct ValidationResult final {
    CanonicalManifestError error = CanonicalManifestError::None;
    std::size_t requirementIndex = kCanonicalManifestNoIndex;
    std::size_t nodeTypeIndex = kCanonicalManifestNoIndex;
};

[[nodiscard]] constexpr ValidationResult
failure(const CanonicalManifestError error,
        const std::size_t requirementIndex = kCanonicalManifestNoIndex,
        const std::size_t nodeTypeIndex = kCanonicalManifestNoIndex) noexcept {
    return {error, requirementIndex, nodeTypeIndex};
}

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

[[nodiscard]] bool sameRequirementIdentity(const ManifestRequirement& left,
                                           const ManifestRequirement& right) noexcept {
    return left.providerId == right.providerId && left.capabilityId == right.capabilityId;
}

[[nodiscard]] ValidationResult validateManifest(const CanonicalManifestV1& manifest,
                                                const CanonicalManifestLimits& limits) noexcept {
    if (limits.maximumRequirements > bloom::project::kMaxManifestRequirementCount ||
        limits.maximumProvidedNodeTypes > bloom::project::kMaxProvidedNodeTypeCount ||
        limits.maximumValues > bloom::project::kCanonicalJsonMaximumValues ||
        limits.maximumOutputBytes > bloom::project::kCanonicalManifestMaximumBytes) {
        return failure(CanonicalManifestError::InvalidLimits);
    }
    if (manifest.format != bloom::project::kCanonicalManifestFormat) {
        return failure(CanonicalManifestError::InvalidFormat);
    }
    if (manifest.containerVersion != bloom::project::kCanonicalManifestContainerVersionV1) {
        return failure(CanonicalManifestError::InvalidContainerVersion);
    }
    if (manifest.documentPath != bloom::project::kCanonicalManifestDocumentPath) {
        return failure(CanonicalManifestError::InvalidDocumentPath);
    }
    if (manifest.documentSchemaVersion !=
        bloom::project::kCanonicalManifestDocumentSchemaVersionV1) {
        return failure(CanonicalManifestError::InvalidDocumentSchemaVersion);
    }
    if (manifest.requirements.size() > limits.maximumRequirements) {
        return failure(CanonicalManifestError::RequirementCountExceeded);
    }

    constexpr std::size_t fixedValueCount = 11;
    std::size_t valueCount = fixedValueCount;
    if (valueCount > limits.maximumValues) {
        return failure(CanonicalManifestError::ValueCountExceeded);
    }

    const ManifestRequirement* previousRequirement = nullptr;
    for (std::size_t requirementIndex = 0; requirementIndex < manifest.requirements.size();
         ++requirementIndex) {
        const auto& requirement = manifest.requirements[requirementIndex];
        if (!bloom::document::isValidNamespacedIdentifier(requirement.providerId)) {
            return failure(CanonicalManifestError::InvalidProviderId, requirementIndex);
        }
        if (!bloom::document::isValidNamespacedIdentifier(requirement.capabilityId)) {
            return failure(CanonicalManifestError::InvalidCapabilityId, requirementIndex);
        }
        if (!requirement.schemaVersion.isValid()) {
            return failure(CanonicalManifestError::InvalidRequirementSchemaVersion,
                           requirementIndex);
        }
        if (previousRequirement != nullptr) {
            if (sameRequirementIdentity(*previousRequirement, requirement)) {
                return failure(CanonicalManifestError::DuplicateRequirementIdentity,
                               requirementIndex);
            }
            if (!requirementLess(*previousRequirement, requirement)) {
                return failure(CanonicalManifestError::InvalidRequirementOrder, requirementIndex);
            }
        }
        previousRequirement = &requirement;

        if (requirement.providedNodeTypeIds.size() > limits.maximumProvidedNodeTypes) {
            return failure(CanonicalManifestError::ProvidedNodeTypeCountExceeded, requirementIndex);
        }

        std::string_view previousNodeType;
        bool hasPreviousNodeType = false;
        for (std::size_t nodeTypeIndex = 0; nodeTypeIndex < requirement.providedNodeTypeIds.size();
             ++nodeTypeIndex) {
            const auto& nodeTypeId = requirement.providedNodeTypeIds[nodeTypeIndex];
            if (!bloom::document::isValidNamespacedIdentifier(nodeTypeId)) {
                return failure(CanonicalManifestError::InvalidProvidedNodeTypeId, requirementIndex,
                               nodeTypeIndex);
            }
            if (hasPreviousNodeType) {
                const auto order = bloom::core::compareUtf8Bytes(previousNodeType, nodeTypeId);
                if (order == std::strong_ordering::equal) {
                    return failure(CanonicalManifestError::DuplicateProvidedNodeTypeId,
                                   requirementIndex, nodeTypeIndex);
                }
                if (order != std::strong_ordering::less) {
                    return failure(CanonicalManifestError::InvalidProvidedNodeTypeOrder,
                                   requirementIndex, nodeTypeIndex);
                }
            }
            previousNodeType = nodeTypeId;
            hasPreviousNodeType = true;
        }

        constexpr std::size_t requirementFixedValues = 7;
        if (requirementFixedValues > limits.maximumValues - valueCount) {
            return failure(CanonicalManifestError::ValueCountExceeded, requirementIndex);
        }
        valueCount += requirementFixedValues;
        if (requirement.providedNodeTypeIds.size() > limits.maximumValues - valueCount) {
            return failure(CanonicalManifestError::ValueCountExceeded, requirementIndex);
        }
        valueCount += requirement.providedNodeTypeIds.size();
    }
    return {};
}

class SizeAccumulator final {
  public:
    [[nodiscard]] bool add(const std::size_t bytes) noexcept {
        if (bytes > std::numeric_limits<std::size_t>::max() - size_) {
            return false;
        }
        size_ += bytes;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

  private:
    std::size_t size_ = 0;
};

[[nodiscard]] bool addStringToken(SizeAccumulator& size, const std::string_view value) noexcept {
    const auto tokenSize = bloom::project::canonicalJsonStringTokenSize(value);
    return tokenSize && size.add(*tokenSize.value());
}

[[nodiscard]] bool addIntegerToken(SizeAccumulator& size, const std::uint32_t value) noexcept {
    return size.add(bloom::project::formatCanonicalUInt64(value).size());
}

[[nodiscard]] bool addMemberPrefix(SizeAccumulator& size, const std::size_t depth, const bool first,
                                   const std::string_view name) noexcept {
    const std::size_t separatorAndIndent = (first ? 1 : 2) + depth * 2;
    return size.add(separatorAndIndent) && addStringToken(size, name) && size.add(2);
}

[[nodiscard]] bool addArrayElementPrefix(SizeAccumulator& size, const std::size_t depth,
                                         const bool first) noexcept {
    return size.add((first ? 1 : 2) + depth * 2);
}

[[nodiscard]] bool addNonEmptyContainerEnd(SizeAccumulator& size,
                                           const std::size_t depth) noexcept {
    return size.add(2 + (depth - 1) * 2);
}

[[nodiscard]] bool addRequirementSize(SizeAccumulator& size,
                                      const ManifestRequirement& requirement) noexcept {
    if (!size.add(1) || !addMemberPrefix(size, 3, true, "providerId") ||
        !addStringToken(size, requirement.providerId) ||
        !addMemberPrefix(size, 3, false, "capabilityId") ||
        !addStringToken(size, requirement.capabilityId) ||
        !addMemberPrefix(size, 3, false, "schemaVersion") || !size.add(1) ||
        !addMemberPrefix(size, 4, true, "major") ||
        !addIntegerToken(size, requirement.schemaVersion.major) ||
        !addMemberPrefix(size, 4, false, "minor") ||
        !addIntegerToken(size, requirement.schemaVersion.minor) ||
        !addNonEmptyContainerEnd(size, 4) ||
        !addMemberPrefix(size, 3, false, "providedNodeTypeIds") || !size.add(1)) {
        return false;
    }

    if (requirement.providedNodeTypeIds.empty()) {
        if (!size.add(1)) {
            return false;
        }
    } else {
        for (std::size_t index = 0; index < requirement.providedNodeTypeIds.size(); ++index) {
            if (!addArrayElementPrefix(size, 4, index == 0) ||
                !addStringToken(size, requirement.providedNodeTypeIds[index])) {
                return false;
            }
        }
        if (!addNonEmptyContainerEnd(size, 4)) {
            return false;
        }
    }
    return addNonEmptyContainerEnd(size, 3);
}

[[nodiscard]] CanonicalManifestSizeResult
computeManifestSize(const CanonicalManifestV1& manifest,
                    const CanonicalManifestLimits& limits) noexcept {
    SizeAccumulator size;
    if (!size.add(1) || !addMemberPrefix(size, 1, true, "format") ||
        !addStringToken(size, manifest.format) ||
        !addMemberPrefix(size, 1, false, "containerVersion") || !size.add(1) ||
        !addMemberPrefix(size, 2, true, "major") ||
        !addIntegerToken(size, manifest.containerVersion.major) ||
        !addMemberPrefix(size, 2, false, "minor") ||
        !addIntegerToken(size, manifest.containerVersion.minor) ||
        !addNonEmptyContainerEnd(size, 2) || !addMemberPrefix(size, 1, false, "document") ||
        !size.add(1) || !addMemberPrefix(size, 2, true, "path") ||
        !addStringToken(size, manifest.documentPath) ||
        !addMemberPrefix(size, 2, false, "schemaVersion") || !size.add(1) ||
        !addMemberPrefix(size, 3, true, "major") ||
        !addIntegerToken(size, manifest.documentSchemaVersion.major) ||
        !addMemberPrefix(size, 3, false, "minor") ||
        !addIntegerToken(size, manifest.documentSchemaVersion.minor) ||
        !addNonEmptyContainerEnd(size, 3) || !addNonEmptyContainerEnd(size, 2) ||
        !addMemberPrefix(size, 1, false, "requirements") || !size.add(1)) {
        return CanonicalManifestSizeResult::failure(CanonicalManifestError::SizeOverflow);
    }

    if (manifest.requirements.empty()) {
        if (!size.add(1)) {
            return CanonicalManifestSizeResult::failure(CanonicalManifestError::SizeOverflow);
        }
    } else {
        for (std::size_t index = 0; index < manifest.requirements.size(); ++index) {
            if (!addArrayElementPrefix(size, 2, index == 0) ||
                !addRequirementSize(size, manifest.requirements[index])) {
                return CanonicalManifestSizeResult::failure(CanonicalManifestError::SizeOverflow,
                                                            index);
            }
        }
        if (!addNonEmptyContainerEnd(size, 2)) {
            return CanonicalManifestSizeResult::failure(CanonicalManifestError::SizeOverflow);
        }
    }
    if (!addNonEmptyContainerEnd(size, 1) || !size.add(1)) {
        return CanonicalManifestSizeResult::failure(CanonicalManifestError::SizeOverflow);
    }
    if (size.size() > limits.maximumOutputBytes) {
        return CanonicalManifestSizeResult::failure(CanonicalManifestError::ManifestSizeExceeded);
    }
    return CanonicalManifestSizeResult::success(size.size());
}

void requireSuccess(const CanonicalJsonWriterResult result) noexcept {
    if (!result) {
        std::terminate();
    }
}

void emitVersion(CanonicalJsonWriter& writer,
                 const bloom::document::SchemaVersion version) noexcept {
    requireSuccess(writer.beginObject());
    requireSuccess(writer.memberName("major"));
    requireSuccess(writer.integerValue(version.major));
    requireSuccess(writer.memberName("minor"));
    requireSuccess(writer.integerValue(version.minor));
    requireSuccess(writer.endObject());
}

void emitRequirement(CanonicalJsonWriter& writer, const ManifestRequirement& requirement) noexcept {
    requireSuccess(writer.beginObject());
    requireSuccess(writer.memberName("providerId"));
    requireSuccess(writer.stringValue(requirement.providerId));
    requireSuccess(writer.memberName("capabilityId"));
    requireSuccess(writer.stringValue(requirement.capabilityId));
    requireSuccess(writer.memberName("schemaVersion"));
    emitVersion(writer, requirement.schemaVersion);
    requireSuccess(writer.memberName("providedNodeTypeIds"));
    requireSuccess(writer.beginArray());
    for (const auto& nodeTypeId : requirement.providedNodeTypeIds) {
        requireSuccess(writer.stringValue(nodeTypeId));
    }
    requireSuccess(writer.endArray());
    requireSuccess(writer.endObject());
}

void emitManifest(CanonicalJsonWriter& writer, const CanonicalManifestV1& manifest) noexcept {
    requireSuccess(writer.beginObject());
    requireSuccess(writer.memberName("format"));
    requireSuccess(writer.stringValue(manifest.format));
    requireSuccess(writer.memberName("containerVersion"));
    emitVersion(writer, manifest.containerVersion);
    requireSuccess(writer.memberName("document"));
    requireSuccess(writer.beginObject());
    requireSuccess(writer.memberName("path"));
    requireSuccess(writer.stringValue(manifest.documentPath));
    requireSuccess(writer.memberName("schemaVersion"));
    emitVersion(writer, manifest.documentSchemaVersion);
    requireSuccess(writer.endObject());
    requireSuccess(writer.memberName("requirements"));
    requireSuccess(writer.beginArray());
    for (const auto& requirement : manifest.requirements) {
        emitRequirement(writer, requirement);
    }
    requireSuccess(writer.endArray());
    requireSuccess(writer.endObject());
    requireSuccess(writer.finish());
}

} // namespace

namespace bloom::project {

CanonicalManifestSizeResult canonicalManifestSize(const CanonicalManifestV1& manifest,
                                                  const CanonicalManifestLimits limits) noexcept {
    const auto validation = validateManifest(manifest, limits);
    if (validation.error != CanonicalManifestError::None) {
        return CanonicalManifestSizeResult::failure(validation.error, validation.requirementIndex,
                                                    validation.nodeTypeIndex);
    }
    return computeManifestSize(manifest, limits);
}

CanonicalManifestWriteResult
encodeCanonicalManifest(const CanonicalManifestV1& manifest, const std::span<char> output,
                        const CanonicalManifestLimits limits) noexcept {
    const auto sizeResult = canonicalManifestSize(manifest, limits);
    if (!sizeResult) {
        return CanonicalManifestWriteResult::failure(sizeResult.error(), std::nullopt,
                                                     sizeResult.requirementIndex(),
                                                     sizeResult.nodeTypeIndex());
    }
    const auto requiredSize = *sizeResult.value();
    if (output.size() < requiredSize) {
        return CanonicalManifestWriteResult::failure(CanonicalManifestError::OutputCapacityExceeded,
                                                     requiredSize);
    }

    const auto maximumContainerEntries =
        std::max({std::size_t{4}, limits.maximumRequirements, limits.maximumProvidedNodeTypes});
    CanonicalJsonWriter writer(output.first(requiredSize),
                               {.maximumDepth = 5,
                                .maximumValues = limits.maximumValues,
                                .maximumContainerEntries = maximumContainerEntries});
    emitManifest(writer, manifest);
    if (writer.bytesWritten() != requiredSize) {
        std::terminate();
    }
    return CanonicalManifestWriteResult::success(requiredSize);
}

} // namespace bloom::project
