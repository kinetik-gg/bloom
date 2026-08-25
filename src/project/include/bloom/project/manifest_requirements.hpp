#pragma once

#include <bloom/document/project.hpp>
#include <bloom/document/schema_version.hpp>
#include <bloom/document/validation.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::project {

inline constexpr std::size_t kMaxManifestRequirementCount = 1'000'000;
inline constexpr std::size_t kMaxProvidedNodeTypeCount = 1'000'000;

struct ManifestRequirement final {
    std::string providerId;
    std::string capabilityId;
    document::SchemaVersion schemaVersion;
    std::vector<std::string> providedNodeTypeIds;

    friend bool operator==(const ManifestRequirement&, const ManifestRequirement&) = default;
};

[[nodiscard]] bool isFoundationNodeType(std::string_view typeId) noexcept;

// Validates the manifest's canonical ordering and exact node/extension ownership coverage against
// already-decoded project truth. Registered extension schemas perform their more specific
// type-to-capability check at the Project I/O registry boundary.
[[nodiscard]] document::ValidationResult
validateManifestRequirements(const document::Project& project,
                             std::span<const ManifestRequirement> requirements);

} // namespace bloom::project
