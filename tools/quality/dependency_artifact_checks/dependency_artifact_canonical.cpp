#include "dependency_artifact_checks_internal.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <ranges>
#include <system_error>
#include <utility>

namespace bloom::quality::dependencies {
using namespace detail;
namespace {

void encodeString(std::string& output, const std::string_view value) {
    output.push_back('"');
    for (const auto byte : value) {
        switch (byte) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default: {
            const auto unsignedByte = static_cast<unsigned char>(byte);
            if (unsignedByte < 0x20U) {
                constexpr char hexadecimal[] = "0123456789abcdef";
                output += "\\u00";
                output.push_back(hexadecimal[(unsignedByte >> 4U) & 0x0FU]);
                output.push_back(hexadecimal[unsignedByte & 0x0FU]);
            } else {
                output.push_back(byte);
            }
            break;
        }
        }
    }
    output.push_back('"');
}

void encodeValue(std::string& output, const Value& value) {
    if (value.isNull()) {
        output += "null";
    } else if (value.isBoolean()) {
        output += value.asBoolean() ? "true" : "false";
    } else if (value.isNumber()) {
        output += value.asNumber().spelling;
    } else if (value.isString()) {
        encodeString(output, value.asString());
    } else if (value.isArray()) {
        output.push_back('[');
        bool first = true;
        for (const auto& child : value.asArray()) {
            if (!std::exchange(first, false)) {
                output.push_back(',');
            }
            encodeValue(output, child);
        }
        output.push_back(']');
    } else {
        output.push_back('{');
        bool first = true;
        for (const auto& [key, child] : value.asObject()) {
            if (!std::exchange(first, false)) {
                output.push_back(',');
            }
            encodeString(output, key);
            output.push_back(':');
            encodeValue(output, child);
        }
        output.push_back('}');
    }
}

void requireCanonicalDomain(const Value& value, const std::string_view location) {
    if (value.isString()) {
        if (!isAscii(value.asString())) {
            fail("unicode-bootstrap", location, "synthetic fixtures must remain ASCII");
        }
        return;
    }
    if (value.isNumber()) {
        if (!json::asUint64(value).has_value()) {
            fail("uint64", location, "synthetic canonical numbers must be uint64 values");
        }
        return;
    }
    if (value.isArray()) {
        for (std::size_t index = 0; index < value.asArray().size(); ++index) {
            requireCanonicalDomain(value.asArray()[index],
                                   std::string(location) + '[' + std::to_string(index) + ']');
        }
    } else if (value.isObject()) {
        for (const auto& [key, child] : value.asObject()) {
            if (!isAscii(key)) {
                fail("unicode-bootstrap", location, "synthetic fixture keys must remain ASCII");
            }
            requireCanonicalDomain(child, std::string(location) + '.' + key);
        }
    }
}

void preflightCanonicalSpelling(const std::string_view encoded) {
    if (encoded.starts_with("\xEF\xBB\xBF")) {
        fail("utf8-bom", "$", "UTF-8 BOM is forbidden");
    }
    bool insideString = false;
    bool escaped = false;
    std::size_t containerDepth = 0;
    bool rootContainerComplete = false;
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const auto byte = encoded[index];
        if (insideString) {
            if (escaped) {
                escaped = false;
                if (byte == '/') {
                    fail("canonical-escape", "$", "escaped solidus is non-canonical");
                }
                if (byte == 'u') {
                    if (index + 4 >= encoded.size()) {
                        fail("canonical-escape", "$", "truncated Unicode escape");
                    }
                    const auto digits = encoded.substr(index + 1, 4);
                    const auto lowercaseHex = [](const char digit) {
                        return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
                    };
                    if (!digits.starts_with("00") || !std::ranges::all_of(digits, lowercaseHex)) {
                        fail("canonical-escape", "$",
                             "only lowercase control Unicode escapes are canonical");
                    }
                    const auto hexadecimal = [](const char digit) {
                        return static_cast<unsigned>(digit <= '9' ? digit - '0' : digit - 'a' + 10);
                    };
                    const auto scalar = (hexadecimal(digits[2]) << 4U) | hexadecimal(digits[3]);
                    if (scalar > 0x1FU || scalar == 8U || scalar == 9U || scalar == 10U ||
                        scalar == 12U || scalar == 13U) {
                        fail("canonical-escape", "$", "control has a shorter canonical escape");
                    }
                    index += 4;
                }
                continue;
            }
            if (byte == '\\') {
                escaped = true;
            } else if (byte == '"') {
                insideString = false;
            }
            continue;
        }
        if (rootContainerComplete) {
            fail("canonical-trailing", "$", "content after the root value is forbidden");
        }
        if (byte == '"') {
            insideString = true;
        } else if (byte == '{' || byte == '[') {
            ++containerDepth;
        } else if ((byte == '}' || byte == ']') && containerDepth > 0) {
            --containerDepth;
            rootContainerComplete = containerDepth == 0;
        } else if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') {
            fail("canonical-token", "$", "whitespace is forbidden in canonical JSON");
        } else if (byte == '-') {
            fail("canonical-token", "$", "negative numbers are outside the fixture domain");
        } else if (byte == '.') {
            const auto followsInteger =
                index > 0 && encoded[index - 1] >= '0' && encoded[index - 1] <= '9';
            fail(followsInteger ? "canonical-comma" : "canonical-token", "$",
                 "fractional numbers are outside the fixture domain");
        }
    }
}

} // namespace

CheckError::CheckError(const std::string_view code, const std::string_view location,
                       const std::string_view detail)
    : std::runtime_error(std::string(code) + ": " + std::string(location) + ": " +
                         std::string(detail)) {}

auto limitsFor(const ArtifactKind kind) noexcept -> ArtifactLimits {
    if (kind == ArtifactKind::Lock) {
        return {16'777'216, 64, 1'000'000};
    }
    return {134'217'728, 64, 4'000'000};
}

auto readBounded(const Path& path, const std::size_t maximumBytes) -> std::string {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        fail("read", path.string(), error.message());
    }
    if (size > maximumBytes) {
        fail("resource-bytes", path.string(), "artifact exceeds its byte limit");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        fail("read", path.string(), "could not open artifact");
    }
    std::string result;
    result.reserve(static_cast<std::size_t>(size));
    std::array<char, 65'536> buffer{};
    while (result.size() <= maximumBytes) {
        const auto remaining = maximumBytes - result.size();
        const auto request = remaining >= buffer.size() ? buffer.size() : remaining + 1;
        stream.read(buffer.data(), static_cast<std::streamsize>(request));
        const auto received = stream.gcount();
        if (received > 0) {
            result.append(buffer.data(), static_cast<std::size_t>(received));
        }
        if (result.size() > maximumBytes) {
            fail("resource-bytes", path.string(), "artifact exceeds its byte limit");
        }
        if (received < static_cast<std::streamsize>(request)) {
            if (stream.bad() || (!stream.eof() && stream.fail())) {
                fail("read", path.string(), "could not read complete artifact");
            }
            break;
        }
    }
    return result;
}

auto encodeCanonical(const Value& value) -> std::string {
    std::string result;
    encodeValue(result, value);
    return result;
}

auto parseCanonicalFixture(const std::string_view encoded, const ArtifactKind kind,
                           const ArtifactLimits* const overrideLimits) -> Value {
    const auto limits = overrideLimits == nullptr ? limitsFor(kind) : *overrideLimits;
    if (encoded.size() > limits.maximumBytes) {
        fail("resource-bytes", "$", "artifact exceeds its byte limit");
    }
    try {
        auto result = json::parse(encoded, {.maximumBytes = limits.maximumBytes,
                                            .maximumDepth = limits.maximumDepth,
                                            .maximumValues = limits.maximumValues,
                                            .maximumContainerEntries = 200'000,
                                            .maximumStringBytes = 1'048'576});
        preflightCanonicalSpelling(encoded);
        requireCanonicalDomain(result, "$");
        if (encodeCanonical(result) != encoded) {
            fail("canonical-bytes", "$", "typed value does not reproduce input exactly");
        }
        return result;
    } catch (const json::ParseError& error) {
        const std::string_view message = error.what();
        if (message.find("BOM") != std::string_view::npos) {
            fail("utf8-bom", "$", message);
        }
        if (message.find("duplicate decoded") != std::string_view::npos) {
            fail("duplicate-member", "$", message);
        }
        if (message.find("leading zero") != std::string_view::npos) {
            fail("canonical-integer", "$", message);
        }
        if (message.find("trailing content") != std::string_view::npos) {
            fail("canonical-trailing", "$", message);
        }
        if (message.find("nesting depth") != std::string_view::npos) {
            fail("resource-depth", "$", message);
        }
        if (message.find("value count") != std::string_view::npos) {
            fail("resource-values", "$", message);
        }
        fail("canonical-json", "$", message);
    }
}

auto loadSchemaArtifact(const Path& path) -> LoadedArtifact {
    constexpr std::size_t maximumSchemaBytes = std::size_t{2} * 1024U * 1024U;
    constexpr std::size_t maximumSchemaStringBytes = std::size_t{1} * 1024U * 1024U;
    auto encoded = readBounded(path, maximumSchemaBytes);
    try {
        auto value = json::parse(encoded, {.maximumBytes = maximumSchemaBytes,
                                           .maximumDepth = 128,
                                           .maximumValues = 1'000'000,
                                           .maximumContainerEntries = 1'000'000,
                                           .maximumStringBytes = maximumSchemaStringBytes});
        if (!value.isObject()) {
            fail("schema-shape", "$", "schema root must be an object");
        }
        return {std::move(value), std::move(encoded)};
    } catch (const json::ParseError& error) {
        const std::string_view message = error.what();
        if (message.find("BOM") != std::string_view::npos) {
            fail("utf8-bom", path.string(), message);
        }
        if (message.find("duplicate decoded") != std::string_view::npos) {
            fail("duplicate-member", path.string(), message);
        }
        fail("schema-json", path.string(), message);
    }
}

} // namespace bloom::quality::dependencies
