#pragma once

#include <cstddef>
#include <cstdint>

namespace bloom::output {

// Shared version 1 analysis, identity-preparation, and digest admission limits.
inline constexpr std::uint32_t kOutputAnalysisMaximumDimensionV1 = 32'768U;
inline constexpr std::uint64_t kOutputAnalysisMaximumPixelCountV1 = 67'108'864U;
inline constexpr std::size_t kOutputAnalysisMaximumProcessPixelBytesV1 = 1'073'741'824U;
inline constexpr std::size_t kOutputAnalysisDigestMaximumPreimageBytesV1 = 4'194'304U;

// frame-output.md's "Version 1 export limits" table names "one encoder or verifier streaming
// chunk" at 16 MiB but no adapter existed yet to materialize it as a bound. The flat OpenEXR
// writer/verifier (issue #99) is the first consumer: it caps how many scanlines it touches per
// write/read call so cancellation is checked at a bounded cadence instead of in one blocking call
// spanning the whole image.
inline constexpr std::size_t kOutputAdapterMaximumStreamingChunkBytesV1 = 16'777'216U;

} // namespace bloom::output
