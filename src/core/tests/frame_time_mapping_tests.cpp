#include <bloom/core/frame_time_mapping.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

using bloom::core::FrameTimeError;
using bloom::core::FrameTimeMapping;
using bloom::core::FrameTimeMappingCreateResult;
using bloom::core::FrameTimeMappingError;
using bloom::core::FrameTimeResult;
using bloom::core::RationalTime;

template <typename Result>
concept ExposesValueFromRvalue = requires(Result result) { static_cast<Result&&>(result).value(); };

template <typename Result>
concept ExposesValueFromConstRvalue =
    requires(Result result) { static_cast<const Result&&>(result).value(); };

static_assert(!ExposesValueFromRvalue<FrameTimeMappingCreateResult>);
static_assert(!ExposesValueFromRvalue<FrameTimeResult>);
static_assert(!ExposesValueFromConstRvalue<FrameTimeMappingCreateResult>);
static_assert(!ExposesValueFromConstRvalue<FrameTimeResult>);

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
    return value.value_or(RationalTime{});
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

void testExtremeRateRoundingBoundary() {
    constexpr auto maximumInt = std::numeric_limits<std::int64_t>::max();
    constexpr auto maximumRate = std::numeric_limits<std::uint32_t>::max();
    constexpr auto rateDenominator = maximumRate - 1;
    constexpr auto lowerFrame = std::uint64_t{1'073'741'823};
    constexpr auto commonDenominator = static_cast<std::int64_t>(std::uint64_t{2} * maximumRate);
    constexpr auto halfwayNumeratorMagnitude =
        ((std::uint64_t{2} * lowerFrame) + 1) * rateDenominator;
    static_assert(halfwayNumeratorMagnitude <= static_cast<std::uint64_t>(maximumInt));
    constexpr auto halfwayNumerator = static_cast<std::int64_t>(halfwayNumeratorMagnitude);
    static_assert(static_cast<std::uint64_t>(halfwayNumerator / 2) >
                  std::numeric_limits<std::uint64_t>::max() / maximumRate);

    const auto value = mapping(time(maximumInt, maximumRate), maximumRate, rateDenominator);
    require(value.rateNumerator() == maximumRate && value.rateDenominator() == rateDenominator,
            "an extreme coprime rate remains normalized");
    require(value.nearestFrameIndex(time(halfwayNumerator - 1, commonDenominator)) == lowerFrame,
            "multi-limb rounding selects the lower frame immediately below halfway");
    require(value.nearestFrameIndex(time(halfwayNumerator, commonDenominator)) == lowerFrame + 1,
            "multi-limb rounding selects the greater frame exactly halfway");
    require(value.nearestFrameIndex(time(halfwayNumerator + 1, commonDenominator)) ==
                lowerFrame + 1,
            "multi-limb rounding selects the greater frame immediately above halfway");
}

void testMaximumIndexDomainBoundary() {
    constexpr auto maximumIndex = std::numeric_limits<std::uint64_t>::max();
    constexpr auto rateNumerator = std::uint32_t{1} << 31U;
    constexpr auto exactDuration = std::int64_t{1} << 33U;

    const auto exact = mapping(time(exactDuration), rateNumerator, 1);
    require(exact.maximumFrameIndex() == maximumIndex,
            "the complete uint64 frame-index domain is accepted");

    const auto adjacent = FrameTimeMapping::create(time(exactDuration + 1), rateNumerator, 1);
    require(!adjacent.hasValue() && adjacent.error() == FrameTimeMappingError::FrameIndexOverflow,
            "the mapping immediately beyond the uint64 frame-index domain is rejected");
}

void testExactTimeRepresentationBoundary() {
    constexpr auto maximumInt = std::numeric_limits<std::int64_t>::max();
    constexpr auto boundaryFrame = static_cast<std::uint64_t>(maximumInt);
    const auto value = mapping(time(maximumInt, 2), 3, 1);

    const auto atBoundary = value.timeForFrame(boundaryFrame);
    require(atBoundary.hasValue() && *atBoundary.value() == time(maximumInt, 3),
            "an exact frame time at the signed numerator limit is representable");
    require(value.timeForFrame(boundaryFrame + 1).error() ==
                FrameTimeError::TimeRepresentationOverflow,
            "the adjacent frame reports exact-time representation overflow");
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
    testExtremeRateRoundingBoundary();
    testMaximumIndexDomainBoundary();
    testExactTimeRepresentationBoundary();
    testStructuredFailures();
    return 0;
}
