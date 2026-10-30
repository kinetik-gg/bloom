#include <bloom/commands/layer_timing_gesture.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace bloom::commands {
namespace {

using core::RationalTime;

[[nodiscard]] std::optional<std::int64_t> product(const std::int64_t a, const std::int64_t b) {
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    if (a == std::numeric_limits<std::int64_t>::min() ||
        b == std::numeric_limits<std::int64_t>::min())
        return std::nullopt;
    const auto magnitude = a < 0 ? -a : a;
    if (magnitude != 0 && (b > limit / magnitude || b < -(limit / magnitude)))
        return std::nullopt;
    return a * b;
}

// a + b * n / d, exactly.
[[nodiscard]] std::optional<RationalTime> addScaled(const RationalTime a, const RationalTime b,
                                                    const std::int64_t n, const std::int64_t d) {
    const auto bn = product(b.numerator(), n);
    const auto bd = product(b.denominator(), d);
    if (!bn || !bd || *bd == 0)
        return std::nullopt;
    const auto left = product(a.numerator(), *bd);
    const auto right = product(*bn, a.denominator());
    const auto denominator = product(a.denominator(), *bd);
    if (!left || !right || !denominator ||
        (*right > 0 && *left > std::numeric_limits<std::int64_t>::max() - *right) ||
        (*right < 0 && *left < std::numeric_limits<std::int64_t>::min() - *right))
        return std::nullopt;
    return RationalTime::create(*left + *right, *denominator);
}

[[nodiscard]] std::optional<RationalTime> frameTime(const std::int64_t frame,
                                                    const document::FrameRate rate) {
    const auto numerator = product(frame, static_cast<std::int64_t>(rate.denominator()));
    return numerator ? RationalTime::create(*numerator, rate.numerator()) : std::nullopt;
}

[[nodiscard]] std::int64_t frameOf(const RationalTime time, const document::FrameRate rate,
                                   const bool up) {
    const double frames = time.toSeconds() * rate.numerator() / rate.denominator();
    const double nearest = std::round(frames);
    // Exact grid times stay put; an off-grid bound rounds toward the inside of its interval.
    if (std::abs(frames - nearest) < 1e-9)
        return static_cast<std::int64_t>(nearest);
    return static_cast<std::int64_t>(up ? std::ceil(frames) : std::floor(frames));
}

} // namespace

std::optional<LayerTimingProposal> proposeLayerTiming(const LayerTimingGestureInput& input) {
    const auto rate = input.rate;
    const auto deltaFrames = static_cast<std::int64_t>(
        std::llround(input.deltaSeconds * rate.numerator() / rate.denominator()));
    const auto in = frameOf(input.in, rate, false);
    const auto out = frameOf(input.out, rate, false);
    const auto last = frameOf(input.duration, rate, true); // first frame at or after the end
    if (out <= in)
        return std::nullopt;
    // Trim limits: the composition, widened to the content extent where it reaches further.
    auto low = std::int64_t{0};
    auto high = last;
    if (input.extentStart)
        low = std::min(low, frameOf(*input.extentStart, rate, true));
    if (input.extentEnd)
        high = std::max(high, frameOf(*input.extentEnd, rate, false));
    LayerTimingProposal result{input.start, input.in, input.out, input.speed};
    const auto setRange = [&](const std::int64_t newIn, const std::int64_t newOut) {
        const auto inTime = frameTime(newIn, rate);
        const auto outTime = frameTime(newOut, rate);
        if (!inTime || !outTime)
            return false;
        result.in = *inTime;
        result.out = *outTime;
        return true;
    };
    if (input.handle == 0) {
        // Keep at least one frame inside [0, duration): in <= last - 1 and out >= 1.
        const auto shift =
            input.alt ? deltaFrames : std::clamp(deltaFrames, std::int64_t{1} - out, last - 1 - in);
        const auto shifted = addScaled(input.start, RationalTime::fromInteger(shift),
                                       rate.denominator(), rate.numerator());
        if (!shifted)
            return std::nullopt;
        result.start = *shifted;
        if (!input.alt && !setRange(in + shift, out + shift))
            return std::nullopt;
        return result;
    }
    auto newIn = in;
    auto newOut = out;
    if (input.handle < 0)
        newIn = std::clamp(in + deltaFrames, low, std::min(out - 1, last - 1));
    else
        newOut = std::clamp(out + deltaFrames, std::max(in + 1, std::int64_t{1}), high);
    if (!input.alt)
        return setRange(newIn, newOut) ? std::optional{result} : std::nullopt;
    // Stretch about the opposite edge: speed' = speed * old / new, and the anchored edge keeps its
    // content time, so start' = anchor - (anchor - start) * new / old.
    const auto oldFrames = out - in;
    const auto newFrames = newOut - newIn;
    if (newFrames == oldFrames)
        return setRange(newIn, newOut) ? std::optional{result} : std::nullopt;
    const auto numerator = product(input.speed.numerator(), oldFrames);
    const auto denominator = product(input.speed.denominator(), newFrames);
    const auto speed = numerator && denominator
                           ? document::LayerSpeed::create(*numerator, *denominator)
                           : std::nullopt;
    if (!speed)
        return std::nullopt; // outside the speed bounds: keep the previous preview
    const auto anchor = input.handle < 0 ? input.out : input.in;
    // anchor - start, then scaled by new/old, subtracted from anchor.
    const auto offset = addScaled(anchor, input.start, -1, 1);
    const auto start = offset ? addScaled(anchor, *offset, -newFrames, oldFrames) : std::nullopt;
    if (!start || !setRange(newIn, newOut))
        return std::nullopt;
    result.start = *start;
    result.speed = *speed;
    return result;
}

} // namespace bloom::commands
