#include "timeline_key_time.hpp"

#include <bloom/runtime/layer_time_projection.hpp>

#include <cstdint>

namespace bloom::ui {
namespace {
[[nodiscard]] bool isIdentity(const KeyClock& clock) {
    return !clock.has_value() ||
           (clock->startTime == core::RationalTime{} && clock->speed.isIdentity());
}

[[nodiscard]] std::optional<std::int64_t> checkedMultiply(const std::int64_t a,
                                                          const std::int64_t b) {
    std::int64_t result = 0;
    if (__builtin_mul_overflow(a, b, &result))
        return std::nullopt;
    return result;
}
} // namespace

std::optional<core::RationalTime> compositionTimeOfKey(const KeyClock& clock,
                                                       const core::RationalTime keyTime) {
    if (isIdentity(clock))
        return keyTime;
    return runtime::placementTimeOfLayerTime(*clock, keyTime);
}

std::optional<core::RationalTime> keyTimeOfCompositionTime(const KeyClock& clock,
                                                           const core::RationalTime time) {
    if (isIdentity(clock))
        return time;
    // tau = (t - start) * n / d, with every product checked.
    const auto& start = clock->startTime;
    const auto left = checkedMultiply(time.numerator(), start.denominator());
    const auto right = checkedMultiply(start.numerator(), time.denominator());
    const auto denominator = checkedMultiply(time.denominator(), start.denominator());
    std::int64_t difference = 0;
    if (!left || !right || !denominator || __builtin_sub_overflow(*left, *right, &difference))
        return std::nullopt;
    const auto numerator = checkedMultiply(difference, clock->speed.numerator());
    const auto scaledDenominator = checkedMultiply(*denominator, clock->speed.denominator());
    if (!numerator || !scaledDenominator)
        return std::nullopt;
    return core::RationalTime::create(*numerator, *scaledDenominator);
}

core::RationalTime projectedKeyTime(const KeyClock& clock, const core::RationalTime keyTime) {
    return compositionTimeOfKey(clock, keyTime).value_or(keyTime);
}

} // namespace bloom::ui
