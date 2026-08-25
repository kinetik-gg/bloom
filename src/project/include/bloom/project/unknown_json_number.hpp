#pragma once

#include <bloom/project/canonical_decimal.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

namespace bloom::project {

enum class UnknownJsonNumberKind : std::uint8_t {
    Integer,
    Float64,
};

// The complete lossless editable subset for numbers owned by unknown additive JSON members.
// Float64 payloads are stored as bits so signed zero and every other finite value remain exact.
class UnknownJsonNumber final {
  public:
    [[nodiscard]] constexpr UnknownJsonNumberKind kind() const noexcept { return kind_; }

    [[nodiscard]] constexpr std::optional<std::int64_t> integerValue() const noexcept {
        if (kind_ != UnknownJsonNumberKind::Integer) {
            return std::nullopt;
        }
        return std::bit_cast<std::int64_t>(payload_);
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> float64Bits() const noexcept {
        if (kind_ != UnknownJsonNumberKind::Float64) {
            return std::nullopt;
        }
        return payload_;
    }

    friend constexpr bool operator==(const UnknownJsonNumber&,
                                     const UnknownJsonNumber&) noexcept = default;

  private:
    friend CanonicalDecimalResult<UnknownJsonNumber>
        parseUnknownJsonNumber(std::string_view) noexcept;

    constexpr UnknownJsonNumber(const UnknownJsonNumberKind kind,
                                const std::uint64_t payload) noexcept
        : payload_(payload), kind_(kind) {}

    std::uint64_t payload_ = 0;
    UnknownJsonNumberKind kind_ = UnknownJsonNumberKind::Integer;
};

class UnknownJsonNumberText final {
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
    friend UnknownJsonNumberText formatUnknownJsonNumber(const UnknownJsonNumber&) noexcept;

    constexpr UnknownJsonNumberText() noexcept = default;

    std::array<char, 25> characters_{};
    std::uint8_t size_ = 0;
};

using UnknownJsonNumberResult = CanonicalDecimalResult<UnknownJsonNumber>;

[[nodiscard]] UnknownJsonNumberResult parseUnknownJsonNumber(std::string_view text) noexcept;
[[nodiscard]] UnknownJsonNumberText
formatUnknownJsonNumber(const UnknownJsonNumber& value) noexcept;

static_assert(std::is_trivially_copyable_v<UnknownJsonNumber>);
static_assert(std::is_trivially_copyable_v<UnknownJsonNumberText>);
static_assert(sizeof(UnknownJsonNumber) <= 16);
static_assert(sizeof(UnknownJsonNumberText) <= 32);

} // namespace bloom::project
