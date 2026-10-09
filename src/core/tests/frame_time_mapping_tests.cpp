#include <bloom/core/frame_time_mapping.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

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

// issue #105 (playback transport): frameOffsetForElapsedNanoseconds() floors rather than rounds
// to nearest -- unlike nearestFrameIndex()'s scrub tie-to-greater semantics, ordinary real-time
// playback at elapsed E shows the frame that began at-or-before E.
void testFrameOffsetForElapsedNanosecondsFloorsExactly() {
    // 1000 fps: 1 frame == 1,000,000 ns exactly, so boundaries land on integer nanoseconds and the
    // floor/no-floor distinction is directly testable without an inexact rate.
    const auto oneKilohertz = mapping(time(1), 1000, 1);
    require(oneKilohertz.frameOffsetForElapsedNanoseconds(0) == 0,
            "zero elapsed time is frame offset zero");
    require(oneKilohertz.frameOffsetForElapsedNanoseconds(999'999) == 0,
            "one nanosecond below a frame boundary still floors to the earlier frame");
    require(oneKilohertz.frameOffsetForElapsedNanoseconds(1'000'000) == 1,
            "exactly one frame duration floors to the next frame, matching timeForFrame(1)");
    require(oneKilohertz.frameOffsetForElapsedNanoseconds(1'000'001) == 1,
            "one nanosecond past a frame boundary stays on that frame");
    require(oneKilohertz.frameOffsetForElapsedNanoseconds(2'500'000) == 2,
            "a non-multiple elapsed duration floors, never rounds to nearest");

    // A fractional rate (24000/1001, i.e. ~23.976 fps) exercises the same checked multiword
    // arithmetic timeForFrame()/nearestFrameIndex() use, not just an integer-friendly rate. Frame
    // ten's exact time is 10 * 1001 / 24000 seconds == 417,083,333.333... ns (not an integer
    // nanosecond count), so the whole-nanosecond boundary straddles it: one nanosecond below still
    // floors to frame nine, one nanosecond above already floors to frame ten.
    const auto fractional = mapping(time(10), 24000, 1001);
    const auto frameTenTime = fractional.timeForFrame(10);
    require(frameTenTime.hasValue(), "frame ten is within the ten-second duration");
    require(fractional.frameOffsetForElapsedNanoseconds(417'083'333) == 9,
            "elapsed time strictly before a fractional-rate frame boundary floors to the prior "
            "frame");
    require(fractional.frameOffsetForElapsedNanoseconds(417'083'334) == 10,
            "elapsed time at/past a fractional-rate frame boundary floors to that frame");

    // Overflow is checked, not UB: an elapsed duration paired with the maximal representable rate
    // numerator overflows the 64-bit quotient and must report std::nullopt rather than wrap.
    constexpr auto maximumRate = std::numeric_limits<std::uint32_t>::max();
    const auto huge = mapping(time(1), maximumRate, 1);
    require(!huge.frameOffsetForElapsedNanoseconds(std::numeric_limits<std::uint64_t>::max())
                 .has_value(),
            "an unrepresentable frame offset is refused rather than silently wrapped");
}

void testRationalPlaybackGridAndDueBoundaries() {
    constexpr std::array rates{std::pair{24U, 1U},       std::pair{25U, 1U},
                               std::pair{30U, 1U},       std::pair{60U, 1U},
                               std::pair{24000U, 1001U}, std::pair{30000U, 1001U},
                               std::pair{60000U, 1001U}};
    constexpr auto ns = std::uint64_t{1'000'000'000};
    for (const auto& [p, q] : rates) {
        const auto grid = mapping(time(1), p, q);
        const auto periodNumerator = ns * q;
        const auto periodCeiling = (periodNumerator + p - 1) / p;
        require(grid.frameOffsetForTime(time(0)) == 0 &&
                    grid.nanosecondsUntilNextFrame(time(0)) == periodCeiling,
                "zero starts a positive exact rational frame period");
        for (const auto index : {1ULL, 10ULL, 600ULL}) {
            const auto exact = time(static_cast<std::int64_t>(index * q), p);
            require(grid.frameOffsetForTime(exact) == index &&
                        grid.nanosecondsUntilNextFrame(exact) == periodCeiling,
                    "exact audio frame boundaries floor without clamping after range/loops");
            const auto boundaryNumerator = index * periodNumerator;
            const auto boundaryCeiling = (boundaryNumerator + p - 1) / p;
            const auto before = boundaryCeiling - 1;
            require(grid.frameOffsetForElapsedNanoseconds(before) == index - 1 &&
                        grid.frameOffsetForTime(time(static_cast<std::int64_t>(before), ns)) ==
                            index - 1,
                    "the final whole nanosecond before a fractional boundary stays on prior frame");
            require(grid.nanosecondsUntilNextFrame(time(static_cast<std::int64_t>(before), ns)) ==
                        (boundaryNumerator - before * p + p - 1) / p,
                    "remaining deadline is exact ceil of rational boundary distance");
            require(grid.frameOffsetForTime(time(static_cast<std::int64_t>(boundaryCeiling), ns)) ==
                        index,
                    "first representable nanosecond at/after boundary advances frame");
        }
        require(!grid.frameOffsetForTime(time(-1)).has_value() &&
                    !grid.nanosecondsUntilNextFrame(time(-1)).has_value(),
                "negative clock input is refused without unsigned wrap");
    }
    constexpr auto maxInt = std::numeric_limits<std::int64_t>::max();
    constexpr auto maxRate = std::numeric_limits<std::uint32_t>::max();
    const auto wide = mapping(time(1), maxRate, maxRate - 1);
    require(wide.frameOffsetForTime(time(1, maxInt)) == 0 &&
                wide.nanosecondsUntilNextFrame(time(1, maxInt)) == ns,
            "95-bit remaining fraction scaled into 125 bits keeps exact positive deadline");
    const auto fast = mapping(time(1), maxRate, 1);
    require(!fast.frameOffsetForTime(time(maxInt)).has_value() &&
                !fast.nanosecondsUntilNextFrame(time(maxInt)).has_value(),
            "unrepresentable audio index refuses both target and boundary without wrap");
    const auto normal = mapping(time(1), 25, 1);
    require(normal.nanosecondsUntilNextFrame(time(0), time(2)) == 20'000'000 &&
                normal.nanosecondsUntilNextFrame(time(0), time(1, 2)) == 80'000'000 &&
                normal.nanosecondsUntilNextFrame(time(0), time(1, 1'000'000)) ==
                    40'000'000'000'000ULL,
            "existing fast and slow audio rate ratios convert media deadline to wall time");
    require(mapping(time(1), 30, 1).nanosecondsUntilNextFrame(time(0), time(1, 2)) == 66'666'667 &&
                mapping(time(1), 60, 1).nanosecondsUntilNextFrame(time(0), time(1, 2)) ==
                    33'333'334,
            "fractional wall periods round once after exact speed scaling");
    require(!normal.nanosecondsUntilNextFrame(time(0), time(0)) &&
                !normal.nanosecondsUntilNextFrame(time(0), time(-1)),
            "invalid speed cannot produce a silent forced-one deadline");
    const auto sparse = mapping(time(1), 1, maxRate);
    require(sparse.nanosecondsUntilNextFrame(time(0)) == ns * maxRate,
            "long rational periods remain checked and finite");
    require(!sparse.nanosecondsUntilNextFrame(time(0), time(1, maxInt)),
            "unrepresentable scaled wall budget is explicitly refused");
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
    testFrameOffsetForElapsedNanosecondsFloorsExactly();
    testRationalPlaybackGridAndDueBoundaries();
    return 0;
}
