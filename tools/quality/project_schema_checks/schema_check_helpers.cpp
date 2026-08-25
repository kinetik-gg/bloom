#include "schema_check_helpers.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <ranges>
#include <string>
#include <unordered_set>

namespace bloom::quality {

SchemaCheckError::SchemaCheckError(const std::string& message) : std::runtime_error(message) {}

} // namespace bloom::quality

namespace bloom::quality::schema_detail {
namespace {

[[nodiscard]] auto formatLocation(const std::string_view location, const std::string_view message)
    -> std::string {
    return std::string{location} + ' ' + std::string{message};
}

[[nodiscard]] auto resolveReference(const json::Value& root, const std::string_view reference)
    -> const json::Value& {
    if (!reference.starts_with("#/")) {
        fail("schema reference '" + std::string{reference} + "' is not repository-local");
    }
    const auto* current = &root;
    auto remaining = reference.substr(2);
    while (true) {
        const auto separator = remaining.find('/');
        auto component = std::string{remaining.substr(0, separator)};
        for (auto index = std::size_t{0}; index < component.size(); ++index) {
            if (component[index] != '~') {
                continue;
            }
            if (index + 1U == component.size()) {
                fail("schema reference '" + std::string{reference} + "' has an invalid escape");
            }
            if (component[index + 1U] == '0') {
                component.replace(index, 2, "~");
            } else if (component[index + 1U] == '1') {
                component.replace(index, 2, "/");
            } else {
                fail("schema reference '" + std::string{reference} + "' has an invalid escape");
            }
        }
        current = current->find(component);
        if (current == nullptr) {
            fail("schema reference '" + std::string{reference} + "' does not resolve");
        }
        if (separator == std::string_view::npos) {
            return *current;
        }
        remaining.remove_prefix(separator + 1U);
    }
}

} // namespace

[[noreturn]] void fail(const std::string_view message) {
    throw SchemaCheckError(std::string{message});
}

auto requireObject(const json::Value& value, const std::string_view location)
    -> const json::Value::Object& {
    if (!value.isObject()) {
        fail(formatLocation(location, "must be an object"));
    }
    return value.asObject();
}

auto requireArray(const json::Value& value, const std::string_view location)
    -> const json::Value::Array& {
    if (!value.isArray()) {
        fail(formatLocation(location, "must be an array"));
    }
    return value.asArray();
}

auto requireMember(const json::Value& object, const std::string_view key,
                   const std::string_view location) -> const json::Value& {
    requireObject(object, location);
    const auto* member = object.find(key);
    if (member == nullptr) {
        fail(formatLocation(location, "must contain member '" + std::string{key} + "'"));
    }
    return *member;
}

auto requireString(const json::Value& value, const std::string_view location)
    -> const std::string& {
    if (!value.isString()) {
        fail(formatLocation(location, "must be a string"));
    }
    return value.asString();
}

void requireExactString(const json::Value& value, const std::string_view expected,
                        const std::string_view location) {
    const auto& actual = requireString(value, location);
    if (actual != expected) {
        fail(formatLocation(location, "must equal '" + std::string{expected} + "'"));
    }
}

void requireExactBoolean(const json::Value& value, const bool expected,
                         const std::string_view location) {
    if (!value.isBoolean() || value.asBoolean() != expected) {
        fail(formatLocation(location, expected ? "must equal true" : "must equal false"));
    }
}

void requireExactInteger(const json::Value& value, const std::uint64_t expected,
                         const std::string_view location) {
    const auto actual = json::asUint64(value);
    if (!actual.has_value() || *actual != expected) {
        fail(formatLocation(location, "must equal integer " + std::to_string(expected)));
    }
}

void requireExact(const json::Value& value, const std::string_view expectedJson,
                  const std::string_view location) {
    const auto expected = json::parse(expectedJson);
    if (!json::exactEqual(value, expected)) {
        fail(formatLocation(location, "does not equal the required JSON value"));
    }
}

void requireExactKeys(const json::Value& value,
                      const std::initializer_list<std::string_view> expected,
                      const std::string_view location) {
    requireExactKeys(value, std::span<const std::string_view>{expected.begin(), expected.size()},
                     location);
}

void requireExactKeys(const json::Value& value, const std::span<const std::string_view> expected,
                      const std::string_view location) {
    const auto& object = requireObject(value, location);
    if (object.size() != expected.size()) {
        fail(formatLocation(location, "has an unexpected member set"));
    }
    for (const auto key : expected) {
        if (value.find(key) == nullptr) {
            fail(formatLocation(location, "is missing required member '" + std::string{key} + "'"));
        }
    }
}

void requireExactStringArray(const json::Value& value,
                             const std::initializer_list<std::string_view> expected,
                             const std::string_view location) {
    requireExactStringArray(
        value, std::span<const std::string_view>{expected.begin(), expected.size()}, location);
}

void requireExactStringArray(const json::Value& value,
                             const std::span<const std::string_view> expected,
                             const std::string_view location) {
    const auto& array = requireArray(value, location);
    if (array.size() != expected.size()) {
        fail(formatLocation(location, "has an unexpected array length"));
    }
    auto index = std::size_t{0};
    for (const auto expectedValue : expected) {
        requireExactString(array[index], expectedValue,
                           std::string{location} + '[' + std::to_string(index) + ']');
        ++index;
    }
}

void validateReferences(const json::Value& root, const json::Value& value,
                        const std::string& location, const std::size_t depth) {
    if (depth > 128U) {
        fail("schema traversal exceeds the quality checker depth limit");
    }
    if (value.isObject()) {
        if (const auto* referenceValue = value.find("$ref"); referenceValue != nullptr) {
            const auto& reference = requireString(*referenceValue, location + ".$ref");
            static_cast<void>(resolveReference(root, reference));
        }
        for (const auto& [key, child] : value.asObject()) {
            auto childLocation = location;
            childLocation.push_back('.');
            childLocation.append(key);
            validateReferences(root, child, childLocation, depth + 1U);
        }
    } else if (value.isArray()) {
        for (auto index = std::size_t{0}; index < value.asArray().size(); ++index) {
            validateReferences(root, value.asArray()[index],
                               location + '[' + std::to_string(index) + ']', depth + 1U);
        }
    }
}

void validateVersionDefinition(const json::Value& definition, const std::string_view location,
                               const bool fixed, const std::string_view majorReference) {
    requireObject(definition, location);
    requireExactString(requireMember(definition, "type", location), "object",
                       std::string{location} + ".type");
    requireExactStringArray(requireMember(definition, "required", location), {"major", "minor"},
                            std::string{location} + ".required");
    requireExactBoolean(requireMember(definition, "unevaluatedProperties", location), true,
                        std::string{location} + ".unevaluatedProperties");
    const auto& properties = requireMember(definition, "properties", location);
    requireExactKeys(properties, {"major", "minor"}, std::string{location} + ".properties");
    if (fixed) {
        requireExact(requireMember(properties, "major", location), R"({"const":1})",
                     std::string{location} + ".properties.major");
        requireExact(requireMember(properties, "minor", location), R"({"const":0})",
                     std::string{location} + ".properties.minor");
        return;
    }
    requireExact(requireMember(properties, "major", location),
                 "{\"$ref\":\"" + std::string{majorReference} + "\"}",
                 std::string{location} + ".properties.major");
    requireExact(requireMember(properties, "minor", location), R"({"$ref":"#/$defs/unsigned32"})",
                 std::string{location} + ".properties.minor");
}

auto validateObjectShape(const json::Value& definitions, const std::string_view name,
                         const std::initializer_list<std::string_view> required,
                         const std::initializer_list<std::string_view> propertyNames)
    -> const json::Value& {
    const auto location = "$.$defs." + std::string{name};
    const auto& definition = requireMember(definitions, name, "$.$defs");
    if (definition.find("$comment") != nullptr) {
        requireExactKeys(definition,
                         {"type", "required", "properties", "unevaluatedProperties", "$comment"},
                         location + " keys");
        static_cast<void>(requireString(*definition.find("$comment"), location + ".$comment"));
    } else {
        requireExactKeys(definition, {"type", "required", "properties", "unevaluatedProperties"},
                         location + " keys");
    }
    requireExactString(requireMember(definition, "type", location), "object", location + ".type");
    requireExactStringArray(requireMember(definition, "required", location), required,
                            location + ".required");
    requireExactBoolean(requireMember(definition, "unevaluatedProperties", location), true,
                        location + ".unevaluatedProperties");
    const auto& properties = requireMember(definition, "properties", location);
    requireExactKeys(properties, propertyNames, location + ".properties keys");
    return properties;
}

auto validateDiscriminatedUnion(const json::Value& definitions, const std::string_view name,
                                const std::span<const ExpectedBranch> expected)
    -> std::vector<const json::Value*> {
    const auto location = "$.$defs." + std::string{name};
    const auto& definition = requireMember(definitions, name, "$.$defs");
    requireExactKeys(definition, {"oneOf"}, location + " keys");
    const auto& alternatives =
        requireArray(requireMember(definition, "oneOf", location), location + ".oneOf");
    if (alternatives.size() != expected.size()) {
        fail(location + ".oneOf must contain exactly " + std::to_string(expected.size()) +
             " known alternatives");
    }
    std::vector<const json::Value*> result;
    result.reserve(expected.size());
    for (auto index = std::size_t{0}; index < expected.size(); ++index) {
        const auto branchLocation = location + ".oneOf[" + std::to_string(index) + ']';
        const auto& branch = alternatives[index];
        requireExactKeys(branch, {"type", "required", "properties", "unevaluatedProperties"},
                         branchLocation + " keys");
        requireExactString(requireMember(branch, "type", branchLocation), "object",
                           branchLocation + ".type");
        requireExactStringArray(requireMember(branch, "required", branchLocation),
                                expected[index].required, branchLocation + ".required");
        requireExactBoolean(requireMember(branch, "unevaluatedProperties", branchLocation), true,
                            branchLocation + ".unevaluatedProperties");
        const auto& properties = requireMember(branch, "properties", branchLocation);
        requireExactKeys(properties, expected[index].properties,
                         branchLocation + ".properties keys");
        requireExact(requireMember(properties, "kind", branchLocation),
                     "{\"const\":\"" + std::string{expected[index].kind} + "\"}",
                     branchLocation + ".properties.kind");
        result.push_back(&properties);
    }
    return result;
}

void validateArray(const json::Value& value, const std::string_view location,
                   const std::string_view itemReference, const std::uint64_t maximum,
                   const std::uint64_t minimum, const bool hasMinimum) {
    requireObject(value, location);
    requireExactKeys(
        value,
        hasMinimum
            ? std::initializer_list<std::string_view>{"type", "maxItems", "items", "minItems"}
            : std::initializer_list<std::string_view>{"type", "maxItems", "items"},
        location);
    requireExactString(requireMember(value, "type", location), "array",
                       std::string{location} + ".type");
    requireExactInteger(requireMember(value, "maxItems", location), maximum,
                        std::string{location} + ".maxItems");
    requireExact(requireMember(value, "items", location),
                 "{\"$ref\":\"" + std::string{itemReference} + "\"}",
                 std::string{location} + ".items");
    if (hasMinimum) {
        requireExactInteger(requireMember(value, "minItems", location), minimum,
                            std::string{location} + ".minItems");
    }
}

} // namespace bloom::quality::schema_detail
