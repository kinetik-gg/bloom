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
        return Family::Time;
    }
    return Family::Conversion;
}

} // namespace

namespace bloom::runtime {

ValueUtilityOutcome evaluateValueUtility(const ValueUtilityInvocation& invocation) {
    switch (familyOf(invocation.operation)) {
    case Family::Time:
        return detail::evaluateValueTime(invocation);
    case Family::Conversion:
        break;
    }
    return detail::evaluateValueConversion(invocation);
}

} // namespace bloom::runtime
