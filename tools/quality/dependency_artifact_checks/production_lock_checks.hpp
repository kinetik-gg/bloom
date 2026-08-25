#pragma once

#include "dependency_artifact_checks.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace bloom::quality::dependencies {

// Production dependency-lock validation bound to the exact repository path
// dependencies/dependencies.lock.json. The synthetic-fixture validators remain unchanged; this
// surface is still not an installed-prefix validator, and production prefix-manifest validation
// remains out of scope.
//
// While the production lock file is absent the validator reports absence as success so CI stays
// green; the Unicode 15.1 bootstrap self-check below always runs regardless.
//
// Unicode 15.1 bootstrap two-source equality: the constants below are the reviewed official
// archive and extracted-file digests recorded in dependencies/unicode/provenance.md. The validator
// hashes the raw checked-in files at dependencies/unicode/15.1.0/<name> against this allowlist and
// separately requires a present lock's unicodeProfile to equal the same constants member for
// member. The lock can therefore never authorize the tables that validate it.
//
// ASCII-STRICT v1 TIGHTENING: any decoded string or object-key byte >= 0x80 anywhere in a
// production lock is rejected with the distinct "ascii-strict" error code before parsing. This is
// a deliberate v1 policy tightening, not a contract reading: canonical JSON emits non-control
// Unicode directly as UTF-8, escapes cannot smuggle non-ASCII scalars past the canonical profile,
// and ASCII is closed under Unicode 15.1 NFC, so the full normalization and Default Case Folding
// machinery can be deferred without weakening acceptance. The contract permits policy to tighten
// but never loosen. Fixture-validation behavior is unchanged.
//
// Supporting-artifact interpretation: per the contract every artifact reference is
// repository-root-relative and begins with "dependencies/", never redirects by artifact values,
// and must be a regular checked-in file whose bytes reproduce its recorded sha256. This validator
// additionally refuses any reference resolved below dependencies/tests/fixtures/, because that
// tree is reserved for synthetic fixtures, and confines patch references below
// dependencies/patches/<component>/. Other evidence directories (licenses, reviews, provenance,
// security, conformance fixtures) are accepted wherever below dependencies/ they declare;
// narrowing them further is deferred until the production lock exists.
//
// Deferred rules (tracked here, deliberately not implemented in v1):
// - feature decisions closed against each recipe's owned feature vocabulary: unrecognized or
//   omitted recipe features remain a human lock/recipe review duty until recipe ownership data is
//   available to this offline checker;
// - full Unicode 15.1 NFC and Default Case Folding validation of decoded strings, deferred behind
//   the ASCII-strict tightening above;
// - acquisition-time provenance-policy signature verification and source-archive digest binding,
//   which belong to the acquire phase rather than offline lock review;
// - non-machine-checkable review obligations, such as whether empty copyrightFiles or noticeFiles
//   are permitted by a component's reviewed license terms.

inline constexpr std::string_view kProductionLockRepositoryPath =
    "dependencies/dependencies.lock.json";

struct UnicodeBootstrapRecord final {
    std::string_view name;
    std::string_view sha256Hex;
};

inline constexpr std::string_view kUnicodeBootstrapVersion = "15.1.0";
inline constexpr std::string_view kUnicodeBootstrapSourceUrl =
    "https://www.unicode.org/Public/15.1.0/ucd/UCD.zip";
inline constexpr std::string_view kUnicodeBootstrapArchiveHex =
    "cb1c663d053926500cd501229736045752713a066bd75802098598b7a7056177";
inline constexpr std::array<UnicodeBootstrapRecord, 5> kUnicodeBootstrapFiles{{
    {"UnicodeData.txt", "2fc713e6a31a87c4850a37fe2caffa4218180fadb5de86b43a143ddb4581fb86"},
    {"CompositionExclusions.txt",
     "59d2d9e3dfdf0a999cf9dae11d594f053631222679a2f5710315ea07f7fe82af"},
    {"DerivedNormalizationProps.txt",
     "8875dccee2bc1a7c1fe568a3b502a9e78c9e0495afd96b6568b4294d0ed1f7e1"},
    {"CaseFolding.txt", "4e55acfdc32825a22e87670e9056a3bf94ad7c5400065778e9e10f8314372bcf"},
    {"NormalizationTest.txt", "871238e37e3be0696ec2bd0891119a041b052da1a84485eda05a5438724b223e"},
}};

struct ProductionLockResult final {
    bool present{false};
    std::string identity;
};

[[nodiscard]] auto sha256DigestText(std::string_view bytes) -> std::string;
void verifyUnicodeBootstrap(const std::filesystem::path& root);
void validateProductionLockDocument(const json::Value& value, const std::filesystem::path& root);
[[nodiscard]] auto validateProductionLock(const std::filesystem::path& root)
    -> ProductionLockResult;

} // namespace bloom::quality::dependencies
