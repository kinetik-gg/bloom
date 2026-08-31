#pragma once

// Private (non-FILE_SET, non-installed) header shared by the PNG writer and reopen verifier,
// mirroring flat_exr_preset_contract.hpp's role for the flat OpenEXR pair: one literal table
// copied from docs/architecture/frame-output.md ("PNG Preset Version 1" and "Determinism And
// Portable Output Identity"), so the writer and verifier stay driven by the same constants instead
// of by each other. Nothing here names a zlib type; both translation units include <zlib.h>
// themselves for the calls they actually make (deflate*/inflate*/crc32_z), keeping this header a
// plain literal table like its EXR counterpart.

#include <array>
#include <cstddef>
#include <cstdint>

namespace bloom::output::detail {

// The eight-byte PNG signature (ISO/IEC 15948, PNG Preset Version 1).
inline constexpr std::array<std::uint8_t, 8> kPngSignatureV1{137, 80, 78, 71, 13, 10, 26, 10};

// IHDR fixed field values -- design decision 2 / "PNG Preset Version 1": bit depth 8, color type 6
// (truecolor+alpha), compression method 0, filter method 0 (per-scanline filtering, distinct from
// the per-row filter TYPE byte below), interlace method 0 (non-interlaced).
inline constexpr std::uint8_t kPngBitDepthV1 = 8;
inline constexpr std::uint8_t kPngColorTypeRgbaV1 = 6;
inline constexpr std::uint8_t kPngCompressionMethodV1 = 0;
inline constexpr std::uint8_t kPngFilterMethodV1 = 0;
inline constexpr std::uint8_t kPngInterlaceMethodV1 = 0;

// The fixed per-scanline filter TYPE byte every row's filtered data begins with -- design decision
// 2: "fixed filter type 0 (None) on every row". None means the row's filtered bytes are its
// literal sample bytes; no arithmetic reconstruction is needed on read-back.
inline constexpr std::uint8_t kPngFilterTypeNoneV1 = 0;

// sRGB rendering intent 0 (perceptual) -- the kind-1 payload's "PNG sRGB rendering-intent byte".
inline constexpr std::uint8_t kPngSrgbRenderingIntentV1 = 0;

inline constexpr std::array<char, 4> kPngChunkTypeIhdrV1{'I', 'H', 'D', 'R'};
inline constexpr std::array<char, 4> kPngChunkTypeSrgbV1{'s', 'R', 'G', 'B'};
inline constexpr std::array<char, 4> kPngChunkTypeIdatV1{'I', 'D', 'A', 'T'};
inline constexpr std::array<char, 4> kPngChunkTypeIendV1{'I', 'E', 'N', 'D'};

inline constexpr std::size_t kPngIhdrDataBytesV1 = 13;
inline constexpr std::size_t kPngSrgbDataBytesV1 = 1;
inline constexpr std::size_t kPngIendDataBytesV1 = 0;

// Fixed, explicitly-set deflate parameters -- design decision 2: "deflate with FIXED,
// explicitly-set parameters (level, strategy, window) so bytes are deterministic under the
// qualified zlib". Level 6 and PNG row-filter type 0 are the doc's own "PNG Preset Version 1"
// wording ("DEFLATE level 6, default zlib strategy, and PNG row-filter type 0"). windowBits is
// positive 15: a zlib-wrapped stream (2-byte header + 4-byte Adler-32 trailer, not raw deflate) at
// the doc's own "32 KiB window" ("Flat OpenEXR"/"PNG Preset Version 1" sections both name this
// exact IDAT shape). memLevel 8 is zlib's documented default, matching
// src/project/zip_container_writer.cpp's own deflateInit2 call (used here for parity, not because
// the doc names it -- the doc is silent on memLevel, and 8 is the value deflateInit()'s convenience
// wrapper always uses). kPngDeflateStrategyV1's value (0) is Z_DEFAULT_STRATEGY from zlib.h; it is
// spelled out numerically (with the writer/verifier .cpp files' own static_assert against the real
// macro) so this literal-table header never needs a zlib.h include for a single named constant.
inline constexpr int kPngDeflateLevelV1 = 6;
inline constexpr int kPngDeflateWindowBitsV1 = 15;
inline constexpr int kPngDeflateMemLevelV1 = 8;
inline constexpr int kPngDeflateStrategyV1 = 0; // Z_DEFAULT_STRATEGY

// IDAT split rule -- design decision 2: "single IDAT chunk unless the checked chunk-size ceiling
// forces splitting (then deterministic fixed-size splits)". The checked chunk-size ceiling reused
// here is output_limits.hpp's existing kOutputAdapterMaximumStreamingChunkBytesV1 (16 MiB): the
// same "one encoder or verifier streaming chunk" ceiling the flat OpenEXR writer/verifier already
// apply to their own scanline chunking, so the compressed IDAT payload is split into fixed
// 16 MiB chunks (the final chunk shorter) whenever it exceeds that bound, and left as one chunk
// otherwise. No PNG-specific ceiling is added: the doc's PNG section names no chunk-size number of
// its own, only the general "Version 1 export limits" table's per-chunk figure that
// kOutputAdapterMaximumStreamingChunkBytesV1 already materializes (frame-output.md, "Version 1
// export limits", "one encoder or verifier streaming chunk").

} // namespace bloom::output::detail
