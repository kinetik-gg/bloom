#pragma once
#include <bloom/media/provider/contract.hpp>

namespace bloom::media::provider {
// Closed CPU export intake. RGB is straight, display-referred RGBA16LE sRGB/Rec.709.
// Audio blocks are planar finite float at the declared source rate; timestamps start at zero.
struct EncodeSettingsV1 {
    std::string container = "mov", videoCodec = "prores_ks", profile = "hq";
    std::string audioCodec;
    std::uint32_t width = 1, height = 1;
    Rational rate{24, 1};
    std::uint64_t frames = 1, audioSamples = 0;
    std::uint32_t sampleRate = 48000, channels = 2;
    std::string bwfDescription;
    // Issue #369: the destination's free space minus a safety margin, bounded by the container.
    std::uint64_t byteLimit = Limits::exportBytes;
    // Container colour tags written for the output colour intent (H.273 code points; the range
    // uses FFmpeg's convention, 1 limited and 2 full). The matrix also selects the RGB -> YUV
    // coefficients. The default is the legacy BT.709 limited-range tagging.
    ColourTags colour{1, 1, 1, 1};
    // Target video bit rate for H.264/HEVC; zero means the codec default.
    std::uint32_t videoBitrateKbps = 0;
    // Encoder threads; zero means automatic. Threading never changes decoded semantics beyond the
    // tolerance profile; lossy codecs already claim DecodedSemanticTolerance.
    std::uint32_t threads = 1;
    // VA-API render node for hardware encode ("/dev/dri/renderD128" when empty).
    std::string vaapiDevice;
    friend bool operator==(const EncodeSettingsV1&, const EncodeSettingsV1&) = default;
};
// Issue #369: each container's own audio channel limit, replacing the fixed 8-channel cap.
[[nodiscard]] std::uint32_t containerChannelLimit(std::string_view container,
                                                  std::string_view audioCodec) noexcept;
struct EncodedChunkV1 {
    std::uint64_t offset = 0;
    Bytes bytes;
};
struct EncodeQcV1 {
    std::uint64_t bytes = 0, frames = 0, audioSamples = 0;
    Rational duration;
    Digest artifact, firstFrame, lastFrame, audio;
    // Fixed point: maximum and mean absolute sample error in unsigned 16-bit units.
    std::uint32_t maximumError = 0, meanError = 0;
    // Issue #369: read back from the reopened artifact.
    ColourTags colour;
    std::string container;
    // The video profile and bit depth the reopened stream actually carries ("main", 8), and,
    // when the serving encoder delivered something other than the request (the bundled 8-bit
    // Kvazaar serving a Main 10 request), the reason. Never silent: the host logs it and the
    // export evidence states it.
    std::string profile;
    std::uint32_t bitDepth = 0;
    std::string adjustment;
};
inline constexpr std::uint32_t kEncodeChunkBytes = 1024U * 1024U;
// v2 compares against the reference taken through the stream's declared sample format, matrix,
// range and chroma siting (v1 used the unsampled reference, failing 4:2:2/4:2:0 decimation of
// small saturated detail as if it were codec error). The bounds are unchanged.
inline constexpr std::string_view kVideoToleranceV2 =
    "bloom.video-rgba16-srgb.v2;first-last;reference=declared-sampling;absolute-max=22938;"
    "mean-ceil=1967;alpha-max=64;timing=exact;stream-layout=exact;pcm=exact";
inline constexpr std::string_view kAacToleranceV1 =
    "bloom.aac-f32.v1;sample-count=exact;priming=trim;absolute-max=0.25;rms=0.05";
[[nodiscard]] bool valid(const EncodeSettingsV1& value);
[[nodiscard]] MediaDeterminismV1 encodeDeterminism(const EncodeSettingsV1& value);
[[nodiscard]] Digest encodeTolerance(const EncodeSettingsV1& value);
[[nodiscard]] Result<Digest> digest(const EncodeSettingsV1& value);
} // namespace bloom::media::provider
