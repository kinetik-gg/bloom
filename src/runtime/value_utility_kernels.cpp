#include "value_utility_support.hpp"

#include <cstdint>

namespace {

using Kernel = bloom::document::ValueUtilityKernel;

// Which translation unit owns a kernel. This switch is EXHAUSTIVE and carries no default arm, which
// is the point: it is the one place a new library node has to be routed, and the compiler refuses a
// kernel nobody claimed. Each family's own switch may then end in a default, because nothing
// outside its family can reach it.
enum class Family : std::uint8_t {
    Conversion,
    Time,
    String,
    Numeric,
    DataBlocks,
};

[[nodiscard]] Family familyOf(const Kernel kernel) noexcept {
    switch (kernel) {
    case Kernel::ScalarToString:
    case Kernel::IntegerToString:
    case Kernel::StringToScalar:
    case Kernel::StringToInteger:
    case Kernel::ScalarToInteger:
    case Kernel::IntegerToScalar:
    case Kernel::BooleanToScalar:
    case Kernel::BooleanToInteger:
    case Kernel::ScalarToBoolean:
    case Kernel::IntegerToBoolean:
    case Kernel::BooleanToString:
    case Kernel::StringToBoolean:
    case Kernel::ColorToString:
    case Kernel::StringToColor:
    case Kernel::ColorToVector3:
    case Kernel::Vector3ToColor:
    case Kernel::Vector2ToVector3:
    case Kernel::Vector3ToVector2:
    case Kernel::Vector2ToString:
    case Kernel::Vector3ToString:
        return Family::Conversion;
    case Kernel::SecondsToFrames:
    case Kernel::FramesToSeconds:
    case Kernel::SecondsToTimecode:
    case Kernel::TimecodeToSeconds:
    // The readouts live with the time conversions because a frame number IS a time, and because the
    // three that are not are refused in the same place.
    case Kernel::FrameNumber:
    case Kernel::FrameRate:
    case Kernel::CompositionDuration:
    case Kernel::CompositionSize:
        return Family::Time;
    case Kernel::StringConcatenate:
    case Kernel::StringFormat:
    case Kernel::StringLength:
    case Kernel::StringSubstring:
    case Kernel::StringCharacterAt:
    case Kernel::StringSplit:
    case Kernel::StringReplace:
    case Kernel::StringTrim:
    case Kernel::StringCase:
    case Kernel::StringPad:
    case Kernel::StringRepeat:
    case Kernel::StringContains:
    case Kernel::StringStartsWith:
    case Kernel::StringEndsWith:
    case Kernel::StringEquals:
        return Family::String;
    case Kernel::IntegerMath:
    case Kernel::Rounding:
    case Kernel::Sign:
    case Kernel::Wrap:
    case Kernel::Snap:
    case Kernel::PingPong:
    case Kernel::Smoothstep:
    case Kernel::DegreesToRadians:
    case Kernel::RadiansToDegrees:
    case Kernel::Rotate2d:
    case Kernel::PolarToCartesian:
    case Kernel::CartesianToPolar:
    case Kernel::SeparateHsv:
    case Kernel::CombineHsv:
    case Kernel::HueShift:
    case Kernel::Luminance:
    case Kernel::BooleanLogic:
    case Kernel::BooleanNot:
    case Kernel::InRange:
        return Family::Numeric;
    case Kernel::DataSample:
    case Kernel::RampSample:
    case Kernel::TableLookup:
    case Kernel::PointSetRead:
        return Family::DataBlocks;
    }
    return Family::Conversion;
}

} // namespace

namespace bloom::runtime {

ValueUtilityOutcome evaluateValueUtility(const ValueUtilityInvocation& invocation) {
    auto refused = detail::shapedOutcome(invocation.operation);
    if (invocation.limits.cancellation &&
        invocation.limits.cancellation->isCancellationRequested()) {
        refused.cancelled = true;
        return refused;
    }
    for (std::size_t index = 0; index < invocation.operands.size(); ++index) {
        const auto* value = invocation.operands[index];
        const auto* string = value == nullptr ? nullptr : std::get_if<std::string>(value);
        if (string != nullptr && string->size() > invocation.limits.maxStringBytes) {
            refused.failed = true;
            refused.failedOperand = index;
            refused.summary = "Value utility string budget exceeded";
            refused.detail = "An input string exceeds the per-value byte limit.";
            return refused;
        }
    }
    ValueUtilityOutcome outcome;
    switch (familyOf(invocation.operation)) {
    case Family::Time:
        outcome = detail::evaluateValueTime(invocation);
        break;
    case Family::String:
        outcome = detail::evaluateValueString(invocation);
        break;
    case Family::Numeric:
        outcome = detail::evaluateValueNumeric(invocation);
        break;
    case Family::Conversion:
        outcome = detail::evaluateValueConversion(invocation);
        break;
    case Family::DataBlocks:
        outcome = detail::evaluateValueDataBlock(invocation);
        break;
    }
    if (invocation.limits.cancellation && invocation.limits.cancellation->isCancellationRequested())
        outcome.cancelled = true;
    if (outcome.failed || outcome.cancelled)
        return outcome;
    auto remaining =
        std::min(invocation.limits.maxStringBytes, invocation.limits.availableStringBytes);
    for (std::size_t slot = 0; slot < outcome.outputCount; ++slot) {
        const auto* string = std::get_if<std::string>(&outcome.outputs[slot]);
        if (string == nullptr)
            continue;
        if (string->size() > remaining) {
            outcome.failed = true;
            outcome.summary = "Value utility string budget exceeded";
            outcome.detail =
                "The string output exceeds the per-value or retained-output byte limit.";
            outcome.outputs[slot] = std::string{};
            if (outcome.outputCount > 1 && std::holds_alternative<bool>(outcome.outputs[1]))
                outcome.outputs[1] = false;
        } else {
            remaining -= string->size();
        }
    }
    return outcome;
}

} // namespace bloom::runtime
