#include "value_utility_support.hpp"

#include <bloom/core/safe_parse.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace {

using namespace bloom;
using runtime::CompiledValue;
using runtime::ValueUtilityInvocation;
using runtime::ValueUtilityOutcome;
using runtime::detail::radixAt;
using runtime::detail::selectorAt;
using runtime::detail::shapedOutcome;
using runtime::detail::ValueUtilityReader;

using Kernel = document::ValueUtilityKernel;

// The widest decimal count and the widest pad a conversion's operands may ask for. Both are already
// clamped by core::formatScalar(); clamping here as well keeps the number that reaches the
// formatter the same number a test can predict from the operand.
constexpr std::int32_t kMaximumDecimals = core::kMaximumFormattedDecimals;
constexpr std::int32_t kMaximumPadWidth = core::kMaximumFormattedWidth;

// A Scalar as an exact integer under the chosen rounding, SATURATING at the signed range rather
// than wrapping. A double past 2^63 names no std::int64_t at all, and the nearest one it does name
// is a better answer than an undefined conversion; NaN names none in either direction, so it
// answers zero.
[[nodiscard]] std::int64_t roundToInteger(const double value,
                                          const document::RoundingMode mode) noexcept {
    if (std::isnan(value)) {
        return 0;
    }
    double rounded = value;
    switch (mode) {
    case document::RoundingMode::Round:
        // Ties away from zero, which is std::round's own rule and the one an artist means by
        // "round": 0.5 is 1 and -0.5 is -1.
        rounded = std::round(value);
        break;
    case document::RoundingMode::Floor:
        rounded = std::floor(value);
        break;
    case document::RoundingMode::Ceiling:
        rounded = std::ceil(value);
        break;
    case document::RoundingMode::Truncate:
        rounded = std::trunc(value);
        break;
    }
    // 2^63 exactly: the first double past the signed range, and the comparison is exact because
    // both sides are powers of two.
    constexpr double kUpperBound = 9223372036854775808.0;
    if (rounded >= kUpperBound) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (rounded < -kUpperBound) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(rounded);
}

// Nonzero is true, and NaN is FALSE. NaN compares unequal to zero, so the obvious `value != 0`
// would call it true -- which would make "is this number set" answer yes for a number that is not a
// number at all.
[[nodiscard]] bool scalarTruth(const double value) noexcept {
    return !std::isnan(value) && value != 0.0;
}

[[nodiscard]] std::string joinComponents(const std::span<const double> components,
                                         const std::string_view separator,
                                         const std::int32_t decimals) {
    std::string text;
    for (std::size_t index = 0; index < components.size(); ++index) {
        if (index != 0) {
            text.append(separator);
        }
        text.append(core::formatScalar(components[index], decimals, false, 0));
    }
    return text;
}

} // namespace

namespace bloom::runtime::detail {

ValueUtilityOutcome evaluateValueConversion(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    // `valid` on a parsing node: written by the parse, never by the operand read. A malformed plan
    // is a separate failure the outcome reports on its own.
    const auto parsed = [&outcome](CompiledValue value, const bool valid) {
        outcome.outputs[0] = std::move(value);
        outcome.outputs[1] = valid;
    };
    switch (invocation.operation) {
    case Kernel::ScalarToString: {
        const auto value = reader.scalar(0);
        const auto decimals = reader.bounded(1, 0, kMaximumDecimals);
        const auto trim = reader.boolean(2);
        const auto padWidth = reader.bounded(3, 0, kMaximumPadWidth);
        const auto prefix = reader.text(4);
        const auto suffix = reader.text(5);
        std::string text(prefix);
        text.append(core::formatScalar(value, decimals, trim, padWidth));
        text.append(suffix);
        outcome.outputs[0] = std::move(text);
        break;
    }
    case Kernel::IntegerToString: {
        const auto value = reader.integer(0);
        const auto padWidth = reader.bounded(1, 0, kMaximumPadWidth);
        const auto prefix = reader.text(2);
        const auto suffix = reader.text(3);
        std::string text(prefix);
        text.append(core::formatInteger(value, radixAt(invocation, 0), padWidth));
        text.append(suffix);
        outcome.outputs[0] = std::move(text);
        break;
    }
    case Kernel::BooleanToString: {
        const auto value = reader.boolean(0);
        outcome.outputs[0] = std::string(value ? reader.text(1) : reader.text(2));
        break;
    }
    case Kernel::ColorToString: {
        const auto color = reader.color(0);
        outcome.outputs[0] = core::formatHexColor(color.red, color.green, color.blue, color.alpha,
                                                  reader.boolean(1), reader.boolean(2));
        break;
    }
    case Kernel::Vector2ToString: {
        const auto vector = reader.vector2(0);
        const auto separator = reader.text(1);
        const auto decimals = reader.bounded(2, 0, kMaximumDecimals);
        const std::array<double, 2> components{vector.x, vector.y};
        outcome.outputs[0] = joinComponents(components, separator, decimals);
        break;
    }
    case Kernel::Vector3ToString: {
        const auto vector = reader.vector3(0);
        const auto separator = reader.text(1);
        const auto decimals = reader.bounded(2, 0, kMaximumDecimals);
        const std::array<double, 3> components{vector.x, vector.y, vector.z};
        outcome.outputs[0] = joinComponents(components, separator, decimals);
        break;
    }
    case Kernel::StringToScalar: {
        const auto text = reader.text(0);
        const auto fallback = reader.scalar(1);
        const auto value = core::parseScalar(text);
        parsed(value.value_or(fallback), value.has_value());
        break;
    }
    case Kernel::StringToInteger: {
        const auto text = reader.text(0);
        const auto fallback = reader.integer(1);
        const auto value = core::parseInteger(text, radixAt(invocation, 0));
        parsed(value.value_or(fallback), value.has_value());
        break;
    }
    case Kernel::StringToBoolean: {
        const auto text = reader.text(0);
        const auto fallback = reader.boolean(1);
        const auto value = core::parseBoolean(text);
        parsed(value.value_or(fallback), value.has_value());
        break;
    }
    case Kernel::StringToColor: {
        const auto text = reader.text(0);
        const auto fallback = reader.color(1);
        const auto channels = core::parseHexColor(text);
        parsed(channels.has_value()
                   ? core::Color4d{channels->red, channels->green, channels->blue, channels->alpha}
                   : fallback,
               channels.has_value());
        break;
    }
    case Kernel::ScalarToInteger:
        outcome.outputs[0] =
            roundToInteger(reader.scalar(0), selectorAt(invocation, 0, document::kRoundingModes,
                                                        document::kDefaultRoundingMode));
        break;
    case Kernel::IntegerToScalar:
        // Exact for every magnitude below 2^53; past that the nearest double is the only answer
        // binary64 has, which is the same widening ValuePromotion::IntegerToScalar performs.
        outcome.outputs[0] = static_cast<double>(reader.integer(0));
        break;
    case Kernel::BooleanToScalar:
        outcome.outputs[0] = reader.boolean(0) ? 1.0 : 0.0;
        break;
    case Kernel::BooleanToInteger:
        outcome.outputs[0] = reader.boolean(0) ? std::int64_t{1} : std::int64_t{0};
        break;
    case Kernel::ScalarToBoolean:
        outcome.outputs[0] = scalarTruth(reader.scalar(0));
        break;
    case Kernel::IntegerToBoolean:
        outcome.outputs[0] = reader.integer(0) != 0;
        break;
    case Kernel::ColorToVector3: {
        // RGB only. Alpha is not a colour component an artist does vector arithmetic on, and a
        // Vector 3 has nowhere to put it; Separate RGBA is how a graph reads alpha as a number.
        const auto color = reader.color(0);
        outcome.outputs[0] = document::Vec3d{color.red, color.green, color.blue};
        break;
    }
    case Kernel::Vector3ToColor: {
        const auto vector = reader.vector3(0);
        outcome.outputs[0] = core::Color4d{vector.x, vector.y, vector.z, reader.scalar(1)};
        break;
    }
    case Kernel::Vector2ToVector3: {
        const auto vector = reader.vector2(0);
        outcome.outputs[0] = document::Vec3d{vector.x, vector.y, reader.scalar(1)};
        break;
    }
    case Kernel::Vector3ToVector2: {
        // Z is DROPPED, not projected: this is a reinterpretation of the first two components, and
        // a perspective divide would be a different operation with a camera behind it.
        const auto vector = reader.vector3(0);
        outcome.outputs[0] = document::Vec2d{vector.x, vector.y};
        break;
    }
    }
    noteMalformedOperand(outcome, reader);
    return outcome;
}

} // namespace bloom::runtime::detail
