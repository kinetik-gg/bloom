#pragma once

#include "dependency_artifact_checks.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::quality::dependencies::detail {

using Path = std::filesystem::path;
using json::Value;

inline constexpr std::string_view kSchemaDirectory = "dependencies/schemas";
inline constexpr std::string_view kFixtureDirectory = "dependencies/tests/fixtures";
inline constexpr std::string_view kPayloadPrefix = "dependencies/tests/fixtures/payloads/";
inline constexpr std::array<std::string_view, 5> kUnicodeFiles{
    "UnicodeData.txt", "CompositionExclusions.txt", "DerivedNormalizationProps.txt",
    "CaseFolding.txt", "NormalizationTest.txt"};

[[noreturn]] void fail(std::string_view code, std::string_view location, std::string_view detail);
[[nodiscard]] auto digestHex(std::string_view value) -> std::string;

auto object(const Value& value, std::initializer_list<std::string_view> keys,
            std::string_view location) -> const Value::Object&;
[[nodiscard]] auto array(const Value& value, std::string_view location, std::size_t maximum,
                         std::size_t minimum = 0) -> const Value::Array&;
[[nodiscard]] auto isAscii(std::string_view value) noexcept -> bool;
[[nodiscard]] auto isIdentifier(std::string_view value) noexcept -> bool;
[[nodiscard]] auto isDottedVersion(std::string_view value) noexcept -> bool;
[[nodiscard]] auto isEnvironmentName(std::string_view value) noexcept -> bool;
[[nodiscard]] auto isCmakeOption(std::string_view value) noexcept -> bool;
auto stringValue(const Value& value, std::string_view location, std::size_t maximum = 4096)
    -> const std::string&;
auto nullableString(const Value& value, std::string_view location, std::size_t maximum = 4096)
    -> std::optional<std::string_view>;
auto identifier(const Value& value, std::string_view location) -> const std::string&;
auto enumString(const Value& value, std::initializer_list<std::string_view> allowed,
                std::string_view location) -> const std::string&;
auto uintValue(const Value& value, std::string_view location,
               std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max()) -> std::uint64_t;
auto booleanValue(const Value& value, std::string_view location) -> bool;
[[nodiscard]] auto isDigest(std::string_view value) noexcept -> bool;
auto digestValue(const Value& value, std::string_view location,
                 const FixtureContext* context = nullptr) -> const std::string&;
[[nodiscard]] auto lowerAscii(std::string value) -> std::string;
auto portablePath(const Value& value, std::string_view location) -> const std::string&;
void validatePrintableToken(std::string_view value, std::string_view location);

template <typename Key>
void requireOrdered(const Value::Array& values, Key key, const std::string_view location) {
    if (values.empty()) {
        return;
    }
    auto previous = key(values.front());
    for (std::size_t index = 1; index < values.size(); ++index) {
        auto current = key(values[index]);
        if (current == previous) {
            fail("duplicate-identity", location, "collection identities must be unique");
        }
        if (current < previous) {
            fail("order", location, "collection must use canonical byte order");
        }
        previous = std::move(current);
    }
}

[[nodiscard]] auto strictEqual(const Value& left, const Value& right) -> bool;
auto artifactReference(const Value& value, std::string_view location, const FixtureContext& context)
    -> std::pair<std::string, std::string>;
void validateDate(const Value& value, std::string_view location);
void validateArtifactArray(const Value& value, std::string_view location,
                           const FixtureContext& context, std::size_t minimum = 0);
void validatePrefixArtifactArray(const Value& value, std::string_view location,
                                 const FixtureContext& context, std::size_t minimum = 0);

struct ComponentArtifact final {
    std::string path;
    std::string digest;
    std::string role;
};

void validateProfile(const Value& value, const std::string& location);
void validateComponent(const Value& value, const std::string& location,
                       const FixtureContext& context);
[[nodiscard]] auto identityVector(std::string_view domain, std::string_view encoded) -> std::string;
[[nodiscard]] auto buildIdentityVector(const Value& build) -> std::string;
[[nodiscard]] auto componentArtifacts(const Value& component) -> std::vector<ComponentArtifact>;

} // namespace bloom::quality::dependencies::detail
