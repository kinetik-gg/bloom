#pragma once

#include "schema_checks.hpp"

#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::quality::schema_detail {

struct ExpectedBranch final {
    std::string_view kind;
    std::vector<std::string_view> required;
    std::vector<std::string_view> properties;
};

[[noreturn]] void fail(std::string_view message);

auto requireObject(const json::Value& value, std::string_view location)
    -> const json::Value::Object&;
[[nodiscard]] auto requireArray(const json::Value& value, std::string_view location)
    -> const json::Value::Array&;
[[nodiscard]] auto requireMember(const json::Value& object, std::string_view key,
                                 std::string_view location) -> const json::Value&;
[[nodiscard]] auto requireString(const json::Value& value, std::string_view location)
    -> const std::string&;
void requireExactString(const json::Value& value, std::string_view expected,
                        std::string_view location);
void requireExactBoolean(const json::Value& value, bool expected, std::string_view location);
void requireExactInteger(const json::Value& value, std::uint64_t expected,
                         std::string_view location);
void requireExact(const json::Value& value, std::string_view expectedJson,
                  std::string_view location);
void requireExactKeys(const json::Value& value, std::initializer_list<std::string_view> expected,
                      std::string_view location);
void requireExactKeys(const json::Value& value, std::span<const std::string_view> expected,
                      std::string_view location);
void requireExactStringArray(const json::Value& value,
                             std::initializer_list<std::string_view> expected,
                             std::string_view location);
void requireExactStringArray(const json::Value& value, std::span<const std::string_view> expected,
                             std::string_view location);

void validateReferences(const json::Value& root, const json::Value& value,
                        const std::string& location = "$", std::size_t depth = 1);
void validateVersionDefinition(const json::Value& definition, std::string_view location, bool fixed,
                               std::string_view majorReference = "#/$defs/positiveMajorVersion");
[[nodiscard]] auto validateObjectShape(const json::Value& definitions, std::string_view name,
                                       std::initializer_list<std::string_view> required,
                                       std::initializer_list<std::string_view> propertyNames)
    -> const json::Value&;
[[nodiscard]] auto validateDiscriminatedUnion(const json::Value& definitions, std::string_view name,
                                              std::span<const ExpectedBranch> expected)
    -> std::vector<const json::Value*>;
void validateArray(const json::Value& value, std::string_view location,
                   std::string_view itemReference, std::uint64_t maximum = 1'000'000,
                   std::uint64_t minimum = 0, bool hasMinimum = false);

void validateDocumentGraphAndReferences(const json::Value& definitions,
                                        const std::vector<const json::Value*>& parameterValues,
                                        const std::vector<const json::Value*>& curves);

} // namespace bloom::quality::schema_detail
