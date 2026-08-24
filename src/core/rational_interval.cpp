#include <bloom/core/rational_interval.hpp>

#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

struct UInt256 final {
    std::array<std::uint64_t, 4> limbs{};
};

struct DoubleWord final {
    std::uint64_t low = 0;
    std::uint64_t high = 0;
};

[[nodiscard]] constexpr std::uint64_t magnitude(const std::int64_t value) noexcept {
    return value >= 0 ? static_cast<std::uint64_t>(value)
                      : static_cast<std::uint64_t>(-(value + 1)) + 1;
}

[[nodiscard]] constexpr DoubleWord multiplyWords(const std::uint64_t left,
                                                 const std::uint64_t right) noexcept {
    constexpr auto mask = std::uint64_t{0xFFFF'FFFF};
    const auto leftLow = left & mask;
    const auto leftHigh = left >> 32U;
    const auto rightLow = right & mask;
    const auto rightHigh = right >> 32U;

    const auto first = leftLow * rightLow;
    const auto lowWord = first & mask;
    const auto firstCarry = first >> 32U;

    const auto second = (leftHigh * rightLow) + firstCarry;
    const auto secondLow = second & mask;
    const auto secondCarry = second >> 32U;

    const auto third = (leftLow * rightHigh) + secondLow;
    const auto high = (leftHigh * rightHigh) + secondCarry + (third >> 32U);
    const auto low = (third << 32U) + lowWord;
    return {low, high};
}

[[nodiscard]] constexpr UInt256 multiplyWordsToUInt256(const std::uint64_t left,
                                                       const std::uint64_t right) noexcept {
    const auto product = multiplyWords(left, right);
    return {{{product.low, product.high, 0, 0}}};
}

[[nodiscard]] constexpr std::strong_ordering compare(const UInt256& left,
                                                     const UInt256& right) noexcept {
    for (std::size_t index = left.limbs.size(); index > 0; --index) {
        const auto limb = index - 1;
        if (left.limbs[limb] != right.limbs[limb]) {
            return left.limbs[limb] <=> right.limbs[limb];
        }
    }
    return std::strong_ordering::equal;
}

[[nodiscard]] constexpr bool isZero(const UInt256& value) noexcept {
    return value.limbs[0] == 0 && value.limbs[1] == 0 && value.limbs[2] == 0 && value.limbs[3] == 0;
}

[[nodiscard]] constexpr UInt256 add(const UInt256& left, const UInt256& right) noexcept {
    UInt256 result;
    std::uint64_t carry = 0;
    for (std::size_t index = 0; index < result.limbs.size(); ++index) {
        const auto partial = left.limbs[index] + carry;
        const auto firstCarry = partial < left.limbs[index];
        const auto sum = partial + right.limbs[index];
        const auto secondCarry = sum < partial;
        result.limbs[index] = sum;
        carry = (firstCarry || secondCarry) ? 1U : 0U;
    }
    return result;
}

// Precondition: left >= right.
[[nodiscard]] constexpr UInt256 subtract(const UInt256& left, const UInt256& right) noexcept {
    UInt256 result;
    std::uint64_t borrow = 0;
    for (std::size_t index = 0; index < result.limbs.size(); ++index) {
        const auto partial = left.limbs[index] - borrow;
        const auto firstBorrow = left.limbs[index] < borrow;
        const auto difference = partial - right.limbs[index];
        const auto secondBorrow = partial < right.limbs[index];
        result.limbs[index] = difference;
        borrow = (firstBorrow || secondBorrow) ? 1U : 0U;
    }
    return result;
}

[[nodiscard]] constexpr UInt256 multiply(const UInt256& value,
                                         const std::uint64_t multiplier) noexcept {
    UInt256 result;
    std::uint64_t carry = 0;
    for (std::size_t index = 0; index < result.limbs.size(); ++index) {
        const auto product = multiplyWords(value.limbs[index], multiplier);
        const auto low = product.low + carry;
        const auto overflow = low < product.low;
        result.limbs[index] = low;
        carry = product.high + (overflow ? 1U : 0U);
    }
    return result;
}

[[nodiscard]] constexpr UInt256
positiveDifferenceNumerator(const bloom::core::RationalTime later,
                            const bloom::core::RationalTime earlier) noexcept {
    const auto laterProduct = multiplyWordsToUInt256(
        magnitude(later.numerator()), static_cast<std::uint64_t>(earlier.denominator()));
    const auto earlierProduct = multiplyWordsToUInt256(
        magnitude(earlier.numerator()), static_cast<std::uint64_t>(later.denominator()));

    if (later.numerator() >= 0 && earlier.numerator() < 0) {
        return add(laterProduct, earlierProduct);
    }
    if (later.numerator() < 0) {
        return subtract(earlierProduct, laterProduct);
    }
    return subtract(laterProduct, earlierProduct);
}

// Remainder is always less than denominator. This computes one binary quotient bit without ever
// materializing a possibly 257-bit doubled remainder.
[[nodiscard]] constexpr bool nextBinaryBit(UInt256& remainder,
                                           const UInt256& denominator) noexcept {
    const auto complement = subtract(denominator, remainder);
    if (compare(remainder, complement) != std::strong_ordering::less) {
        remainder = subtract(remainder, complement);
        return true;
    }
    remainder = add(remainder, remainder);
    return false;
}

[[nodiscard]] double roundedUnitRatio(UInt256 numerator, const UInt256& denominator) noexcept {
    std::int32_t exponent = -1;
    while (!nextBinaryBit(numerator, denominator)) {
        --exponent;
        if (exponent < -1022) {
            return -1.0;
        }
    }

    std::uint64_t significand = std::uint64_t{1} << 52U;
    for (std::uint32_t bit = 0; bit < 52; ++bit) {
        significand |= static_cast<std::uint64_t>(nextBinaryBit(numerator, denominator))
                       << (51U - bit);
    }

    const auto guard = nextBinaryBit(numerator, denominator);
    const auto sticky = !isZero(numerator);
    if (guard && (sticky || (significand & 1U) != 0U)) {
        ++significand;
        if (significand == (std::uint64_t{1} << 53U)) {
            significand >>= 1U;
            ++exponent;
        }
    }

    const auto biasedExponent = static_cast<std::uint64_t>(exponent + 1023);
    const auto fraction = significand - (std::uint64_t{1} << 52U);
    return std::bit_cast<double>((biasedExponent << 52U) | fraction);
}

} // namespace

namespace bloom::core {

RationalIntervalFactorResult rationalIntervalFactor(const RationalTime position,
                                                    const RationalTime start,
                                                    const RationalTime end) noexcept {
    if (start >= end) {
        return {0.0, RationalIntervalError::DegenerateInterval};
    }
    if (position < start || position > end) {
        return {0.0, RationalIntervalError::PositionOutsideInterval};
    }
    if (position == start) {
        return {0.0, RationalIntervalError::None};
    }
    if (position == end) {
        return {1.0, RationalIntervalError::None};
    }

    const auto offsetNumerator = positiveDifferenceNumerator(position, start);
    const auto spanNumerator = positiveDifferenceNumerator(end, start);

    // The common start denominator cancels exactly:
    // ((p - s) / (e - s)) = (offsetNumerator * e.den) /
    //                         (spanNumerator * p.den).
    const auto numerator = multiply(offsetNumerator, static_cast<std::uint64_t>(end.denominator()));
    const auto denominator =
        multiply(spanNumerator, static_cast<std::uint64_t>(position.denominator()));
    if (isZero(numerator) || isZero(denominator) ||
        compare(numerator, denominator) != std::strong_ordering::less) {
        return {0.0, RationalIntervalError::InternalRangeFailure};
    }

    const auto factor = roundedUnitRatio(numerator, denominator);
    if (factor < 0.0 || factor > 1.0) {
        return {0.0, RationalIntervalError::InternalRangeFailure};
    }
    return {factor, RationalIntervalError::None};
}

} // namespace bloom::core
