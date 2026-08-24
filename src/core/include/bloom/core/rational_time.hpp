#pragma once

#include <compare>
#include <cstdint>
#include <optional>

namespace bloom::core {

class RationalTime final {
  public:
    constexpr RationalTime() noexcept = default;

    [[nodiscard]] static std::optional<RationalTime> create(std::int64_t numerator,
                                                            std::int64_t denominator) noexcept;
    [[nodiscard]] static constexpr RationalTime fromInteger(std::int64_t value) noexcept {
        return RationalTime(value, 1);
    }

    [[nodiscard]] constexpr std::int64_t numerator() const noexcept { return numerator_; }
    [[nodiscard]] constexpr std::int64_t denominator() const noexcept { return denominator_; }
    [[nodiscard]] double toSeconds() const noexcept;

    friend constexpr bool operator==(const RationalTime&, const RationalTime&) noexcept = default;
    friend std::strong_ordering operator<=>(const RationalTime& lhs,
                                            const RationalTime& rhs) noexcept;

  private:
    constexpr RationalTime(std::int64_t numerator, std::int64_t denominator) noexcept
        : numerator_(numerator), denominator_(denominator) {}

    std::int64_t numerator_ = 0;
    std::int64_t denominator_ = 1;
};

} // namespace bloom::core
