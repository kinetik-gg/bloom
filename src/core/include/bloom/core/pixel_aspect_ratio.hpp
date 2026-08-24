#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>

namespace bloom::core {

// Canonical positive rational ratio of pixel width to pixel height, shared by document and render
// values.
class PixelAspectRatio final {
  public:
    [[nodiscard]] static constexpr PixelAspectRatio square() noexcept {
        return PixelAspectRatio(1, 1);
    }

    [[nodiscard]] static constexpr std::optional<PixelAspectRatio>
    create(const std::uint64_t numerator, const std::uint64_t denominator) noexcept {
        if (numerator == 0 || denominator == 0) {
            return std::nullopt;
        }

        const auto divisor = std::gcd(numerator, denominator);
        const auto reducedNumerator = numerator / divisor;
        const auto reducedDenominator = denominator / divisor;
        constexpr auto maximumTerm = std::numeric_limits<std::uint32_t>::max();
        if (reducedNumerator > maximumTerm || reducedDenominator > maximumTerm) {
            return std::nullopt;
        }

        return PixelAspectRatio(static_cast<std::uint32_t>(reducedNumerator),
                                static_cast<std::uint32_t>(reducedDenominator));
    }

    [[nodiscard]] constexpr std::uint32_t numerator() const noexcept { return numerator_; }
    [[nodiscard]] constexpr std::uint32_t denominator() const noexcept { return denominator_; }

    friend constexpr bool operator==(const PixelAspectRatio&,
                                     const PixelAspectRatio&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const PixelAspectRatio& lhs,
                                                      const PixelAspectRatio& rhs) noexcept {
        const auto lhsScaled = static_cast<std::uint64_t>(lhs.numerator_) * rhs.denominator_;
        const auto rhsScaled = static_cast<std::uint64_t>(rhs.numerator_) * lhs.denominator_;
        return lhsScaled <=> rhsScaled;
    }

  private:
    constexpr PixelAspectRatio(const std::uint32_t numerator,
                               const std::uint32_t denominator) noexcept
        : numerator_(numerator), denominator_(denominator) {}

    std::uint32_t numerator_;
    std::uint32_t denominator_;
};

} // namespace bloom::core
