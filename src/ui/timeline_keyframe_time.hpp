#pragma once
#include <bloom/core/rational_time.hpp>
#include <cmath>
#include <limits>
#include <numeric>

namespace bloom::ui {
// Portable checked rational addition for editor gestures and clipboard offsets. Refuse an
// unrepresentable result rather than overflowing or changing the authored time silently.
inline std::optional<core::RationalTime> offsetKeyTime(core::RationalTime base,
                                                       core::RationalTime offset) {
    const auto divisor = std::gcd(base.denominator(), offset.denominator());
    const auto leftFactor = offset.denominator() / divisor;
    const auto rightFactor = base.denominator() / divisor;
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto fits = [](std::int64_t value, std::int64_t factor) {
        return value <= maximum / factor && value >= minimum / factor;
    };
    if (!fits(base.numerator(), leftFactor) || !fits(offset.numerator(), rightFactor) ||
        !fits(base.denominator(), leftFactor))
        return std::nullopt;
    const auto a = base.numerator() * leftFactor, b = offset.numerator() * rightFactor;
    if ((b > 0 && a > maximum - b) || (b < 0 && a < minimum - b))
        return std::nullopt;
    return core::RationalTime::create(a + b, base.denominator() * leftFactor);
}
inline std::optional<core::RationalTime> keyTimeDifference(core::RationalTime target,
                                                           core::RationalTime origin) {
    if (origin.numerator() == std::numeric_limits<std::int64_t>::min())
        return std::nullopt;
    const auto negative = core::RationalTime::create(-origin.numerator(), origin.denominator());
    return negative ? offsetKeyTime(target, *negative) : std::nullopt;
}
inline std::optional<core::RationalTime> keyPointerOffset(double seconds) {
    constexpr std::int64_t precision = 1'000'000'000;
    const double scaled = std::round(seconds * static_cast<double>(precision));
    if (!std::isfinite(scaled) ||
        scaled <= static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        scaled >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    return core::RationalTime::create(static_cast<std::int64_t>(scaled), precision);
}
} // namespace bloom::ui
