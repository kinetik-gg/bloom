#include "output_analysis_numeric.hpp"

#include <bit>
#include <cstdint>
#include <limits>

namespace bloom::output::detail {

std::optional<OutputAnalysisRoundedBinary32V1> roundOutputAnalysisPositiveRationalToBinary32V1(
    const OutputAnalysisPositiveRational32V1 value) noexcept {
    if (value.numerator == 0 || value.denominator == 0) {
        return std::nullopt;
    }

    int exponent = 0;
    if (value.numerator >= value.denominator) {
        const auto integral = value.numerator / value.denominator;
        exponent = std::bit_width(integral) - 1;
    } else {
        std::uint64_t scaledNumerator = value.numerator;
        while (scaledNumerator < value.denominator) {
            scaledNumerator <<= 1U;
            --exponent;
        }
    }

    const int scale = 23 - exponent;
    std::uint64_t quotient = 0;
    std::uint64_t remainder = 0;
    std::uint64_t divisor = value.denominator;
    if (scale >= 0) {
        quotient = value.numerator / divisor;
        remainder = value.numerator % divisor;
        for (int bit = 0; bit < scale; ++bit) {
            quotient <<= 1U;
            remainder <<= 1U;
            if (remainder >= divisor) {
                ++quotient;
                remainder -= divisor;
            }
        }
    } else {
        const auto rightShift = static_cast<unsigned int>(-scale);
        if (rightShift >= 32U ||
            divisor > (std::numeric_limits<std::uint64_t>::max() >> rightShift)) {
            return std::nullopt;
        }
        divisor <<= rightShift;
        quotient = value.numerator / divisor;
        remainder = value.numerator % divisor;
    }

    const bool exact = remainder == 0;
    const auto twiceRemainder = remainder * 2U;
    if (twiceRemainder > divisor || (twiceRemainder == divisor && (quotient & 1U) != 0)) {
        ++quotient;
    }
    if (quotient == (std::uint64_t{1} << 24U)) {
        quotient >>= 1U;
        ++exponent;
    }
    if (exponent < -126 || exponent > 127 || quotient < (std::uint64_t{1} << 23U) ||
        quotient >= (std::uint64_t{1} << 24U)) {
        return std::nullopt;
    }
    const auto biasedExponent = static_cast<std::uint32_t>(exponent + 127);
    const auto fraction = static_cast<std::uint32_t>(quotient - (std::uint64_t{1} << 23U));
    return OutputAnalysisRoundedBinary32V1{(biasedExponent << 23U) | fraction, exact};
}

} // namespace bloom::output::detail
