#include "value_utility_support.hpp"

#include <bloom/core/color.hpp>
#include <bloom/core/scalar_primitives.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>

namespace {

using namespace bloom;
using runtime::ValueUtilityInvocation;
using runtime::ValueUtilityOutcome;
using runtime::detail::evaluateScalarPrimitive;
using runtime::detail::scalarRoundingPrimitive;
using runtime::detail::selectorAt;
using runtime::detail::shapedOutcome;
using runtime::detail::ValueUtilityReader;

using Kernel = document::ValueUtilityKernel;
using Scalar = core::primitives::ScalarPrimitive;

// SATURATING integer arithmetic. Signed overflow is undefined in C++, and a value graph's operands
// are numbers an artist can drive from anywhere, so every one of these is bounded rather than left
// to wrap. Saturation matches Scalar To Integer's own edge behaviour, so the two agree about what
// happens past the range.
constexpr auto kIntegerHighest = std::numeric_limits<std::int64_t>::max();
constexpr auto kIntegerLowest = std::numeric_limits<std::int64_t>::min();

[[nodiscard]] std::int64_t saturatingAdd(const std::int64_t left,
                                         const std::int64_t right) noexcept {
    if (right > 0 && left > kIntegerHighest - right) {
        return kIntegerHighest;
    }
    if (right < 0 && left < kIntegerLowest - right) {
        return kIntegerLowest;
    }
    return left + right;
}

[[nodiscard]] std::int64_t saturatingSubtract(const std::int64_t left,
                                              const std::int64_t right) noexcept {
    if (right < 0 && left > kIntegerHighest + right) {
        return kIntegerHighest;
    }
    if (right > 0 && left < kIntegerLowest + right) {
        return kIntegerLowest;
    }
    return left - right;
}

[[nodiscard]] std::int64_t saturatingMultiply(const std::int64_t left,
                                              const std::int64_t right) noexcept {
    if (left == 0 || right == 0) {
        return 0;
    }
    const bool negative = (left < 0) != (right < 0);
    // Magnitudes as unsigned values, so the most negative operand has one rather than overflowing
    // its own negation.
    const auto leftMagnitude =
        left < 0 ? ~static_cast<std::uint64_t>(left) + 1 : static_cast<std::uint64_t>(left);
    const auto rightMagnitude =
        right < 0 ? ~static_cast<std::uint64_t>(right) + 1 : static_cast<std::uint64_t>(right);
    const std::uint64_t limit = negative ? static_cast<std::uint64_t>(kIntegerHighest) + 1
                                         : static_cast<std::uint64_t>(kIntegerHighest);
    if (leftMagnitude > limit / rightMagnitude) {
        return negative ? kIntegerLowest : kIntegerHighest;
    }
    const auto magnitude = leftMagnitude * rightMagnitude;
    if (negative) {
        return magnitude == static_cast<std::uint64_t>(kIntegerHighest) + 1
                   ? kIntegerLowest
                   : -static_cast<std::int64_t>(magnitude);
    }
    return static_cast<std::int64_t>(magnitude);
}

[[nodiscard]] std::int64_t integerArithmetic(const document::IntegerOperation operation,
                                             const std::int64_t left,
                                             const std::int64_t right) noexcept {
    switch (operation) {
    case document::IntegerOperation::Add:
        return saturatingAdd(left, right);
    case document::IntegerOperation::Subtract:
        return saturatingSubtract(left, right);
    case document::IntegerOperation::Multiply:
        return saturatingMultiply(left, right);
    case document::IntegerOperation::Divide:
        // Zero, the same documented fallback the scalar tranche gives a division by zero. The
        // second guard is the one signed division that overflows: the most negative value divided
        // by -1 has no representable quotient.
        if (right == 0) {
            return 0;
        }
        if (left == kIntegerLowest && right == -1) {
            return kIntegerHighest;
        }
        return left / right;
    case document::IntegerOperation::Modulo:
        // Truncated division's remainder, which takes the sign of the DIVIDEND -- C's own rule, and
        // the one every language an artist is likely to have met uses for `%`.
        if (right == 0) {
            return 0;
        }
        if (left == kIntegerLowest && right == -1) {
            return 0;
        }
        return left % right;
    case document::IntegerOperation::Minimum:
        return left < right ? left : right;
    case document::IntegerOperation::Maximum:
        return left > right ? left : right;
    }
    return 0;
}

// Wrapped into [minimum, maximum), the half-open interval every repeating parameter uses: a value
// exactly at the maximum is the same position as the minimum, one period along.
[[nodiscard]] double wrapped(const double value, const double minimum,
                             const double maximum) noexcept {
    const double range = maximum - minimum;
    // A degenerate or reversed interval has no period, so the documented fallback is the value
    // UNWRAPPED -- visibly wrong in the picture, where silently swapping the bounds would look
    // correct and hide the authoring mistake. Exactly what Clamp does with reversed bounds.
    if (!(range > 0.0) || !std::isfinite(value) || !std::isfinite(range)) {
        return value;
    }
    const double offset = std::fmod(value - minimum, range);
    return minimum + (offset < 0.0 ? offset + range : offset);
}

// A triangle wave over [0, length]: 0 up to length, back down to 0, repeating. A non-positive
// length has no period at all, so it answers zero.
[[nodiscard]] double pingPong(const double value, const double length) noexcept {
    if (!(length > 0.0) || !std::isfinite(value) || !std::isfinite(length)) {
        return 0.0;
    }
    const double period = length * 2.0;
    const double position = std::fabs(std::fmod(value, period));
    return position <= length ? position : period - position;
}

// Rounded to the nearest multiple of `step`, through the same frozen Round the rest of the library
// uses -- so a Snap and a Rounding node agree about a tie. A zero or non-finite step names no grid,
// so the value passes through.
[[nodiscard]] double snapped(const double value, const double step) {
    if (step == 0.0 || !std::isfinite(step) || !std::isfinite(value)) {
        return value;
    }
    const std::array<double, 1> inputs{value / step};
    return runtime::detail::evaluateScalarPrimitive(Scalar::Round, inputs) * step;
}

constexpr double kRadiansPerDegree = std::numbers::pi / 180.0;
constexpr double kDegreesPerRadian = 180.0 / std::numbers::pi;

} // namespace

namespace bloom::runtime::detail {

ValueUtilityOutcome evaluateValueNumeric(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    switch (invocation.operation) {
    case Kernel::IntegerMath: {
        const auto operation = selectorAt(invocation, 0, document::kIntegerOperations,
                                          document::kDefaultIntegerOperation);
        outcome.outputs[0] = integerArithmetic(operation, reader.integer(0), reader.integer(1));
        break;
    }
    case Kernel::Rounding: {
        const auto mode =
            selectorAt(invocation, 0, document::kRoundingModes, document::kDefaultRoundingMode);
        const std::array<double, 1> inputs{reader.scalar(0)};
        outcome.outputs[0] = evaluateScalarPrimitive(scalarRoundingPrimitive(mode), inputs);
        break;
    }
    case Kernel::Sign: {
        const std::array<double, 1> inputs{reader.scalar(0)};
        outcome.outputs[0] = evaluateScalarPrimitive(Scalar::Sign, inputs);
        break;
    }
    case Kernel::Wrap:
        outcome.outputs[0] = wrapped(reader.scalar(0), reader.scalar(1), reader.scalar(2));
        break;
    case Kernel::Snap:
        outcome.outputs[0] = snapped(reader.scalar(0), reader.scalar(1));
        break;
    case Kernel::PingPong:
        outcome.outputs[0] = pingPong(reader.scalar(0), reader.scalar(1));
        break;
    case Kernel::Smoothstep: {
        // Operand order is the frozen primitive's own: the two edges, then the value. Map Range's
        // Smoothstep interpolation already calls it this way, so the two cannot disagree.
        const std::array<double, 3> inputs{reader.scalar(0), reader.scalar(1), reader.scalar(2)};
        outcome.outputs[0] = evaluateScalarPrimitive(Scalar::Smoothstep, inputs);
        break;
    }
    case Kernel::DegreesToRadians:
        outcome.outputs[0] = reader.scalar(0) * kRadiansPerDegree;
        break;
    case Kernel::RadiansToDegrees:
        outcome.outputs[0] = reader.scalar(0) * kDegreesPerRadian;
        break;
    case Kernel::Rotate2d: {
        const auto vector = reader.vector2(0);
        const auto degrees = reader.scalar(1);
        const auto pivot = reader.vector2(2);
        const double radians = degrees * kRadiansPerDegree;
        const double cosine = std::cos(radians);
        const double sine = std::sin(radians);
        const double x = vector.x - pivot.x;
        const double y = vector.y - pivot.y;
        // Counter-clockwise in a Y-up frame, which is the sense a positive Layer rotation already
        // has; the node does not know which way the composition's Y points, and inventing a second
        // convention here would make two rotations in one graph disagree.
        outcome.outputs[0] =
            document::Vec2d{pivot.x + x * cosine - y * sine, pivot.y + x * sine + y * cosine};
        break;
    }
    case Kernel::PolarToCartesian: {
        const auto radius = reader.scalar(0);
        const double radians = reader.scalar(1) * kRadiansPerDegree;
        outcome.outputs[0] =
            document::Vec2d{radius * std::cos(radians), radius * std::sin(radians)};
        break;
    }
    case Kernel::CartesianToPolar: {
        const auto vector = reader.vector2(0);
        outcome.outputs[0] = std::hypot(vector.x, vector.y);
        // std::atan2's own range, (-180, 180] in degrees, and (0, 0) answers zero rather than a
        // direction it does not have -- the same choice Normalize makes for a zero-length vector.
        outcome.outputs[1] = (vector.x == 0.0 && vector.y == 0.0)
                                 ? 0.0
                                 : std::atan2(vector.y, vector.x) * kDegreesPerRadian;
        break;
    }
    case Kernel::SeparateHsv: {
        const auto hsva = core::toHsva(reader.color(0));
        outcome.outputs[0] = hsva.hue;
        outcome.outputs[1] = hsva.saturation;
        outcome.outputs[2] = hsva.value;
        outcome.outputs[3] = hsva.alpha;
        break;
    }
    case Kernel::CombineHsv:
        outcome.outputs[0] = core::fromHsva(
            {reader.scalar(0), reader.scalar(1), reader.scalar(2), reader.scalar(3)});
        break;
    case Kernel::HueShift: {
        auto hsva = core::toHsva(reader.color(0));
        // Hue is periodic, so the shift wraps rather than clamping: a Hue Shift that ran into a
        // clamp would stop moving instead of going round. core::fromHsva() owns the wrap.
        hsva.hue += reader.scalar(1);
        outcome.outputs[0] = core::fromHsva(hsva);
        break;
    }
    case Kernel::Luminance:
        // Rec.709 weights through the colour module, never three constants retyped here.
        outcome.outputs[0] = core::rec709Luminance(reader.color(0));
        break;
    case Kernel::BooleanLogic: {
        const auto operation = selectorAt(invocation, 0, document::kBooleanOperations,
                                          document::kDefaultBooleanOperation);
        const bool left = reader.boolean(0);
        const bool right = reader.boolean(1);
        bool result = false;
        switch (operation) {
        case document::BooleanOperation::And:
            result = left && right;
            break;
        case document::BooleanOperation::Or:
            result = left || right;
            break;
        case document::BooleanOperation::Xor:
            result = left != right;
            break;
        case document::BooleanOperation::Nand:
            result = !(left && right);
            break;
        case document::BooleanOperation::Nor:
            result = !(left || right);
            break;
        }
        outcome.outputs[0] = result;
        break;
    }
    case Kernel::BooleanNot:
        outcome.outputs[0] = !reader.boolean(0);
        break;
    case Kernel::InRange: {
        const auto value = reader.scalar(0);
        const auto minimum = reader.scalar(1);
        const auto maximum = reader.scalar(2);
        // INCLUSIVE at both ends, and a reversed pair names no interval so nothing is inside it --
        // the same refusal to guess that Clamp makes, expressed as a predicate. NaN is in no
        // interval, which falls out of the comparisons.
        outcome.outputs[0] = value >= minimum && value <= maximum;
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
