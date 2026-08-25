#pragma once

#include <bloom/core/sha256.hpp>
#include <bloom/document/schema_version.hpp>
#include <bloom/document/validation.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::document {

inline constexpr SchemaVersion kColorSettingsSchemaVersionV1{1, 0};
inline constexpr SchemaVersion kOcioConfigReferenceSchemaVersionV1{1, 0};
inline constexpr std::string_view kProcessColorSpaceIdV1 = "lin_rec709_scene";
inline constexpr std::string_view kBloomNeutralConfigUriV1 = "bloom://ocio/neutral-v1/config.ocio";

inline constexpr std::size_t kMaxOcioProjectRelativePathBytes = 4'096;
inline constexpr std::size_t kMaxOcioExternalUriBytes = 16'384;
inline constexpr std::size_t kMaxOcioContextVariables = 256;
inline constexpr std::size_t kMaxOcioContextNameBytes = 128;
inline constexpr std::size_t kMaxOcioContextValueBytes = 4'096;

struct BuiltInOcioConfigLocator final {
    std::string uri;

    friend bool operator==(const BuiltInOcioConfigLocator&,
                           const BuiltInOcioConfigLocator&) = default;
};

struct ProjectRelativeOciozLocator final {
    std::string path;

    friend bool operator==(const ProjectRelativeOciozLocator&,
                           const ProjectRelativeOciozLocator&) = default;
};

struct ExternalOciozLocator final {
    // V1 validates the cross-platform safe file-URI subset shared by Bloom's future platform
    // adapter: local absolute paths, file:/// drive paths, and registered-name UNC authorities.
    // User information, ports, IP literals, queries, fragments, raw non-ASCII, malformed escapes,
    // and percent-encoded NUL or separators are outside this trusted document value.
    std::string uri;

    friend bool operator==(const ExternalOciozLocator&, const ExternalOciozLocator&) = default;
};

struct ExternalOcioConfigLocator final {
    // Uses the same bounded file-URI subset as ExternalOciozLocator and must terminate in the
    // decoded exact filename config.ocio.
    std::string uri;

    friend bool operator==(const ExternalOcioConfigLocator&,
                           const ExternalOcioConfigLocator&) = default;
};

using OcioConfigLocator = std::variant<BuiltInOcioConfigLocator, ProjectRelativeOciozLocator,
                                       ExternalOciozLocator, ExternalOcioConfigLocator>;

enum class OcioRevisionAlgorithm : std::uint8_t {
    Unknown = 0,
    Sha256 = 1,
};

struct OcioConfigRevision final {
    OcioRevisionAlgorithm algorithm = OcioRevisionAlgorithm::Unknown;
    core::Sha256Digest digest;

    friend bool operator==(const OcioConfigRevision&, const OcioConfigRevision&) = default;
};

enum class OcioConfigPortability : std::uint8_t {
    Unknown = 0,
    BuiltIn = 1,
    ProjectRelative = 2,
    External = 3,
};

struct OcioContextVariable final {
    std::string name;
    std::string value;

    friend bool operator==(const OcioContextVariable&, const OcioContextVariable&) = default;
};

struct OcioConfigReference final {
    SchemaVersion schemaVersion;
    OcioConfigLocator locator;
    OcioConfigRevision expectedRevision;
    OcioConfigPortability portability = OcioConfigPortability::Unknown;
    std::vector<OcioContextVariable> contextVariables;

    [[nodiscard]] ValidationResult validate() const;

    friend bool operator==(const OcioConfigReference&, const OcioConfigReference&) = default;
};

struct ColorSettings final {
    SchemaVersion schemaVersion;
    std::string processColorSpaceId;
    OcioConfigReference ocioConfig;

    [[nodiscard]] ValidationResult validate() const;

    friend bool operator==(const ColorSettings&, const ColorSettings&) = default;
};

// Constructs the exact version 1 new-project value around a revision supplied by the qualified
// Bloom Neutral build profile. This function does not discover, provision, or qualify that asset.
[[nodiscard]] ColorSettings makeBloomNeutralColorSettingsV1(core::Sha256Digest expectedRevision);

} // namespace bloom::document
