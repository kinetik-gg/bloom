#pragma once

#include <cstddef>
#include <cstdint>

namespace bloom::output {

// Shared version 1 analysis, identity-preparation, and digest admission limits.
inline constexpr std::uint32_t kOutputAnalysisMaximumDimensionV1 = 32'768U;
inline constexpr std::uint64_t kOutputAnalysisMaximumPixelCountV1 = 67'108'864U;
inline constexpr std::size_t kOutputAnalysisMaximumProcessPixelBytesV1 = 1'073'741'824U;
inline constexpr std::size_t kOutputAnalysisDigestMaximumPreimageBytesV1 = 4'194'304U;

} // namespace bloom::output
