#pragma once

#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>

namespace bloom::output {

// Reported between bounded scanline chunks (never more finely) so a caller can observe monotonic
// write progress and, in tests, deterministically synchronize a mid-write cancellation instead of
// racing a background thread against an unobserved internal loop.
struct FlatExrWriteProgressV1 final {
    std::uint64_t completedScanlines = 0;
    std::uint64_t totalScanlines = 0;

    friend bool operator==(const FlatExrWriteProgressV1&,
                           const FlatExrWriteProgressV1&) noexcept = default;
};

using FlatExrWriteProgressCallbackV1 = std::function<void(const FlatExrWriteProgressV1&)>;

enum class FlatExrWriteStatusV1 : std::uint8_t {
    Written,
    Cancelled,
    Failed,
};

// Version 1 has no partial-success outcome: a Failed or Cancelled result never leaves a usable
// artifact at `destination`, and the writer performs its own best-effort cleanup of any partial
// bytes it created (see `FlatExrWriteResultV1::destinationRemoved`). This adapter does not
// implement the exclusive staging, atomic replace, or durability contract owned by the future
// `StagedArtifactCoordinator` (docs/architecture/frame-output.md, "Atomic Publication") -- it only
// writes to the caller-supplied destination path, which that future coordinator-owned staging
// file is expected to be.
enum class FlatExrWriteErrorCodeV1 : std::uint8_t {
    None,
    // The process data or display window does not fit the checked OpenEXR signed-32 inclusive
    // coordinate domain.
    WindowOutOfRange,
    // The source pixel aspect rounds to a non-finite, non-positive, or otherwise unrepresentable
    // binary32 value. (Unreachable through `core::PixelAspectRatio`'s own positive-uint32
    // invariant in practice; guarded anyway as the declared checked conversion boundary.)
    InvalidPixelAspectRatio,
    // Dimensions or pixel/byte counts exceed `output_limits.hpp`'s closed version 1 ceilings.
    ResourceLimitExceeded,
    // The destination file could not be created/opened for writing.
    DestinationUnavailable,
    // A filesystem or encoder error occurred while streaming scanlines.
    IoFailure,
    AllocationFailure,
    InternalInvariant,
};

class [[nodiscard]] FlatExrWriteResultV1 final {
  public:
    [[nodiscard]] static FlatExrWriteResultV1 written() noexcept;
    [[nodiscard]] static FlatExrWriteResultV1 cancelled(bool destinationRemoved) noexcept;
    [[nodiscard]] static FlatExrWriteResultV1 failed(FlatExrWriteErrorCodeV1 error,
                                                     bool destinationRemoved) noexcept;

    [[nodiscard]] FlatExrWriteStatusV1 status() const noexcept { return status_; }
    [[nodiscard]] FlatExrWriteErrorCodeV1 error() const noexcept { return error_; }
    // True when, after a non-Written outcome, the writer's own best-effort cleanup confirmed the
    // destination path holds no partial artifact (either it removed one or none was ever
    // created). False means a partial file may remain and the caller must not treat the path as
    // clean. Always true when status() is Written (there is nothing to remove).
    [[nodiscard]] bool destinationRemoved() const noexcept { return destinationRemoved_; }

  private:
    FlatExrWriteResultV1(FlatExrWriteStatusV1 status, FlatExrWriteErrorCodeV1 error,
                         bool destinationRemoved) noexcept;

    FlatExrWriteStatusV1 status_ = FlatExrWriteStatusV1::Failed;
    FlatExrWriteErrorCodeV1 error_ = FlatExrWriteErrorCodeV1::InternalInvariant;
    bool destinationRemoved_ = true;
};

// Writes `frame`'s retained process image as a version 1 flat OpenEXR
// (`FlatExrRgba32fLinRec709SceneV1`, docs/architecture/frame-output.md) to `destination`.
//
// Consumes an immutable `ProcessFrame` view and the preset's own fixed parameters; there is no
// caller-supplied preset configuration. Premultiplied Float32 RGBA samples are bit-copied with no
// numeric, color, alpha, or precision conversion. Scanlines are written in increasing-Y order,
// incrementally, with cancellation checked between bounded chunks
// (`kOutputAdapterMaximumStreamingChunkBytesV1`). No OpenEXR/Imath type, enum, pointer, or
// exception appears in this public header; the real writer lives in a private translation unit
// (same boundary discipline as `bloom_color_ocio`).
class FlatExrRgba32fLinRec709SceneWriterV1 final {
  public:
    [[nodiscard]] FlatExrWriteResultV1
    write(const runtime::ProcessFrame& frame, const std::filesystem::path& destination,
          const runtime::CancellationToken& cancellation,
          const FlatExrWriteProgressCallbackV1& progress = {}) const noexcept;
};

} // namespace bloom::output
