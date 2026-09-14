#include "value_utility_support.hpp"

#include <bloom/runtime/value_graph_evaluation.hpp>

#include <bloom/core/safe_parse.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace bloom;
using runtime::ValueUtilityInvocation;
using runtime::ValueUtilityOutcome;
using runtime::detail::shapedOutcome;
using runtime::detail::ValueUtilityReader;

using Kernel = document::ValueUtilityKernel;

// A composition's rate as a double. Exact for every rate a document can hold: both parts are
// std::uint32_t, so the quotient is a ratio of two exactly representable integers.
[[nodiscard]] double rateAsScalar(const document::FrameRate rate) noexcept {
    return static_cast<double>(rate.numerator()) / static_cast<double>(rate.denominator());
}

// The NOMINAL integer frame count a timecode's frame field counts to: 24 at 24, 25 at 25, and 30 at
// 30000/1001. Non-drop timecode counts whole frames per labelled second, which is why the label
// drifts from wall clock at a fractional rate -- that drift is what non-drop timecode IS, and
// correcting it is what drop-frame does, which Bloom does not support.
[[nodiscard]] std::int64_t nominalFrameCount(const document::FrameRate rate) noexcept {
    const auto rounded = static_cast<std::int64_t>(std::lround(rateAsScalar(rate)));
    return rounded < 1 ? 1 : rounded;
}

// Seconds as an exact frame index, floored, saturating at the signed range. Floor rather than round
// for the same reason valueGraphFrameIndex() floors: the frame a time falls INSIDE is the frame
// being rendered, and rounding would name the next one for the second half of every frame.
[[nodiscard]] std::int64_t framesForSeconds(const double seconds,
                                            const document::FrameRate rate) noexcept {
    if (std::isnan(seconds)) {
        return 0;
    }
    const double frames = std::floor(seconds * rateAsScalar(rate));
    constexpr double kUpperBound = 9223372036854775808.0;
    if (frames >= kUpperBound) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (frames < -kUpperBound) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(frames);
}

[[nodiscard]] std::string twoDigits(const std::int64_t value) {
    return core::formatInteger(value, 10, 2);
}

// `HH:MM:SS:FF`, non-drop. Hours are NOT wrapped at 24 and not truncated to two digits: a
// composition may legitimately be longer than a day, and a label that silently rolled over would
// name the wrong instant. A negative time carries a leading `-` on the whole label rather than on
// one field.
[[nodiscard]] std::string timecodeForFrames(const std::int64_t frameIndex,
                                            const document::FrameRate rate) {
    const auto nominal = nominalFrameCount(rate);
    const bool negative = frameIndex < 0;
    // The magnitude as an unsigned value first, so the most negative index has one rather than
    // overflowing its own negation.
    const auto magnitude = negative ? ~static_cast<std::uint64_t>(frameIndex) + 1
                                    : static_cast<std::uint64_t>(frameIndex);
    const auto count = static_cast<std::uint64_t>(nominal);
    const auto frames = static_cast<std::int64_t>(magnitude % count);
    const auto totalSeconds = magnitude / count;
    const auto seconds = static_cast<std::int64_t>(totalSeconds % 60U);
    const auto totalMinutes = totalSeconds / 60U;
    const auto minutes = static_cast<std::int64_t>(totalMinutes % 60U);
    const auto hours = static_cast<std::int64_t>(totalMinutes / 60U);
    std::string label;
    if (negative) {
        label.push_back('-');
    }
    label.append(twoDigits(hours));
    label.push_back(document::kTimecodeFieldSeparator);
    label.append(twoDigits(minutes));
    label.push_back(document::kTimecodeFieldSeparator);
    label.append(twoDigits(seconds));
    label.push_back(document::kTimecodeFieldSeparator);
    label.append(twoDigits(frames));
    return label;
}

// `HH:MM:SS:FF`, `MM:SS:FF` or `SS:FF`, with an optional leading sign, after trimming. Every field
// is one or more ASCII digits and nothing else -- no spaces around the colons, no `;` in place of
// the last one (that spelling MEANS drop-frame, which Bloom does not support, so accepting it would
// silently read a drop-frame label as non-drop).
//
// The shorter forms are accepted because an artist typing a shot-length offset writes `02:12` or
// `01:30:00`, not `00:00:02:12`. Fields are read from the RIGHT, so the last one is always frames.
[[nodiscard]] std::optional<std::int64_t> framesForTimecode(const std::string_view text,
                                                            const document::FrameRate rate) {
    auto trimmed = core::trimAsciiWhitespace(text);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    bool negative = false;
    if (trimmed.front() == '+' || trimmed.front() == '-') {
        negative = trimmed.front() == '-';
        trimmed.remove_prefix(1);
    }
    std::array<std::int64_t, 4> fields{};
    std::size_t count = 0;
    std::size_t start = 0;
    while (true) {
        const auto separator = trimmed.find(document::kTimecodeFieldSeparator, start);
        const auto piece =
            trimmed.substr(start, separator == std::string_view::npos ? std::string_view::npos
                                                                      : separator - start);
        if (count >= fields.size()) {
            return std::nullopt;
        }
        // The field readers are core::parseInteger()'s own: no sign inside a field, digits only,
        // and an overflowing magnitude refused rather than wrapped.
        if (piece.empty() || piece.front() == '+' || piece.front() == '-') {
            return std::nullopt;
        }
        const auto value = core::parseInteger(piece, 10);
        if (!value.has_value()) {
            return std::nullopt;
        }
        fields[count++] = *value;
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }
    if (count < 2) {
        return std::nullopt;
    }
    // Right-aligned: the last field is always frames, the one before it seconds, and so on.
    std::array<std::int64_t, 4> ordered{};
    for (std::size_t index = 0; index < count; ++index) {
        ordered[ordered.size() - count + index] = fields[index];
    }
    const auto hours = ordered[0];
    const auto minutes = ordered[1];
    const auto seconds = ordered[2];
    const auto frames = ordered[3];
    const auto nominal = nominalFrameCount(rate);
    // A field past its own modulus names no instant: 61 seconds is a minute the label already has a
    // column for, and a frame field at or past the nominal count names a frame the second does not
    // contain.
    if (minutes >= 60 || seconds >= 60 || frames >= nominal) {
        return std::nullopt;
    }
    // Bounded so the accumulation below cannot overflow: a label past this is not a time anything
    // in a composition addresses.
    constexpr std::int64_t kMaximumHours = 1'000'000;
    if (hours > kMaximumHours) {
        return std::nullopt;
    }
    const auto total = ((hours * 60 + minutes) * 60 + seconds) * nominal + frames;
    return negative ? -total : total;
}

} // namespace

namespace bloom::runtime::detail {

ValueUtilityOutcome evaluateValueTime(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    switch (invocation.operation) {
    case Kernel::SecondsToFrames:
        outcome.outputs[0] = framesForSeconds(reader.scalar(0), invocation.rate);
        break;
    case Kernel::FramesToSeconds: {
        // The exact inverse at a frame-aligned time: a frame index times the rate's reciprocal,
        // with both parts of the rate exactly representable.
        const auto frames = reader.integer(0);
        outcome.outputs[0] =
            static_cast<double>(frames) * (static_cast<double>(invocation.rate.denominator()) /
                                           static_cast<double>(invocation.rate.numerator()));
        break;
    }
    case Kernel::SecondsToTimecode:
        outcome.outputs[0] =
            timecodeForFrames(framesForSeconds(reader.scalar(0), invocation.rate), invocation.rate);
        break;
    case Kernel::FrameNumber: {
        // The SAME answer a Time node's frame output gives, from the same exact rational
        // arithmetic, so the two can never name different frames for one instant.
        const auto frame = runtime::valueGraphFrameIndex(invocation.time, invocation.rate);
        if (!frame.has_value()) {
            outcome.failed = true;
            outcome.failedOperand = 0;
            outcome.summary = "Frame number is not representable at this time";
            outcome.detail = "The request time multiplied by the frame rate overflows an exact "
                             "integer.";
            break;
        }
        outcome.outputs[0] = *frame;
        break;
    }
    case Kernel::FrameRate:
    case Kernel::CompositionDuration:
    case Kernel::CompositionSize:
        // Baked into a constant by the compiler, so one reaching the evaluator means the plan
        // disagrees with the lowering that produced it.
        outcome.failed = true;
        outcome.failedOperand = 0;
        outcome.summary = "Composition readout was not lowered to a constant";
        outcome.detail = "A composition readout is compiled from the document snapshot, not "
                         "evaluated per frame.";
        break;
    case Kernel::TimecodeToSeconds: {
        const auto text = reader.text(0);
        const auto fallback = reader.scalar(1);
        const auto frames = framesForTimecode(text, invocation.rate);
        // Back through the NOMINAL count, not the actual rate: a non-drop label counts nominal
        // frames per labelled second, and reading it through the fractional rate would answer a
        // different instant than the one the label names.
        const double seconds = frames.has_value()
                                   ? static_cast<double>(*frames) *
                                         (static_cast<double>(invocation.rate.denominator()) /
                                          static_cast<double>(invocation.rate.numerator()))
                                   : fallback;
        outcome.outputs[0] = seconds;
        outcome.outputs[1] = frames.has_value();
        break;
    }
    default:
        // evaluateValueUtility()'s own switch is exhaustive over every kernel and routes each to
        // exactly one family, so nothing outside this one reaches here.
        break;
    }
    noteMalformedOperand(outcome, reader);
    return outcome;
}

} // namespace bloom::runtime::detail
