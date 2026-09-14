#include "value_utility_support.hpp"

namespace bloom::runtime {

// The one entry point, dispatching by FAMILY rather than by kernel. Each family owns a translation
// unit of its own, so adding a string operation touches the string file and nothing else, and no
// single file carries the whole library.
ValueUtilityOutcome evaluateValueUtility(const ValueUtilityInvocation& invocation) {
    return detail::evaluateValueConversion(invocation);
}

} // namespace bloom::runtime
