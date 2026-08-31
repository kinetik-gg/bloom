#pragma once

#include <cstddef>
#include <cstdint>

namespace bloom::output {

// Shared version 1 analysis, identity-preparation, and digest admission limits.
inline constexpr std::uint32_t kOutputAnalysisMaximumDimensionV1 = 32'768U;
inline constexpr std::uint64_t kOutputAnalysisMaximumPixelCountV1 = 67'108'864U;
inline constexpr std::size_t kOutputAnalysisMaximumProcessPixelBytesV1 = 1'073'741'824U;
inline constexpr std::size_t kOutputAnalysisDigestMaximumPreimageBytesV1 = 4'194'304U;

// frame-output.md's "Version 1 export limits" table's "retained prepared PNG bytes" row. Only the
// PNG preset has a prepared display/output buffer at all (EXR exposes the retained process rows
// directly), so this bounds exactly the one allocation bloom/output/png_export_write.hpp's
// ColorPreparing stage makes: the straight-RGBA8 stream the writer and reopen verifier then share.
//
// Reachability note (see the implementor's report): the closed pixel-count ceiling above is
// 2^26 pixels and RGBA8 is 4 bytes per pixel, so the largest prepared stream the production
// pipeline can ever ask for is exactly 268435456 bytes -- exactly at, and therefore never over,
// this limit ("Values exactly at either limit do not exceed it"). The limit is enforced anyway,
// and a caller may LOWER it ("a request may lower but not raise them"), which is how the
// over-limit rejection is exercised.
inline constexpr std::uint64_t kOutputExportPreparedPngBytesMaximumV1 = 268'435'456ULL;

// frame-output.md's "Version 1 export limits" table names "one encoder or verifier streaming
// chunk" at 16 MiB but no adapter existed yet to materialize it as a bound. The flat OpenEXR
// writer/verifier (issue #99) is the first consumer: it caps how many scanlines it touches per
// write/read call so cancellation is checked at a bounded cadence instead of in one blocking call
// spanning the whole image.
inline constexpr std::size_t kOutputAdapterMaximumStreamingChunkBytesV1 = 16'777'216U;

} // namespace bloom::output
