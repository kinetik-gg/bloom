#include <bloom/core/frame_time_mapping.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

using bloom::core::FrameTimeError;
using bloom::core::FrameTimeMapping;
using bloom::core::FrameTimeMappingError;
using bloom::core::RationalTime;

[[noreturn]] void fail(const std::string_view message) {
    std::cerr << "frame time mapping test failed: " << message << '\n';
    std::exit(1);
}

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

[[nodiscard]] RationalTime time(const std::int64_t numerator, const std::int64_t denominator = 1) {
    const auto value = RationalTime::create(numerator, denominator);
    require(value.has_value(), "test rational must be valid");
    return *value;
}

[[nodiscard]] FrameTimeMapping mapping(const RationalTime duration,
                                       const std::uint32_t rateNumerator,
                                       const std::uint32_t rateDenominator) {
    const auto result = FrameTimeMapping::create(duration, rateNumerator, rateDenominator);
    require(result.hasValue(), "test mapping must be valid");
    return *result.value();
}

void testStrictFrameRangeAndExactTimes() {
    const auto oneSecond = mapping(time(1), 24, 1);
    require(oneSecond.maximumFrameIndex() == 23, "one second at 24 fps ends at frame 23");
    const auto last = oneSecond.timeForFrame(23);
    require(last.hasValue() && *last.value() == time(23, 24), "frame time is an exact rational");
    require(oneSecond.timeForFrame(24).error() == FrameTimeError::FrameOutsideRange,
            "duration endpoint is excluded");

    const auto oneFrame = mapping(time(1001, 24000), 24000, 1001);
    require(oneFrame.maximumFrameIndex() == 0,
            "a duration equal to one frame contains only frame zero");

    const auto overOneFrame = mapping(time(1002, 24000), 24000, 1001);
    require(overOneFrame.maximumFrameIndex() == 1,
            "a duration beyond one frame includes frame one");
    const auto frameOne = overOneFrame.timeForFrame(1);
    require(frameOne.hasValue() && *frameOne.value() == time(1001, 24000),
            "fractional frame rate maps without binary rounding");
}

void testNearestTiesAndClamping() {
    const auto value = mapping(time(1), 24, 1);
    require(value.nearestFrameIndex(time(-1)) == 0 && value.nearestFrameIndex(time(0)) == 0,
            "non-positive times clamp to the first frame");
    require(value.nearestFrameIndex(time(1, 49)) == 0,
            "a time below halfway selects the lower frame");
    require(value.nearestFrameIndex(time(1, 48)) == 1,
            "an exact halfway time selects the greater frame");
    require(value.nearestFrameIndex(time(1, 47)) == 1,
            "a time above halfway selects the greater frame");
    require(value.nearestFrameIndex(time(999)) == 23,
            "times beyond the duration clamp to the final valid frame");

    const auto shortDuration = mapping(time(11, 100), 24, 1);
    require(shortDuration.maximumFrameIndex() == 2 &&
                shortDuration.nearestFrameIndex(time(109, 1000)) == 2,
            "rounding never escapes a short composition's valid frame range");
}

void testRateNormalizationAndExtremeArithmetic() {
    const auto normalized = mapping(time(1), 48, 2);
    require(normalized.rateNumerator() == 24 && normalized.rateDenominator() == 1,
            "mapping normalizes an input rate");

    constexpr auto maximumInt = std::numeric_limits<std::int64_t>::max();
    constexpr auto maximumRate = std::numeric_limits<std::uint32_t>::max();
    const auto large = mapping(time(maximumInt, maximumRate), maximumRate, 1);
    require(large.maximumFrameIndex() == static_cast<std::uint64_t>(maximumInt) - 1,
            "multiword range arithmetic preserves a large cancelling ratio");
    require(large.nearestFrameIndex(time(maximumInt - 1, maximumRate)) ==
                static_cast<std::uint64_t>(maximumInt) - 1,
            "multiword nearest-index arithmetic preserves an extreme exact frame");

    const auto overflowing = FrameTimeMapping::create(time(maximumInt), maximumRate, 1);
    require(!overflowing.hasValue() &&
                overflowing.error() == FrameTimeMappingError::FrameIndexOverflow,
            "an unrepresentable complete frame range is rejected");
}

void testStructuredFailures() {
    require(FrameTimeMapping::create(time(1), 0, 1).error() == FrameTimeMappingError::InvalidRate &&
                FrameTimeMapping::create(time(1), 1, 0).error() ==
                    FrameTimeMappingError::InvalidRate,
            "zero rate components are rejected");
    require(FrameTimeMapping::create(time(0), 24, 1).error() ==
                    FrameTimeMappingError::NonPositiveDuration &&
                FrameTimeMapping::create(time(-1), 24, 1).error() ==
                    FrameTimeMappingError::NonPositiveDuration,
            "non-positive durations are rejected");

    constexpr auto maximumInt = std::numeric_limits<std::int64_t>::max();
    constexpr auto maximumRate = std::numeric_limits<std::uint32_t>::max();
    const auto sparse = mapping(time(maximumInt), 4, maximumRate);
    const auto last = sparse.timeForFrame(sparse.maximumFrameIndex());
    require(!last.hasValue() && last.error() == FrameTimeError::TimeRepresentationOverflow,
            "a valid index reports when its exact time exceeds RationalTime representation");
}

} // namespace

int main() {
    testStrictFrameRangeAndExactTimes();
    testNearestTiesAndClamping();
    testRateNormalizationAndExtremeArithmetic();
    testStructuredFailures();
    return 0;
}
