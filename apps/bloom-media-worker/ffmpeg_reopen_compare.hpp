#pragma once
// The declared sample domain of an encoded video stream and the reopen comparison built on it
// (tolerance profile v2). Worker-private: FFmpeg types never leave the media worker.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
namespace bloom::media::ffmpeg {
// What a stream declares about its samples: matrix, range and chroma siting. The sample format
// itself (and so its subsampling) comes from the frame.
struct DeclaredSampling {
    AVColorSpace matrix = AVCOL_SPC_UNSPECIFIED;
    AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
    AVChromaLocation siting = AVCHROMA_LOC_UNSPECIFIED;
};
// Unspecified siting is the H.273 default that ProRes, DNxHR, H.264 and HEVC all use: chroma
// co-sited with the left luma sample, vertically centred for 4:2:0.
[[nodiscard]] AVChromaLocation effectiveSiting(AVChromaLocation siting) noexcept;
// The swscale coefficient table for a declared matrix, or -1 when it is not a supported Y'CbCr
// matrix. Unspecified is BT.709, which Bloom has always written for an untagged matrix.
[[nodiscard]] int swsCoefficients(AVColorSpace matrix) noexcept;
// Same-size conversion between `from` and `to` under the declared sampling: the declared matrix
// and range on the Y'CbCr side, and the declared siting on every subsampled axis. Returns null when
// the conversion is unavailable. The encoder's output conversion and the verifier's reference are
// both built here, so the codec's own sampling is modelled exactly once.
[[nodiscard]] SwsContext* declaredConversion(int width, int height, AVPixelFormat from,
                                             AVPixelFormat to, const DeclaredSampling& sampling,
                                             unsigned flags);
// RGBA16 -> encoder sample format (siting-aware chroma decimation).
inline constexpr unsigned kOutputConversionFlags = SWS_BICUBIC | SWS_FULL_CHR_H_INP | SWS_BITEXACT;
// Decoded sample format -> RGBA16 (siting-aware chroma reconstruction).
inline constexpr unsigned kReconstructionFlags =
    SWS_BILINEAR | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND | SWS_BITEXACT;
// Y'CbCr samples in `format` -> RGBA16 (little-endian, 8 bytes per pixel) under the declared
// sampling: chroma reconstructed at the declared siting, then the declared matrix and range applied
// with clipping. False when the conversion is unavailable.
[[nodiscard]] bool reconstructYuv(int width, int height, AVPixelFormat format,
                                  const std::uint8_t* const* planes, const int* strides,
                                  const DeclaredSampling& declared, std::vector<std::byte>& out);
struct ReopenMeasure {
    // The decoded frame reconstructed to RGBA16 (little-endian, 8 bytes per pixel).
    std::vector<std::byte> rgba;
    // Colour channel error against the declared-sampling reference, in unsigned 16-bit units.
    std::uint32_t maximum = 0;
    std::uint64_t sum = 0, count = 0;
    // Alpha error against the unsampled reference alpha.
    std::uint32_t alphaMaximum = 0;
    // Non-empty when the comparison could not be made at all.
    std::string failure;
};
// Measures a decoded frame against the RGBA16 reference the encoder received. For a Y'CbCr frame
// the reference is first taken through the frame's own sample format under the declared sampling,
// then both are reconstructed to RGBA16 by one conversion: the error is what the codec changed,
// not what 4:2:0/4:2:2 sampling removes by design. A sample, matrix, range or siting that differs
// from what the encoder actually did still shows up as error. An RGB frame compares directly.
[[nodiscard]] ReopenMeasure measureReopen(const AVFrame& decoded,
                                          std::span<const std::byte> reference,
                                          const DeclaredSampling& declared);
} // namespace bloom::media::ffmpeg
