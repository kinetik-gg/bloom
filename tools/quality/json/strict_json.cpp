#include "strict_json.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

namespace bloom::quality::json {
namespace {

[[nodiscard]] auto describeByte(const unsigned char byte) -> std::string {
    constexpr char digits[] = "0123456789abcdef";
    std::string result{"0x00"};
    result[2] = digits[(byte >> 4U) & 0x0FU];
    result[3] = digits[byte & 0x0FU];
    return result;
}

class Parser final {
  public:
    Parser(const std::string_view input, const ParseLimits& limits)
        : input_(input), limits_(limits) {
        if (input.size() > limits.maximumBytes) {
            throw ParseError("JSON input exceeds the byte limit");
        }
        if (input.starts_with("\xEF\xBB\xBF")) {
            throw ParseError("JSON input begins with a UTF-8 BOM");
        }
        if (limits.maximumDepth == 0 || limits.maximumValues == 0 ||
            limits.maximumContainerEntries == 0 || limits.maximumStringBytes == 0) {
            throw ParseError("JSON parse limits must be positive");
        }
    }

    [[nodiscard]] auto run() -> Value {
        skipWhitespace();
        auto result = parseValue(1);
        skipWhitespace();
        if (!atEnd()) {
            fail("unexpected trailing content");
        }
        return result;
    }

  private:
    [[noreturn]] void fail(const std::string_view message) const {
        throw ParseError("JSON byte " + std::to_string(position_) + ": " + std::string{message});
    }

    [[nodiscard]] auto atEnd() const noexcept -> bool { return position_ == input_.size(); }

    [[nodiscard]] auto peek() const -> unsigned char {
        if (atEnd()) {
            fail("unexpected end of input");
        }
        return static_cast<unsigned char>(input_[position_]);
    }

    [[nodiscard]] auto take() -> unsigned char {
        const auto result = peek();
        ++position_;
        return result;
    }

    void expect(const unsigned char expected) {
        if (take() != expected) {
            fail("expected " + describeByte(expected));
        }
    }

    void skipWhitespace() noexcept {
        while (!atEnd()) {
            const auto byte = static_cast<unsigned char>(input_[position_]);
            if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') {
                return;
            }
            ++position_;
        }
    }

    void countValue() {
        if (valueCount_ == limits_.maximumValues) {
            fail("JSON value count exceeds the configured limit");
        }
        ++valueCount_;
    }

    [[nodiscard]] auto parseValue(const std::size_t depth) -> Value {
        if (depth > limits_.maximumDepth) {
            fail("JSON nesting depth exceeds the configured limit");
        }
        countValue();
        switch (peek()) {
        case 'n':
            parseKeyword("null");
            return Value{nullptr};
        case 'f':
            parseKeyword("false");
            return Value{false};
        case 't':
            parseKeyword("true");
            return Value{true};
        case '"':
            return Value{parseString()};
        case '[':
            return parseArray(depth);
        case '{':
            return parseObject(depth);
        default:
            if (peek() == '-' || isDigit(peek())) {
                return Value{Number{parseNumber()}};
            }
            fail("expected a JSON value");
        }
    }

    void parseKeyword(const std::string_view keyword) {
        if (input_.substr(position_, keyword.size()) != keyword) {
            fail("invalid JSON keyword");
        }
        position_ += keyword.size();
    }

    [[nodiscard]] static auto isDigit(const unsigned char byte) noexcept -> bool {
        return byte >= '0' && byte <= '9';
    }

    [[nodiscard]] auto parseNumber() -> std::string {
        const auto begin = position_;
        if (peek() == '-') {
            ++position_;
            if (atEnd()) {
                fail("a minus sign must be followed by a number");
            }
        }
        if (peek() == '0') {
            ++position_;
            if (!atEnd() && isDigit(peek())) {
                fail("a JSON number cannot have a leading zero");
            }
        } else {
            if (peek() < '1' || peek() > '9') {
                fail("expected an integer component");
            }
            while (!atEnd() && isDigit(peek())) {
                ++position_;
            }
        }
        if (!atEnd() && peek() == '.') {
            ++position_;
            if (atEnd() || !isDigit(peek())) {
                fail("a decimal point must be followed by a digit");
            }
            while (!atEnd() && isDigit(peek())) {
                ++position_;
            }
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
            ++position_;
            if (!atEnd() && (peek() == '+' || peek() == '-')) {
                ++position_;
            }
            if (atEnd() || !isDigit(peek())) {
                fail("an exponent must contain a digit");
            }
            while (!atEnd() && isDigit(peek())) {
                ++position_;
            }
        }
        return std::string{input_.substr(begin, position_ - begin)};
    }

    [[nodiscard]] auto parseArray(const std::size_t depth) -> Value {
        expect('[');
        skipWhitespace();
        Value::Array result;
        if (!atEnd() && peek() == ']') {
            ++position_;
            return Value{std::move(result)};
        }
        while (true) {
            if (result.size() == limits_.maximumContainerEntries) {
                fail("JSON array length exceeds the configured limit");
            }
            result.push_back(parseValue(depth + 1));
            skipWhitespace();
            const auto separator = take();
            if (separator == ']') {
                return Value{std::move(result)};
            }
            if (separator != ',') {
                fail("expected ',' or ']' in array");
            }
            skipWhitespace();
        }
    }

    [[nodiscard]] auto parseObject(const std::size_t depth) -> Value {
        expect('{');
        skipWhitespace();
        Value::Object result;
        std::unordered_set<std::string> keys;
        if (!atEnd() && peek() == '}') {
            ++position_;
            return Value{std::move(result)};
        }
        while (true) {
            if (result.size() == limits_.maximumContainerEntries) {
                fail("JSON object length exceeds the configured limit");
            }
            if (peek() != '"') {
                fail("an object member name must be a string");
            }
            auto key = parseString();
            if (!keys.insert(key).second) {
                fail("duplicate decoded JSON object member '" + key + "'");
            }
            skipWhitespace();
            expect(':');
            skipWhitespace();
            result.emplace_back(std::move(key), parseValue(depth + 1));
            skipWhitespace();
            const auto separator = take();
            if (separator == '}') {
                return Value{std::move(result)};
            }
            if (separator != ',') {
                fail("expected ',' or '}' in object");
            }
            skipWhitespace();
        }
    }

    [[nodiscard]] static auto hexValue(const unsigned char byte) -> std::optional<std::uint16_t> {
        if (byte >= '0' && byte <= '9') {
            return static_cast<std::uint16_t>(byte - '0');
        }
        if (byte >= 'a' && byte <= 'f') {
            return static_cast<std::uint16_t>(byte - 'a' + 10U);
        }
        if (byte >= 'A' && byte <= 'F') {
            return static_cast<std::uint16_t>(byte - 'A' + 10U);
        }
        return std::nullopt;
    }

    [[nodiscard]] auto parseHexQuad() -> std::uint16_t {
        auto result = std::uint16_t{0};
        for (auto index = 0; index < 4; ++index) {
            if (atEnd()) {
                fail("truncated Unicode escape");
            }
            const auto digit = hexValue(take());
            if (!digit.has_value()) {
                fail("invalid hexadecimal digit in Unicode escape");
            }
            result = static_cast<std::uint16_t>((result << 4U) | *digit);
        }
        return result;
    }

    [[nodiscard]] auto parseEscapedScalar() -> std::uint32_t {
        const auto first = parseHexQuad();
        if (first >= 0xDC00U && first <= 0xDFFFU) {
            fail("lone low surrogate in Unicode escape");
        }
        if (first < 0xD800U || first > 0xDBFFU) {
            return first;
        }
        if (position_ + 2U > input_.size() || input_[position_] != '\\' ||
            input_[position_ + 1U] != 'u') {
            fail("lone high surrogate in Unicode escape");
        }
        position_ += 2U;
        const auto second = parseHexQuad();
        if (second < 0xDC00U || second > 0xDFFFU) {
            fail("high surrogate is not followed by a low surrogate");
        }
        return 0x10000U + ((static_cast<std::uint32_t>(first) - 0xD800U) << 10U) +
               (static_cast<std::uint32_t>(second) - 0xDC00U);
    }

    void appendScalar(std::string& output, const std::uint32_t scalar) const {
        if (scalar <= 0x7FU) {
            output.push_back(static_cast<char>(scalar));
        } else if (scalar <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (scalar >> 6U)));
            output.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
        } else if (scalar <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (scalar >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xF0U | (scalar >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((scalar >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
        }
        if (output.size() > limits_.maximumStringBytes) {
            fail("decoded JSON string exceeds the configured byte limit");
        }
    }

    [[nodiscard]] auto decodeUtf8Scalar() -> std::uint32_t {
        const auto first = take();
        if (first <= 0x7FU) {
            return first;
        }
        std::size_t continuationCount = 0;
        std::uint32_t result = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuationCount = 1;
            result = first & 0x1FU;
            minimum = 0x80U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuationCount = 2;
            result = first & 0x0FU;
            minimum = 0x800U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuationCount = 3;
            result = first & 0x07U;
            minimum = 0x10000U;
        } else {
            fail("invalid UTF-8 leading byte " + describeByte(first));
        }
        for (std::size_t index = 0; index < continuationCount; ++index) {
            if (atEnd()) {
                fail("truncated UTF-8 sequence");
            }
            const auto continuation = take();
            if ((continuation & 0xC0U) != 0x80U) {
                fail("invalid UTF-8 continuation byte " + describeByte(continuation));
            }
            result = (result << 6U) | (continuation & 0x3FU);
        }
        if (result < minimum || result > 0x10FFFFU || (result >= 0xD800U && result <= 0xDFFFU)) {
            fail("invalid UTF-8 scalar sequence");
        }
        return result;
    }

    [[nodiscard]] auto parseString() -> std::string {
        expect('"');
        std::string result;
        while (true) {
            if (atEnd()) {
                fail("unterminated JSON string");
            }
            const auto byte = peek();
            if (byte == '"') {
                ++position_;
                return result;
            }
            if (byte < 0x20U) {
                fail("unescaped control byte in JSON string");
            }
            if (byte == '\\') {
                position_ += 1U;
                if (atEnd()) {
                    fail("truncated JSON escape");
                }
                const auto escape = take();
                switch (escape) {
                case '"':
                case '\\':
                case '/':
                    appendScalar(result, escape);
                    break;
                case 'b':
                    appendScalar(result, '\b');
                    break;
                case 'f':
                    appendScalar(result, '\f');
                    break;
                case 'n':
                    appendScalar(result, '\n');
                    break;
                case 'r':
                    appendScalar(result, '\r');
                    break;
                case 't':
                    appendScalar(result, '\t');
                    break;
                case 'u':
                    appendScalar(result, parseEscapedScalar());
                    break;
                default:
                    fail("invalid JSON escape");
                }
                continue;
            }
            appendScalar(result, decodeUtf8Scalar());
        }
    }

    std::string_view input_;
    ParseLimits limits_;
    std::size_t position_{0};
    std::size_t valueCount_{0};
};

[[nodiscard]] auto objectExactEqual(const Value::Object& left, const Value::Object& right) -> bool {
    if (left.size() != right.size()) {
        return false;
    }
    return std::ranges::all_of(left, [&right](const auto& member) {
        const auto candidate = std::ranges::find_if(
            right, [&member](const auto& other) { return other.first == member.first; });
        return candidate != right.end() && exactEqual(member.second, candidate->second);
    });
}

} // namespace

Value::Value(std::nullptr_t value) : storage_(value) {}
Value::Value(const bool value) : storage_(value) {}
Value::Value(Number value) : storage_(std::move(value)) {}
Value::Value(std::string value) : storage_(std::move(value)) {}
Value::Value(Array value) : storage_(std::move(value)) {}
Value::Value(Object value) : storage_(std::move(value)) {}

auto Value::storage() const noexcept -> const Storage& { return storage_; }
auto Value::isNull() const noexcept -> bool {
    return std::holds_alternative<std::nullptr_t>(storage_);
}
auto Value::isBoolean() const noexcept -> bool { return std::holds_alternative<bool>(storage_); }
auto Value::isNumber() const noexcept -> bool { return std::holds_alternative<Number>(storage_); }
auto Value::isString() const noexcept -> bool {
    return std::holds_alternative<std::string>(storage_);
}
auto Value::isArray() const noexcept -> bool { return std::holds_alternative<Array>(storage_); }
auto Value::isObject() const noexcept -> bool { return std::holds_alternative<Object>(storage_); }

auto Value::asBoolean() const -> bool { return std::get<bool>(storage_); }
auto Value::asNumber() const -> const Number& { return std::get<Number>(storage_); }
auto Value::asString() const -> const std::string& { return std::get<std::string>(storage_); }
auto Value::asArray() const -> const Array& { return std::get<Array>(storage_); }
auto Value::asArray() -> Array& { return std::get<Array>(storage_); }
auto Value::asObject() const -> const Object& { return std::get<Object>(storage_); }
auto Value::asObject() -> Object& { return std::get<Object>(storage_); }

auto Value::find(const std::string_view key) const -> const Value* {
    if (!isObject()) {
        return nullptr;
    }
    const auto iterator =
        std::ranges::find_if(asObject(), [key](const auto& member) { return member.first == key; });
    return iterator == asObject().end() ? nullptr : &iterator->second;
}

auto Value::find(const std::string_view key) -> Value* {
    return const_cast<Value*>(std::as_const(*this).find(key));
}

auto Value::at(const std::string_view key) const -> const Value& {
    const auto* result = find(key);
    if (result == nullptr) {
        throw std::out_of_range("missing JSON object member '" + std::string{key} + "'");
    }
    return *result;
}

auto Value::at(const std::string_view key) -> Value& {
    return const_cast<Value&>(std::as_const(*this).at(key));
}

ParseError::ParseError(const std::string& message) : std::runtime_error(message) {}

auto parse(const std::string_view encoded, const ParseLimits& limits) -> Value {
    return Parser{encoded, limits}.run();
}

auto parseFile(const std::filesystem::path& path, const ParseLimits& limits) -> Value {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw ParseError("could not stat " + path.string() + ": " + error.message());
    }
    if (size > limits.maximumBytes) {
        throw ParseError(path.string() + " exceeds the JSON byte limit");
    }
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        throw ParseError("could not read " + path.string());
    }
    std::string encoded(static_cast<std::size_t>(size), '\0');
    stream.read(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    if (!stream && !encoded.empty()) {
        throw ParseError("could not read all bytes from " + path.string());
    }
    if (stream.peek() != std::char_traits<char>::eof()) {
        throw ParseError(path.string() + " changed while it was being read");
    }
    try {
        return parse(encoded, limits);
    } catch (const ParseError& parseError) {
        throw ParseError(path.string() + ": " + parseError.what());
    }
}

auto exactEqual(const Value& left, const Value& right) -> bool {
    if (left.storage().index() != right.storage().index()) {
        return false;
    }
    if (left.isObject()) {
        return objectExactEqual(left.asObject(), right.asObject());
    }
    if (left.isNull()) {
        return true;
    }
    if (left.isBoolean()) {
        return left.asBoolean() == right.asBoolean();
    }
    if (left.isNumber()) {
        return left.asNumber() == right.asNumber();
    }
    if (left.isString()) {
        return left.asString() == right.asString();
    }
    const auto& leftArray = left.asArray();
    const auto& rightArray = right.asArray();
    return leftArray.size() == rightArray.size() &&
           std::ranges::equal(leftArray, rightArray, exactEqual);
}

auto isIntegerToken(const std::string_view token) noexcept -> bool {
    if (token.empty()) {
        return false;
    }
    auto position = std::size_t{0};
    if (token.front() == '-') {
        position = 1;
        if (position == token.size()) {
            return false;
        }
    }
    return std::ranges::all_of(token.substr(position), [](const char character) {
        return character >= '0' && character <= '9';
    });
}

auto asInt64(const Value& value) -> std::optional<std::int64_t> {
    if (!value.isNumber() || !isIntegerToken(value.asNumber().spelling)) {
        return std::nullopt;
    }
    std::int64_t result{};
    const auto& token = value.asNumber().spelling;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), result);
    if (error != std::errc{} || end != token.data() + token.size()) {
        return std::nullopt;
    }
    return result;
}

auto asUint64(const Value& value) -> std::optional<std::uint64_t> {
    if (!value.isNumber() || !isIntegerToken(value.asNumber().spelling) ||
        value.asNumber().spelling.starts_with('-')) {
        return std::nullopt;
    }
    std::uint64_t result{};
    const auto& token = value.asNumber().spelling;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), result);
    if (error != std::errc{} || end != token.data() + token.size()) {
        return std::nullopt;
    }
    return result;
}

} // namespace bloom::quality::json
