#pragma once

#include "strict_json.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace bloom::quality::dependencies {

// This tooling validates Bloom's checked-in synthetic contract fixtures and, since the production
// lock validator landed, the production dependency lock at dependencies/dependencies.lock.json.
// It is still not an installed-prefix validator: production prefix-manifest validation remains out
// of scope.

enum class ArtifactKind {
    Lock,
    Prefix,
};

struct ArtifactLimits final {
    std::size_t maximumBytes;
    std::size_t maximumDepth;
    std::size_t maximumValues;
};

class CheckError final : public std::runtime_error {
  public:
    CheckError(std::string_view code, std::string_view location, std::string_view detail);
};

struct LoadedArtifact final {
    json::Value value;
    std::string encoded;
};

struct FixtureContext final {
    std::filesystem::path root;
    std::filesystem::path payloadRoot;
    std::unordered_set<std::string> payloadDigests;
    std::unordered_map<std::string, std::uint64_t> payloadSizes;
};

struct CheckResult final {
    std::string lockVector;
    std::string prefixVector;
    bool productionLockPresent{false};
    std::string productionLockIdentity;
};

[[nodiscard]] auto limitsFor(ArtifactKind kind) noexcept -> ArtifactLimits;
[[nodiscard]] auto readBounded(const std::filesystem::path& path, std::size_t maximumBytes)
    -> std::string;
[[nodiscard]] auto loadSchemaArtifact(const std::filesystem::path& path) -> LoadedArtifact;
[[nodiscard]] auto parseCanonicalFixture(std::string_view encoded, ArtifactKind kind,
                                         const ArtifactLimits* overrideLimits = nullptr)
    -> json::Value;
[[nodiscard]] auto encodeCanonical(const json::Value& value) -> std::string;

void validateSchemaArtifact(const json::Value& value, ArtifactKind kind);
[[nodiscard]] auto makeFixtureContext(const std::filesystem::path& root) -> FixtureContext;
void validateLockFixture(const json::Value& value, const FixtureContext& context);
void validatePrefixFixture(const json::Value& value, const json::Value& lock,
                           std::string_view lockEncoded, const FixtureContext& context);
[[nodiscard]] auto checkRepository(const std::filesystem::path& root) -> CheckResult;

void rejectFixtureAsProductionPath(const std::filesystem::path& path,
                                   const std::filesystem::path& expected,
                                   const std::filesystem::path& fixtureRoot);
void verifyFixturePayload(std::string_view relativePath, std::string_view digest,
                          std::string_view location, const FixtureContext& context);
[[nodiscard]] auto timestampFromEpoch(std::uint64_t epoch) -> std::string;

} // namespace bloom::quality::dependencies
