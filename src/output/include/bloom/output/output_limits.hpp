#pragma once

#include <cstddef>
#include <cstdint>

namespace bloom::output {

// The digest's bounded canonical metadata preimage is an identity format constraint, not a
// limit on image pixels. Pixel storage is checked for representability and admitted at the live
// owner, independently of identity semantics.
inline constexpr std::size_t kOutputAnalysisDigestMaximumPreimageBytesV1 = 4'194'304U;

// Compatibility for the host publication default until its rooted admission migration. The PNG
// writer itself no longer clamps to this value; remove this symbol with the host default.
inline constexpr std::uint64_t kOutputExportPreparedPngBytesMaximumV1 = 268'435'456ULL;

// frame-output.md's "Version 1 export limits" table names "one encoder or verifier streaming
// chunk" at 16 MiB but no adapter existed yet to materialize it as a bound. The flat OpenEXR
// writer/verifier (issue #99) is the first consumer: it caps how many scanlines it touches per
// write/read call so cancellation is checked at a bounded cadence instead of in one blocking call
// spanning the whole image.
inline constexpr std::size_t kOutputAdapterMaximumStreamingChunkBytesV1 = 16'777'216U;

} // namespace bloom::output
