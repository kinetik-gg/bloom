#pragma once

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

namespace bloom::project {

enum class CanonicalDecimalError : std::uint8_t {
    None,
    InvalidLexicalForm,
    OutOfRange,
    ZeroNotAllowed,
    NotReduced,
    NonCanonicalZero,
    NonFinite,
    NonCanonical,
};

enum class CanonicalDecimalField : std::uint8_t {
    None,
    Value,
    Numerator,
    Denominator,
};

template <typename Value> class [[nodiscard]] CanonicalDecimalResult final {
    static_assert(std::is_trivially_copyable_v<Value>);

  public:
    [[nodiscard]] static constexpr CanonicalDecimalResult success(const Value value) noexcept {
        return CanonicalDecimalResult(value);
    }

    [[nodiscard]] static constexpr CanonicalDecimalResult
    failure(const CanonicalDecimalError error, const CanonicalDecimalField field) noexcept {
        return CanonicalDecimalResult(error, field);
    }

    [[nodiscard]] constexpr bool hasValue() const noexcept { return value_.has_value(); }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] constexpr const Value* value() const& noexcept {
        return value_.has_value() ? &*value_ : nullptr;
    }
    [[nodiscard]] constexpr const Value* value() const&& = delete;
    [[nodiscard]] constexpr CanonicalDecimalError error() const noexcept { return error_; }
    [[nodiscard]] constexpr CanonicalDecimalField field() const noexcept { return field_; }

  private:
    constexpr explicit CanonicalDecimalResult(const Value value) noexcept : value_(value) {}

    constexpr CanonicalDecimalResult(const CanonicalDecimalError error,
                                     const CanonicalDecimalField field) noexcept
        : error_(error), field_(field) {}

    std::optional<Value> value_;
    CanonicalDecimalError error_ = CanonicalDecimalError::None;
    CanonicalDecimalField field_ = CanonicalDecimalField::None;
};

struct CanonicalPositiveRatio final {
    std::uint32_t numerator = 1;
    std::uint32_t denominator = 1;

    friend constexpr bool operator==(const CanonicalPositiveRatio&,
                                     const CanonicalPositiveRatio&) noexcept = default;
};

class CanonicalDecimalText final {
  public:
    [[nodiscard]] constexpr std::string_view view() const& noexcept {
        return {characters_.data(), static_cast<std::size_t>(size_)};
    }
    [[nodiscard]] constexpr std::string_view view() const&& = delete;
    [[nodiscard]] constexpr const char* data() const& noexcept { return characters_.data(); }
    [[nodiscard]] constexpr const char* data() const&& = delete;
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return static_cast<std::size_t>(size_);
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

  private:
    friend CanonicalDecimalText formatCanonicalUInt64(std::uint64_t) noexcept;
    friend CanonicalDecimalText formatCanonicalInt64(std::int64_t) noexcept;

    constexpr CanonicalDecimalText() noexcept = default;

    std::array<char, 20> characters_{};
    std::uint8_t size_ = 0;
};

class CanonicalFloat64Text final {
  public:
    [[nodiscard]] constexpr std::string_view view() const& noexcept {
        return {characters_.data(), static_cast<std::size_t>(size_)};
    }
    [[nodiscard]] constexpr std::string_view view() const&& = delete;
    [[nodiscard]] constexpr const char* data() const& noexcept { return characters_.data(); }
    [[nodiscard]] constexpr const char* data() const&& = delete;
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return static_cast<std::size_t>(size_);
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

  private:
    friend CanonicalDecimalResult<CanonicalFloat64Text> formatCanonicalFloat64(double) noexcept;

    constexpr CanonicalFloat64Text() noexcept = default;

    std::array<char, 25> characters_{};
    std::uint8_t size_ = 0;
};

using CanonicalUInt64Result = CanonicalDecimalResult<std::uint64_t>;
using CanonicalUInt32Result = CanonicalDecimalResult<std::uint32_t>;
using CanonicalInt64Result = CanonicalDecimalResult<std::int64_t>;
using CanonicalRationalTimeResult = CanonicalDecimalResult<core::RationalTime>;
using CanonicalPositiveRatioResult = CanonicalDecimalResult<CanonicalPositiveRatio>;
using CanonicalPixelAspectRatioResult = CanonicalDecimalResult<core::PixelAspectRatio>;
using CanonicalFloat64Result = CanonicalDecimalResult<double>;
using CanonicalFloat64TextResult = CanonicalDecimalResult<CanonicalFloat64Text>;

[[nodiscard]] CanonicalUInt64Result parseCanonicalObjectId(std::string_view text) noexcept;
[[nodiscard]] CanonicalUInt64Result
parseCanonicalAllocatorHighWater(std::string_view text) noexcept;
[[nodiscard]] CanonicalInt64Result parseCanonicalInt64(std::string_view text) noexcept;
[[nodiscard]] CanonicalUInt32Result parseCanonicalJsonUInt32(std::string_view text,
                                                             std::uint32_t maximum) noexcept;
[[nodiscard]] CanonicalRationalTimeResult
parseCanonicalRationalTime(std::string_view numerator, std::string_view denominator) noexcept;
[[nodiscard]] CanonicalPositiveRatioResult
parseCanonicalPositiveRatio(std::string_view numerator, std::string_view denominator) noexcept;
[[nodiscard]] CanonicalPixelAspectRatioResult
parseCanonicalPixelAspectRatio(std::string_view numerator, std::string_view denominator) noexcept;

[[nodiscard]] CanonicalDecimalText formatCanonicalUInt64(std::uint64_t value) noexcept;
[[nodiscard]] CanonicalDecimalText formatCanonicalInt64(std::int64_t value) noexcept;
[[nodiscard]] CanonicalFloat64Result parseCanonicalFloat64(std::string_view text) noexcept;
[[nodiscard]] CanonicalFloat64TextResult formatCanonicalFloat64(double value) noexcept;

static_assert(std::is_trivially_copyable_v<CanonicalPositiveRatio>);
static_assert(std::is_trivially_copyable_v<CanonicalDecimalText>);
static_assert(std::is_trivially_copyable_v<CanonicalFloat64Text>);
static_assert(sizeof(CanonicalDecimalText) <= 24);
static_assert(sizeof(CanonicalFloat64Text) <= 32);

} // namespace bloom::project
