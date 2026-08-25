#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::quality::json {

// This bounded DOM is shared by repository quality tools only. It is deliberately independent of
// Bloom's production project reader and does not define a public project-format API.

struct Number final {
    std::string spelling;

    friend auto operator==(const Number&, const Number&) -> bool = default;
};

class Value final {
  public:
    using Array = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;
    using Storage = std::variant<std::nullptr_t, bool, Number, std::string, Array, Object>;

    explicit Value(std::nullptr_t value = nullptr);
    explicit Value(bool value);
    explicit Value(Number value);
    explicit Value(std::string value);
    explicit Value(Array value);
    explicit Value(Object value);

    [[nodiscard]] auto storage() const noexcept -> const Storage&;
    [[nodiscard]] auto isNull() const noexcept -> bool;
    [[nodiscard]] auto isBoolean() const noexcept -> bool;
    [[nodiscard]] auto isNumber() const noexcept -> bool;
    [[nodiscard]] auto isString() const noexcept -> bool;
    [[nodiscard]] auto isArray() const noexcept -> bool;
    [[nodiscard]] auto isObject() const noexcept -> bool;

    [[nodiscard]] auto asBoolean() const -> bool;
    [[nodiscard]] auto asNumber() const -> const Number&;
    [[nodiscard]] auto asString() const -> const std::string&;
    [[nodiscard]] auto asArray() const -> const Array&;
    [[nodiscard]] auto asArray() -> Array&;
    [[nodiscard]] auto asObject() const -> const Object&;
    [[nodiscard]] auto asObject() -> Object&;

    [[nodiscard]] auto find(std::string_view key) const -> const Value*;
    [[nodiscard]] auto find(std::string_view key) -> Value*;
    [[nodiscard]] auto at(std::string_view key) const -> const Value&;
    [[nodiscard]] auto at(std::string_view key) -> Value&;

  private:
    Storage storage_;
};

struct ParseLimits final {
    std::size_t maximumBytes{static_cast<std::size_t>(32) * 1024U * 1024U};
    std::size_t maximumDepth{128};
    std::size_t maximumValues{1'000'000};
    std::size_t maximumContainerEntries{1'000'000};
    std::size_t maximumStringBytes{static_cast<std::size_t>(16) * 1024U * 1024U};
};

class ParseError final : public std::runtime_error {
  public:
    explicit ParseError(const std::string& message);
};

[[nodiscard]] auto parse(std::string_view encoded, const ParseLimits& limits = {}) -> Value;
[[nodiscard]] auto parseFile(const std::filesystem::path& path, const ParseLimits& limits = {})
    -> Value;

[[nodiscard]] auto exactEqual(const Value& left, const Value& right) -> bool;
[[nodiscard]] auto isIntegerToken(std::string_view token) noexcept -> bool;
[[nodiscard]] auto asInt64(const Value& value) -> std::optional<std::int64_t>;
[[nodiscard]] auto asUint64(const Value& value) -> std::optional<std::uint64_t>;

} // namespace bloom::quality::json
