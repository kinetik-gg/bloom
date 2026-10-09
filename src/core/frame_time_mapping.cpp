#include <bloom/core/frame_time_mapping.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>

namespace {

struct UInt128 final {
    std::array<std::uint64_t, 2> limbs{};
};

struct DoubleWord final {
    std::uint64_t low = 0;
    std::uint64_t high = 0;
};

struct DivisionResult final {
    std::uint64_t quotient = 0;
    UInt128 remainder{};
    bool quotientOverflow = false;
};

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
    return {(third << 32U) + lowWord, (leftHigh * rightHigh) + secondCarry + (third >> 32U)};
}

[[nodiscard]] constexpr UInt128 multiplyToUInt128(const std::uint64_t left,
                                                  const std::uint64_t right) noexcept {
    const auto product = multiplyWords(left, right);
    return {{{product.low, product.high}}};
}

[[nodiscard]] constexpr std::optional<UInt128> multiplyByWord(const UInt128& left,
                                                              const std::uint64_t right) noexcept {
    const auto low = multiplyWords(left.limbs[0], right);
    const auto high = multiplyWords(left.limbs[1], right);
    const auto upper = low.high + high.low;
    if (high.high != 0 || upper < low.high)
        return std::nullopt;
    return UInt128{{{low.low, upper}}};
}

[[nodiscard]] constexpr std::strong_ordering compare(const UInt128& left,
                                                     const UInt128& right) noexcept {
    if (left.limbs[1] != right.limbs[1]) {
        return left.limbs[1] <=> right.limbs[1];
    }
    return left.limbs[0] <=> right.limbs[0];
}

[[nodiscard]] constexpr UInt128 subtractOne(UInt128 value) noexcept {
    if (value.limbs[0] == 0) {
        --value.limbs[1];
    }
    --value.limbs[0];
    return value;
}

[[nodiscard]] constexpr UInt128 subtract(const UInt128& left, const UInt128& right) noexcept {
    const auto borrow = left.limbs[0] < right.limbs[0];
    return {
        {{left.limbs[0] - right.limbs[0], left.limbs[1] - right.limbs[1] - (borrow ? 1U : 0U)}}};
}

[[nodiscard]] constexpr UInt128 shiftLeftAndAddBit(const UInt128& value, const bool bit) noexcept {
    return {{{(value.limbs[0] << 1U) | (bit ? 1U : 0U),
              (value.limbs[1] << 1U) | (value.limbs[0] >> 63U)}}};
}

[[nodiscard]] constexpr bool bitAt(const UInt128& value, const std::size_t bit) noexcept {
    const auto limb = bit / 64U;
    const auto offset = bit % 64U;
    return ((value.limbs[limb] >> offset) & 1U) != 0;
}

// Divisors stay below 2^127; scaled playback-rate products are checked before division.
// Consequently, a remainder smaller than the divisor can be doubled without overflowing UInt128.
[[nodiscard]] constexpr DivisionResult divideToUInt64(const UInt128& numerator,
                                                      const UInt128& denominator) noexcept {
    DivisionResult result;
    for (std::size_t bit = 128; bit > 0; --bit) {
        result.remainder = shiftLeftAndAddBit(result.remainder, bitAt(numerator, bit - 1));
        if (compare(result.remainder, denominator) != std::strong_ordering::less) {
            result.remainder = subtract(result.remainder, denominator);
            if (bit > 64) {
                result.quotientOverflow = true;
            } else {
                result.quotient |= std::uint64_t{1} << (bit - 1);
            }
        }
    }
    return result;
}

[[nodiscard]] constexpr bool doubledAtLeast(const UInt128& left, const UInt128& right) noexcept {
    return compare(shiftLeftAndAddBit(left, false), right) != std::strong_ordering::less;
}

} // namespace

namespace bloom::core {

FrameTimeMappingCreateResult FrameTimeMapping::create(const RationalTime duration,
                                                      std::uint32_t rateNumerator,
                                                      std::uint32_t rateDenominator) noexcept {
    if (rateNumerator == 0 || rateDenominator == 0) {
        return {FrameTimeMapping(RationalTime{}, 1, 1, 0), FrameTimeMappingError::InvalidRate};
    }
    if (duration <= RationalTime{}) {
        return {FrameTimeMapping(RationalTime{}, 1, 1, 0),
                FrameTimeMappingError::NonPositiveDuration};
    }

    const auto rateDivisor = std::gcd(rateNumerator, rateDenominator);
    rateNumerator /= rateDivisor;
    rateDenominator /= rateDivisor;

    const auto frameRangeNumerator =
        multiplyToUInt128(static_cast<std::uint64_t>(duration.numerator()), rateNumerator);
    const auto frameRangeDenominator =
        multiplyToUInt128(static_cast<std::uint64_t>(duration.denominator()), rateDenominator);
    const auto maximum = divideToUInt64(subtractOne(frameRangeNumerator), frameRangeDenominator);
    if (maximum.quotientOverflow) {
        return {FrameTimeMapping(RationalTime{}, 1, 1, 0),
                FrameTimeMappingError::FrameIndexOverflow};
    }

    return {FrameTimeMapping(duration, rateNumerator, rateDenominator, maximum.quotient),
            FrameTimeMappingError::None};
}

FrameTimeResult FrameTimeMapping::timeForFrame(const std::uint64_t frameIndex) const noexcept {
    if (frameIndex > maximumFrameIndex_) {
        return {RationalTime{}, FrameTimeError::FrameOutsideRange};
    }

    const auto divisor = std::gcd(frameIndex, static_cast<std::uint64_t>(rateNumerator_));
    const auto reducedIndex = frameIndex / divisor;
    const auto reducedDenominator = static_cast<std::uint64_t>(rateNumerator_) / divisor;
    const auto maximumNumerator =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (reducedIndex > maximumNumerator / rateDenominator_) {
        return {RationalTime{}, FrameTimeError::TimeRepresentationOverflow};
    }

    const auto numerator = static_cast<std::int64_t>(reducedIndex * rateDenominator_);
    const auto time =
        RationalTime::create(numerator, static_cast<std::int64_t>(reducedDenominator));
    if (!time.has_value()) {
        return {RationalTime{}, FrameTimeError::TimeRepresentationOverflow};
    }
    return {*time, FrameTimeError::None};
}

std::uint64_t FrameTimeMapping::nearestFrameIndex(const RationalTime time) const noexcept {
    if (time <= RationalTime{}) {
        return 0;
    }
    if (time >= duration_) {
        return maximumFrameIndex_;
    }

    const auto numerator =
        multiplyToUInt128(static_cast<std::uint64_t>(time.numerator()), rateNumerator_);
    const auto denominator =
        multiplyToUInt128(static_cast<std::uint64_t>(time.denominator()), rateDenominator_);
    const auto result = divideToUInt64(numerator, denominator);

    // Since time is below duration and the mapping's maximum fits uint64_t, this quotient must fit.
    // Keep the clamp defensive so malformed internal state cannot wrap a frame index.
    if (result.quotientOverflow || result.quotient >= maximumFrameIndex_) {
        return maximumFrameIndex_;
    }
    return doubledAtLeast(result.remainder, denominator) ? result.quotient + 1 : result.quotient;
}

std::optional<std::uint64_t> FrameTimeMapping::frameOffsetForElapsedNanoseconds(
    const std::uint64_t elapsedNanoseconds) const noexcept {
    constexpr auto nanosecondsPerSecond = std::uint64_t{1'000'000'000};
    const auto numerator =
        multiplyToUInt128(elapsedNanoseconds, static_cast<std::uint64_t>(rateNumerator_));
    const auto denominator =
        multiplyToUInt128(nanosecondsPerSecond, static_cast<std::uint64_t>(rateDenominator_));
    const auto result = divideToUInt64(numerator, denominator);
    if (result.quotientOverflow) {
        return std::nullopt;
    }
    return result.quotient;
}

std::optional<std::uint64_t>
FrameTimeMapping::frameOffsetForTime(const RationalTime time) const noexcept {
    if (time < RationalTime{})
        return std::nullopt;
    const auto numerator =
        multiplyToUInt128(static_cast<std::uint64_t>(time.numerator()), rateNumerator_);
    const auto denominator =
        multiplyToUInt128(static_cast<std::uint64_t>(time.denominator()), rateDenominator_);
    const auto result = divideToUInt64(numerator, denominator);
    return result.quotientOverflow ? std::nullopt : std::optional{result.quotient};
}

std::optional<std::uint64_t>
FrameTimeMapping::nanosecondsUntilNextFrame(const RationalTime time) const noexcept {
    if (time < RationalTime{})
        return std::nullopt;
    const auto numerator =
        multiplyToUInt128(static_cast<std::uint64_t>(time.numerator()), rateNumerator_);
    const auto denominator =
        multiplyToUInt128(static_cast<std::uint64_t>(time.denominator()), rateDenominator_);
    const auto position = divideToUInt64(numerator, denominator);
    if (position.quotientOverflow)
        return std::nullopt;
    // If time is n/d and rate is p/q, the fractional-frame remainder is (n*p) mod(d*q).
    // Seconds until the next boundary are (d*q - remainder)/(d*p). This avoids rounding the
    // absolute position or adding one to a potentially maximal frame index. The remaining
    // numerator has at most 95 bits; scaling by 1e9 fits 125 bits on every target.
    constexpr auto nanosecondsPerSecond = std::uint64_t{1'000'000'000};
    const auto remaining =
        multiplyByWord(subtract(denominator, position.remainder), nanosecondsPerSecond);
    if (!remaining)
        return std::nullopt;
    const auto divisor =
        multiplyToUInt128(static_cast<std::uint64_t>(time.denominator()), rateNumerator_);
    const auto result = divideToUInt64(*remaining, divisor);
    const bool roundUp = compare(result.remainder, UInt128{}) != std::strong_ordering::equal;
    if (result.quotientOverflow ||
        (roundUp && result.quotient == std::numeric_limits<std::uint64_t>::max()))
        return std::nullopt;
    return result.quotient + (roundUp ? 1U : 0U);
}

std::optional<std::uint64_t>
FrameTimeMapping::nanosecondsUntilNextFrame(const RationalTime time,
                                            const RationalTime playbackRate) const noexcept {
    if (time < RationalTime{} || playbackRate <= RationalTime{})
        return std::nullopt;
    const auto positionNumerator =
        multiplyToUInt128(static_cast<std::uint64_t>(time.numerator()), rateNumerator_);
    const auto positionDenominator =
        multiplyToUInt128(static_cast<std::uint64_t>(time.denominator()), rateDenominator_);
    const auto position = divideToUInt64(positionNumerator, positionDenominator);
    if (position.quotientOverflow)
        return std::nullopt;
    // Scale the exact media fraction by the existing speed ratio before the single ceiling.
    // Rounding media nanoseconds first would add avoidable error at fractional rates/speeds.
    const auto remaining = multiplyByWord(subtract(positionDenominator, position.remainder),
                                          std::uint64_t{1'000'000'000});
    if (!remaining)
        return std::nullopt;
    const auto numerator =
        multiplyByWord(*remaining, static_cast<std::uint64_t>(playbackRate.denominator()));
    const auto divisor = multiplyByWord(
        multiplyToUInt128(static_cast<std::uint64_t>(time.denominator()), rateNumerator_),
        static_cast<std::uint64_t>(playbackRate.numerator()));
    if (!numerator || !divisor || divisor->limbs[1] >= (std::uint64_t{1} << 63U))
        return std::nullopt;
    const auto result = divideToUInt64(*numerator, *divisor);
    const bool roundUp = compare(result.remainder, UInt128{}) != std::strong_ordering::equal;
    if (result.quotientOverflow ||
        (roundUp && result.quotient == std::numeric_limits<std::uint64_t>::max()))
        return std::nullopt;
    return result.quotient + (roundUp ? 1U : 0U);
}

} // namespace bloom::core
