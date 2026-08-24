#include <bloom/core/rational_time.hpp>

#include <cstdint>
#include <numeric>

namespace {

[[nodiscard]] constexpr std::uint64_t magnitude(const std::int64_t value) noexcept {
    if (value >= 0) {
        return static_cast<std::uint64_t>(value);
    }

    return static_cast<std::uint64_t>(-(value + 1)) + 1;
}

[[nodiscard]] std::strong_ordering reverse(const std::strong_ordering ordering) noexcept {
    if (ordering == std::strong_ordering::less) {
        return std::strong_ordering::greater;
    }
    if (ordering == std::strong_ordering::greater) {
        return std::strong_ordering::less;
    }
    return std::strong_ordering::equal;
}

[[nodiscard]] std::strong_ordering comparePositive(std::uint64_t lhsNumerator,
                                                   std::uint64_t lhsDenominator,
                                                   std::uint64_t rhsNumerator,
                                                   std::uint64_t rhsDenominator) noexcept {
    bool invert = false;

    while (true) {
        const auto lhsWhole = lhsNumerator / lhsDenominator;
        const auto rhsWhole = rhsNumerator / rhsDenominator;
        if (lhsWhole != rhsWhole) {
            const auto result = lhsWhole <=> rhsWhole;
            return invert ? reverse(result) : result;
        }

        const auto lhsRemainder = lhsNumerator % lhsDenominator;
        const auto rhsRemainder = rhsNumerator % rhsDenominator;
        if (lhsRemainder == 0 || rhsRemainder == 0) {
            std::strong_ordering result = std::strong_ordering::equal;
            if (lhsRemainder == 0 && rhsRemainder != 0) {
                result = std::strong_ordering::less;
            } else if (lhsRemainder != 0 && rhsRemainder == 0) {
                result = std::strong_ordering::greater;
            }
            return invert ? reverse(result) : result;
        }

        lhsNumerator = lhsDenominator;
        lhsDenominator = lhsRemainder;
        rhsNumerator = rhsDenominator;
        rhsDenominator = rhsRemainder;
        invert = !invert;
    }
}

} // namespace

namespace bloom::core {

std::optional<RationalTime> RationalTime::create(const std::int64_t numerator,
                                                 const std::int64_t denominator) noexcept {
    if (denominator <= 0) {
        return std::nullopt;
    }

    const auto divisor = std::gcd(magnitude(numerator), static_cast<std::uint64_t>(denominator));
    const auto signedDivisor = static_cast<std::int64_t>(divisor);
    return RationalTime(numerator / signedDivisor, denominator / signedDivisor);
}

double RationalTime::toSeconds() const noexcept {
    return static_cast<double>(numerator_) / static_cast<double>(denominator_);
}

std::strong_ordering operator<=>(const RationalTime& lhs, const RationalTime& rhs) noexcept {
    const bool lhsNegative = lhs.numerator_ < 0;
    const bool rhsNegative = rhs.numerator_ < 0;
    if (lhsNegative != rhsNegative) {
        return lhsNegative ? std::strong_ordering::less : std::strong_ordering::greater;
    }

    const auto result =
        comparePositive(magnitude(lhs.numerator_), static_cast<std::uint64_t>(lhs.denominator_),
                        magnitude(rhs.numerator_), static_cast<std::uint64_t>(rhs.denominator_));
    return lhsNegative ? reverse(result) : result;
}

} // namespace bloom::core
