#include <bloom/ui/timeline_frame_math.hpp>

#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

using namespace bloom;

[[noreturn]] void fail(const std::string_view message) {
    std::cerr << "timeline frame math test failed: " << message << '\n';
    std::exit(1);
}

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

[[nodiscard]] core::RationalTime time(const std::int64_t numerator,
                                      const std::int64_t denominator = 1) {
    const auto value = core::RationalTime::create(numerator, denominator);
    require(value.has_value(), "test rational must be valid");
    return value.value_or(core::RationalTime{});
}

[[nodiscard]] document::FrameRate rate(const std::uint32_t numerator,
                                       const std::uint32_t denominator = 1) {
    const auto value = document::FrameRate::create(numerator, denominator);
    require(value.has_value(), "test frame rate must be valid");
    return value.value_or(document::FrameRate::framesPerSecond24());
}

void testRoundTripAndBoundary() {
    const auto fps24 = rate(24, 1);
    const auto oneSecond = time(1);

    const auto max = ui::maxFrameIndex(fps24, oneSecond);
    require(max.has_value() && *max == 23, "one second at 24 fps ends at frame 23");

    const auto last = ui::frameTimeForIndex(fps24, oneSecond, 23);
    require(last.has_value() && *last == time(23, 24),
            "the maximum index maps to its exact rational time");

    const auto pastEnd = ui::frameTimeForIndex(fps24, oneSecond, 24);
    require(!pastEnd.has_value(),
            "the duration endpoint is excluded, matching the strictly-less rule");

    for (std::uint64_t index = 0; index <= 23; ++index) {
        const auto mapped = ui::frameTimeForIndex(fps24, oneSecond, index);
        if (!mapped.has_value()) {
            require(false, "every in-range index maps to an exact time");
            continue;
        }
        const auto nearest = ui::nearestFrameIndexForTime(fps24, oneSecond, *mapped);
        require(nearest.has_value() && *nearest == index,
                "an exact frame time round-trips to the same index");
    }
}

void testTieToGreaterAtExactHalfway() {
    // 1 second at 24 fps: frame 0 is at 0/24, frame 1 is at 1/24. The exact halfway point between
    // them is 1/48, chosen so it is exactly representable as a RationalTime (matching the contract
    // doc's own worked example in "Session Time And Scrubbing").
    const auto fps24 = rate(24, 1);
    const auto oneSecond = time(1);

    require(ui::nearestFrameIndexForTime(fps24, oneSecond, time(1, 49)) ==
                std::optional<std::uint64_t>(0),
            "a time just below halfway selects the lower frame");
    require(ui::nearestFrameIndexForTime(fps24, oneSecond, time(1, 48)) ==
                std::optional<std::uint64_t>(1),
            "an exact halfway time selects the greater frame");
    require(ui::nearestFrameIndexForTime(fps24, oneSecond, time(1, 47)) ==
                std::optional<std::uint64_t>(1),
            "a time just above halfway selects the greater frame");
}

void testClampBelowZeroAndAboveRange() {
    const auto fps24 = rate(24, 1);
    const auto oneSecond = time(1);

    require(ui::nearestFrameIndexForTime(fps24, oneSecond, time(-5)) ==
                std::optional<std::uint64_t>(0),
            "a negative time clamps to the first frame");
    require(ui::nearestFrameIndexForTime(fps24, oneSecond, time(0)) ==
                std::optional<std::uint64_t>(0),
            "zero clamps to the first frame");
    require(ui::nearestFrameIndexForTime(fps24, oneSecond, time(1000)) ==
                std::optional<std::uint64_t>(23),
            "a time at or beyond duration clamps to the final valid frame");
}

void testCheckedOverflowRefusal() {
    constexpr auto maximumInt = std::numeric_limits<std::int64_t>::max();
    constexpr auto maximumRate = std::numeric_limits<std::uint32_t>::max();
    const auto absurdRate = rate(maximumRate, 1);

    require(!ui::maxFrameIndex(absurdRate, time(maximumInt)).has_value(),
            "a complete frame range that cannot be represented in 64 bits is refused");
    require(ui::frameTimeForIndex(absurdRate, time(1), 0).has_value(),
            "a representable in-range index still succeeds (sanity check for the refusal test)");

    require(!ui::maxFrameIndex(rate(1, 1), time(0)).has_value(),
            "a non-positive duration is refused, never UB");
    require(!ui::maxFrameIndex(rate(1, 1), time(-1)).has_value(),
            "a negative duration is refused, never UB");
    require(!ui::nearestFrameIndexForTime(rate(1, 1), time(0), time(0)).has_value(),
            "an invalid mapping refuses the nearest-index query too");
}

} // namespace

int main() {
    testRoundTripAndBoundary();
    testTieToGreaterAtExactHalfway();
    testClampBelowZeroAndAboveRange();
    testCheckedOverflowRefusal();
    return 0;
}
