#include <bloom/scripting/json_script.hpp>

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <map>
#include <string>
#include <type_traits>
#include <variant>

namespace bloom::scripting {
namespace {

struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue, std::less<>>;

struct JsonValue final {
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, JsonArray,
                                 JsonObject>;
    Storage storage = nullptr;
};

class Parser final {
  public:
    explicit Parser(std::string_view input) : input_(input) {}

    [[nodiscard]] std::optional<JsonValue> parse(ScriptDiagnostic& diagnostic) {
        skipWhitespace();
        auto value = parseValue(diagnostic);
        if (!value.has_value()) {
            return std::nullopt;
        }
        skipWhitespace();
        if (offset_ != input_.size()) {
            fail(diagnostic, "trailing-data", "Unexpected data after the JSON value");
            return std::nullopt;
        }
        return value;
    }

  private:
    void skipWhitespace() noexcept {
        while (offset_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[offset_])) != 0) {
            ++offset_;
        }
    }

    void fail(ScriptDiagnostic& diagnostic, std::string code, std::string message) const {
        diagnostic = {
            .code = std::move(code), .byteOffset = offset_, .message = std::move(message)};
    }

    [[nodiscard]] bool consume(const char expected) noexcept {
        if (offset_ >= input_.size() || input_[offset_] != expected) {
            return false;
        }
        ++offset_;
        return true;
    }

    [[nodiscard]] std::optional<JsonValue> parseValue(ScriptDiagnostic& diagnostic) {
        skipWhitespace();
        if (offset_ >= input_.size()) {
            fail(diagnostic, "unexpected-end", "Expected a JSON value");
            return std::nullopt;
        }
        switch (input_[offset_]) {
        case '{':
            return parseObject(diagnostic);
        case '[':
            return parseArray(diagnostic);
        case '"': {
            auto value = parseString(diagnostic);
            return value.has_value() ? std::optional<JsonValue>(JsonValue{std::move(*value)})
                                     : std::nullopt;
        }
        case 't':
            return parseLiteral("true", JsonValue{true}, diagnostic);
        case 'f':
            return parseLiteral("false", JsonValue{false}, diagnostic);
        case 'n':
            return parseLiteral("null", JsonValue{nullptr}, diagnostic);
        default:
            return parseNumber(diagnostic);
        }
    }

    [[nodiscard]] std::optional<JsonValue> parseLiteral(std::string_view literal, JsonValue value,
                                                        ScriptDiagnostic& diagnostic) {
        if (input_.substr(offset_, literal.size()) != literal) {
            fail(diagnostic, "invalid-literal", "Invalid JSON literal");
            return std::nullopt;
        }
        offset_ += literal.size();
        return value;
    }

    [[nodiscard]] std::optional<std::string> parseString(ScriptDiagnostic& diagnostic) {
        if (!consume('"')) {
            fail(diagnostic, "invalid-string", "Expected a string");
            return std::nullopt;
        }
        std::string result;
        while (offset_ < input_.size()) {
            const char character = input_[offset_++];
            if (character == '"') {
                return result;
            }
            if (static_cast<unsigned char>(character) < 0x20U) {
                fail(diagnostic, "invalid-string", "Control characters are not allowed in strings");
                return std::nullopt;
            }
            if (character != '\\') {
                result.push_back(character);
                continue;
            }
            if (offset_ >= input_.size()) {
                fail(diagnostic, "invalid-escape", "Incomplete string escape");
                return std::nullopt;
            }
            const char escaped = input_[offset_++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                result.push_back(escaped);
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u': {
                if (offset_ + 4 > input_.size()) {
                    fail(diagnostic, "invalid-unicode", "Incomplete Unicode escape");
                    return std::nullopt;
                }
                unsigned value = 0;
                for (int index = 0; index < 4; ++index) {
                    const char digit = input_[offset_++];
                    value <<= 4U;
                    if (digit >= '0' && digit <= '9') {
                        value += static_cast<unsigned>(digit - '0');
                    } else if (digit >= 'a' && digit <= 'f') {
                        value += static_cast<unsigned>(digit - 'a' + 10);
                    } else if (digit >= 'A' && digit <= 'F') {
                        value += static_cast<unsigned>(digit - 'A' + 10);
                    } else {
                        fail(diagnostic, "invalid-unicode", "Invalid Unicode escape");
                        return std::nullopt;
                    }
                }
                if (value > 0x7FU) {
                    if (value <= 0x7FFU) {
                        result.push_back(static_cast<char>(0xC0U | (value >> 6U)));
                        result.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
                    } else {
                        result.push_back(static_cast<char>(0xE0U | (value >> 12U)));
                        result.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
                        result.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
                    }
                } else {
                    result.push_back(static_cast<char>(value));
                }
                break;
            }
            default:
                fail(diagnostic, "invalid-escape", "Unknown string escape");
                return std::nullopt;
            }
        }
        fail(diagnostic, "unexpected-end", "Unterminated string");
        return std::nullopt;
    }

    [[nodiscard]] std::optional<JsonValue> parseObject(ScriptDiagnostic& diagnostic) {
        static_cast<void>(consume('{'));
        JsonObject result;
        skipWhitespace();
        if (consume('}')) {
            return JsonValue{std::move(result)};
        }
        while (true) {
            skipWhitespace();
            auto key = parseString(diagnostic);
            if (!key.has_value()) {
                return std::nullopt;
            }
            skipWhitespace();
            if (!consume(':')) {
                fail(diagnostic, "invalid-object", "Expected ':' after object key");
                return std::nullopt;
            }
            auto value = parseValue(diagnostic);
            if (!value.has_value()) {
                return std::nullopt;
            }
            if (!result.emplace(std::move(*key), std::move(*value)).second) {
                fail(diagnostic, "duplicate-key", "Duplicate object key");
                return std::nullopt;
            }
            skipWhitespace();
            if (consume('}')) {
                return JsonValue{std::move(result)};
            }
            if (!consume(',')) {
                fail(diagnostic, "invalid-object", "Expected ',' or '}' in object");
                return std::nullopt;
            }
        }
    }

    [[nodiscard]] std::optional<JsonValue> parseArray(ScriptDiagnostic& diagnostic) {
        static_cast<void>(consume('['));
        JsonArray result;
        skipWhitespace();
        if (consume(']')) {
            return JsonValue{std::move(result)};
        }
        while (true) {
            auto value = parseValue(diagnostic);
            if (!value.has_value()) {
                return std::nullopt;
            }
            result.push_back(std::move(*value));
            skipWhitespace();
            if (consume(']')) {
                return JsonValue{std::move(result)};
            }
            if (!consume(',')) {
                fail(diagnostic, "invalid-array", "Expected ',' or ']' in array");
                return std::nullopt;
            }
        }
    }

    [[nodiscard]] std::optional<JsonValue> parseNumber(ScriptDiagnostic& diagnostic) {
        const auto start = offset_;
        if (offset_ < input_.size() && (input_[offset_] == '-' || input_[offset_] == '+')) {
            ++offset_;
        }
        while (offset_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[offset_])) != 0) {
            ++offset_;
        }
        bool floating = false;
        if (offset_ < input_.size() && input_[offset_] == '.') {
            floating = true;
            ++offset_;
            while (offset_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[offset_])) != 0) {
                ++offset_;
            }
        }
        if (offset_ < input_.size() && (input_[offset_] == 'e' || input_[offset_] == 'E')) {
            floating = true;
            ++offset_;
            if (offset_ < input_.size() && (input_[offset_] == '-' || input_[offset_] == '+')) {
                ++offset_;
            }
            while (offset_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[offset_])) != 0) {
                ++offset_;
            }
        }
        if (start == offset_) {
            fail(diagnostic, "invalid-number", "Expected a JSON number");
            return std::nullopt;
        }
        const auto token = input_.substr(start, offset_ - start);
        if (floating) {
            char* end = nullptr;
            const auto value = std::strtod(std::string(token).c_str(), &end);
            if (end == nullptr) {
                fail(diagnostic, "invalid-number", "Invalid JSON number");
                return std::nullopt;
            }
            return JsonValue{value};
        }
        std::int64_t value = 0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
            fail(diagnostic, "invalid-number", "Integer is outside the supported range");
            return std::nullopt;
        }
        return JsonValue{value};
    }

    std::string_view input_;
    std::size_t offset_ = 0;
};

[[nodiscard]] std::optional<Value> convert(const JsonValue& json) {
    return std::visit(
        [](const auto& value) -> std::optional<Value> {
            using Type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Type, JsonArray>) {
                ValueArray result;
                result.reserve(value.size());
                for (const auto& element : value) {
                    const auto converted = convert(element);
                    if (!converted.has_value()) {
                        return std::nullopt;
                    }
                    result.push_back(*converted);
                }
                return Value{std::move(result)};
            } else if constexpr (std::is_same_v<Type, JsonObject> ||
                                 std::is_same_v<Type, std::nullptr_t>) {
                return std::nullopt;
            } else {
                return Value{value};
            }
        },
        json.storage);
}

} // namespace

ScriptParseResult parseScriptJson(const std::string_view json) {
    Parser parser(json);
    ScriptDiagnostic diagnostic;
    auto root = parser.parse(diagnostic);
    if (!root.has_value()) {
        ScriptParseResult result;
        result.diagnostic_ = std::move(diagnostic);
        return result;
    }
    const auto* array = std::get_if<JsonArray>(&root->storage);
    if (array == nullptr) {
        ScriptParseResult result;
        result.diagnostic_ = ScriptDiagnostic{.code = "root-not-array",
                                              .byteOffset = 0,
                                              .message = "Script root must be a JSON array"};
        return result;
    }

    ScriptParseResult result;
    for (std::size_t index = 0; index < array->size(); ++index) {
        const auto* object = std::get_if<JsonObject>(&(*array)[index].storage);
        if (object == nullptr) {
            result.diagnostic_ = ScriptDiagnostic{.code = "transaction-not-object",
                                                  .byteOffset = index,
                                                  .message = "Each script item "
                                                             "must be an object"};
            return result;
        }
        const auto operation = object->find("op");
        const auto args = object->find("args");
        if (operation == object->end() || args == object->end()) {
            result.diagnostic_ = ScriptDiagnostic{.code = "transaction-shape",
                                                  .byteOffset = index,
                                                  .message = "Each item requires 'op' and 'args'"};
            return result;
        }
        const auto* operationText = std::get_if<std::string>(&operation->second.storage);
        const auto* argsObject = std::get_if<JsonObject>(&args->second.storage);
        if (operationText == nullptr || argsObject == nullptr) {
            result.diagnostic_ =
                ScriptDiagnostic{.code = "transaction-types",
                                 .byteOffset = index,
                                 .message = "'op' must be a string and 'args' an object"};
            return result;
        }
        ScriptTransaction transaction{.operation = *operationText, .arguments = {}};
        for (const auto& [name, value] : *argsObject) {
            const auto converted = convert(value);
            if (!converted.has_value()) {
                result.diagnostic_ =
                    ScriptDiagnostic{.code = "unsupported-value",
                                     .byteOffset = index,
                                     .message = "Argument '" + name + "' is not a supported value"};
                return result;
            }
            transaction.arguments.emplace(name, *converted);
        }
        result.transactions_.push_back(std::move(transaction));
    }
    return result;
}

} // namespace bloom::scripting
