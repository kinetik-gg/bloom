#include <algorithm>
#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/runtime/composition_time.hpp>
#include <cmath>
#include <limits>
#include <numeric>

namespace bloom::runtime {
namespace {
using core::RationalTime;
constexpr auto limit = std::numeric_limits<std::int64_t>::max();

std::optional<std::int64_t> multiply(const std::int64_t a, const std::int64_t b) {
    // Refuse the asymmetric INT64_MIN endpoint so absolute magnitudes remain representable.
    if (a == -limit - 1 || b == -limit - 1 || (a != 0 && std::abs(b) > limit / std::abs(a)))
        return std::nullopt;
    return a * b;
}
std::optional<RationalTime> scalar(const double value) {
    if (!std::isfinite(value))
        return std::nullopt;
    if (value == 0)
        return RationalTime{};
    int exponent = 0;
    const auto fraction = std::frexp(value, &exponent);
    auto numerator = static_cast<std::int64_t>(std::ldexp(fraction, 53));
    exponent -= 53;
    while (exponent < 0 && numerator % 2 == 0) {
        numerator /= 2;
        ++exponent;
    }
    if (exponent < -62 || exponent > 62)
        return std::nullopt;
    if (exponent >= 0) {
        const auto result = multiply(numerator, std::int64_t{1} << exponent);
        return result ? std::optional{RationalTime::fromInteger(*result)} : std::nullopt;
    }
    return RationalTime::create(numerator, std::int64_t{1} << -exponent);
}
std::optional<RationalTime> product(const RationalTime a, const RationalTime b) {
    if (a.numerator() == -limit - 1 || b.numerator() == -limit - 1)
        return std::nullopt;
    const auto first = std::gcd(a.numerator(), b.denominator());
    const auto second = std::gcd(b.numerator(), a.denominator());
    const auto numerator = multiply(a.numerator() / first, b.numerator() / second);
    const auto denominator = multiply(a.denominator() / second, b.denominator() / first);
    return numerator && denominator ? RationalTime::create(*numerator, *denominator) : std::nullopt;
}
std::optional<RationalTime> difference(const RationalTime a, const RationalTime b) {
    const auto divisor = std::gcd(a.denominator(), b.denominator());
    const auto left = multiply(a.numerator(), b.denominator() / divisor);
    const auto right = multiply(b.numerator(), a.denominator() / divisor);
    const auto denominator = multiply(a.denominator(), b.denominator() / divisor);
    if (!left || !right || !denominator || (*right > 0 && *left < -limit + *right) ||
        (*right < 0 && *left > limit + *right))
        return std::nullopt;
    return RationalTime::create(*left - *right, *denominator);
}
std::optional<RationalTime> remainder(const RationalTime time, const RationalTime period) {
    const auto divisor = std::gcd(time.denominator(), period.denominator());
    const auto numerator = multiply(time.numerator(), period.denominator() / divisor);
    const auto modulus = multiply(period.numerator(), time.denominator() / divisor);
    const auto denominator = multiply(time.denominator(), period.denominator() / divisor);
    if (!numerator || !modulus || !denominator || *modulus <= 0)
        return std::nullopt;
    return RationalTime::create(*numerator % *modulus, *denominator);
}
std::optional<RationalTime> roundedMapping(const RationalTime time, const double offset,
                                           const double scale) {
    // Two ordinary decimal Float64 parameters can have binary denominators whose product
    // exceeds int64. Keep exact arithmetic where possible; otherwise quantize source time
    // to a nanosecond, independently of frame rate, instead of rejecting such parameters.
    constexpr std::int64_t ticksPerSecond = 1'000'000'000;
    // Use binary64 on every platform; long double has different precision on Windows and Linux.
    const double shifted = time.toSeconds() - offset;
    const double seconds = shifted * scale;
    const auto ticks = std::round(seconds * ticksPerSecond);
    if (!std::isfinite(ticks) || ticks <= -static_cast<double>(limit) ||
        ticks >= static_cast<double>(limit))
        return std::nullopt;
    return RationalTime::create(static_cast<std::int64_t>(ticks), ticksPerSecond);
}
} // namespace

std::optional<core::RationalTime> mapCompositionTime(const core::RationalTime time,
                                                     const double offset, const double scale,
                                                     const core::RationalTime duration,
                                                     const document::FrameRate rate,
                                                     const std::int64_t loopMode) {
    const auto mapping =
        core::FrameTimeMapping::create(duration, rate.numerator(), rate.denominator());
    const auto offsetTime = scalar(offset);
    const auto scaleTime = scalar(scale);
    if (!mapping || !std::isfinite(offset) || !std::isfinite(scale) || loopMode < 0 || loopMode > 2)
        return std::nullopt;
    const auto shifted = offsetTime ? difference(time, *offsetTime) : std::nullopt;
    auto mapped = shifted && scaleTime ? product(*shifted, *scaleTime) : std::nullopt;
    if (!mapped)
        mapped = roundedMapping(time, offset, scale);
    if (!mapped)
        return std::nullopt;
    if (*mapped < RationalTime{})
        return RationalTime{};
    const auto last = mapping.value()->timeForFrame(mapping.value()->maximumFrameIndex());
    if (!last)
        return std::nullopt;
    if (*last.value() == RationalTime{})
        return RationalTime{};
    if (loopMode == 1)
        return remainder(*mapped, duration);
    if (loopMode == 0)
        return std::min(*mapped, *last.value());
    const auto period = product(*last.value(), RationalTime::fromInteger(2));
    const auto wrapped = period ? remainder(*mapped, *period) : std::nullopt;
    if (!wrapped)
        return std::nullopt;
    return *wrapped <= *last.value() ? wrapped : difference(*period, *wrapped);
}
} // namespace bloom::runtime
