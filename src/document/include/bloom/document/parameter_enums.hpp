#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::document {

// One member of a closed vocabulary that a parameter stores as an integer: the English label every
// surface shows and the stored value. Registry metadata, like ParameterPresentation: never
// serialized and never read by validation, which stays the authority on what a document may store.
struct ParameterEnumOption final {
    std::string label;
    std::int64_t value = 0;

    friend bool operator==(const ParameterEnumOption&, const ParameterEnumOption&) = default;
};

// The closed vocabulary of an integer-stored schema, in the order the editor offers it; empty when
// the schema is not an enumeration. Scripting clients use it to accept a value by name and to
// describe a parameter; the Properties selectors show the same members.
[[nodiscard]] std::vector<ParameterEnumOption> parameterEnumOptions(std::string_view schemaKey);

// The stored value whose label matches `name`, ignoring case, spaces, hyphens and underscores
// ("Ping-pong", "ping_pong" and "pingpong" all name the same member).
[[nodiscard]] std::optional<std::int64_t> parameterEnumValue(std::string_view schemaKey,
                                                             std::string_view name);

} // namespace bloom::document
