#pragma once

#include <bloom/render/image_types.hpp>
#include <bloom/runtime/cancellation.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>

namespace bloom::output {

// The encoder's (and verifier's) input surface: packed straight RGBA8 sRGB rows plus dimensions,
// row-major and tightly packed (row stride == width * 4 bytes, no padding -- exactly
// render::checkedRgba8Layout's own packed-layout rule). This is deliberately narrower than a full
// bloom::color::PreparedDisplayFrame: only the two fields this preset's writer/verifier actually
// need, chosen so wiring PreparedDisplayFrame's real product into this encoder (the next slice's
// ColorPreparing stage) is a trivial two-field aggregate construction --
// `PngRgba8SrgbPreparedStreamV1{preparedFrame.displayWindow().extent(), preparedFrame.pixels()}` --
// with no reshaping, copy, or stride translation. `pixels` is non-owning; the caller keeps it
// alive for the duration of the call.
struct PngRgba8SrgbPreparedStreamV1 final {
    render::ImageExtent dimensions;
    std::span<const render::Rgba8> pixels;
};

// Reported between bounded row chunks (never more finely) so a caller can observe monotonic write
// progress and, in tests, deterministically synchronize a mid-write cancellation instead of racing
// a background thread against an unobserved internal loop.
struct PngWriteProgressV1 final {
    std::uint64_t completedRows = 0;
    std::uint64_t totalRows = 0;

    friend bool operator==(const PngWriteProgressV1&, const PngWriteProgressV1&) noexcept = default;
};

using PngWriteProgressCallbackV1 = std::function<void(const PngWriteProgressV1&)>;

enum class PngWriteStatusV1 : std::uint8_t {
    Written,
    Cancelled,
    Failed,
};

// Version 1 has no partial-success outcome: a Failed or Cancelled result never leaves a usable
// artifact at `destination` (see `PngWriteResultV1::destinationRemoved()`). This adapter does not
// implement the exclusive staging, atomic replace, or durability contract owned by the future
// `StagedArtifactCoordinator` (docs/architecture/frame-output.md, "Atomic Publication") -- it only
// writes to the caller-supplied destination path, which that future coordinator-owned staging file
// is expected to be (mirrors flat_exr_output_adapter.hpp's identical note).
enum class PngWriteErrorCodeV1 : std::uint8_t {
    None,
    // dimensions.width()/height() is zero, or pixels.size() does not equal
    // dimensions.width() * dimensions.height().
    InvalidPreparedStream,
    // Dimensions or pixel/byte counts exceed output_limits.hpp's closed version 1 ceilings.
    ResourceLimitExceeded,
    // The destination file could not be created/opened for writing.
    DestinationUnavailable,
    // A filesystem error occurred while streaming chunk bytes.
    IoFailure,
    // zlib's deflateInit2/deflate returned an unexpected status.
    CompressorFailure,
    AllocationFailure,
    InternalInvariant,
};

class [[nodiscard]] PngWriteResultV1 final {
  public:
    [[nodiscard]] static PngWriteResultV1 written() noexcept;
    [[nodiscard]] static PngWriteResultV1 cancelled(bool destinationRemoved) noexcept;
    [[nodiscard]] static PngWriteResultV1 failed(PngWriteErrorCodeV1 error,
                                                 bool destinationRemoved) noexcept;

    [[nodiscard]] PngWriteStatusV1 status() const noexcept { return status_; }
    [[nodiscard]] PngWriteErrorCodeV1 error() const noexcept { return error_; }
    // True when, after a non-Written outcome, the writer's own best-effort cleanup confirmed the
    // destination path holds no partial artifact (either it removed one or none was ever
    // created). False means a partial file may remain and the caller must not treat the path as
    // clean. Always true when status() is Written (there is nothing to remove).
    [[nodiscard]] bool destinationRemoved() const noexcept { return destinationRemoved_; }

  private:
    PngWriteResultV1(PngWriteStatusV1 status, PngWriteErrorCodeV1 error,
                     bool destinationRemoved) noexcept;

    PngWriteStatusV1 status_ = PngWriteStatusV1::Failed;
    PngWriteErrorCodeV1 error_ = PngWriteErrorCodeV1::InternalInvariant;
    bool destinationRemoved_ = true;
};

// Writes `prepared` as the version 1 `png.ihdr-srgb-idat-iend.v1` profile
// (`PngRgba8SrgbV1`, docs/architecture/frame-output.md "PNG Preset Version 1") to `destination`:
// 8-byte signature; one IHDR (bit depth 8, color type 6, compression/filter/interlace method 0);
// one sRGB (rendering intent 0); one or more contiguous IDAT holding a single zlib (deflate)
// stream of every row's filtered bytes -- filter type 0 (None) fixed on every row, DEFLATE level
// 6/default strategy/32 KiB window (see png_preset_contract.hpp for the exact fixed parameter
// set), split into fixed `kOutputAdapterMaximumStreamingChunkBytesV1`-sized IDAT chunks only when
// the compressed stream exceeds that ceiling; one IEND. CRC-32 per chunk via zlib's `crc32_z`;
// nothing else -- no other chunk. Bloom-owned encoder over the qualified ZLIB only; no OIIO/libpng
// anywhere. Streaming, bounded-chunk, cancellation-checked between row chunks; typed failures; no
// partial success. No zlib type appears in this public header; the real writer lives in a private
// translation unit (same boundary discipline as flat_exr_output_adapter.hpp/bloom_color_ocio).
class PngRgba8SrgbWriterV1 final {
  public:
    [[nodiscard]] PngWriteResultV1
    write(const PngRgba8SrgbPreparedStreamV1& prepared, const std::filesystem::path& destination,
          const runtime::CancellationToken& cancellation,
          const PngWriteProgressCallbackV1& progress = {}) const noexcept;
};

} // namespace bloom::output
