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
    std::uint64_t byteLimit = Limits::sourceBytes;
    friend bool operator==(const EncodeSettingsV1&, const EncodeSettingsV1&) = default;
};
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
};
inline constexpr std::uint32_t kEncodeChunkBytes = 1024U * 1024U;
inline constexpr std::string_view kVideoToleranceV1 =
    "bloom.video-rgba16-srgb.v1;first-last;absolute-max=22938;mean-ceil=1967;"
    "alpha-max=64;timing=exact;stream-layout=exact;pcm=exact";
inline constexpr std::string_view kAacToleranceV1 =
    "bloom.aac-f32.v1;sample-count=exact;priming=trim;absolute-max=0.25;rms=0.05";
[[nodiscard]] bool valid(const EncodeSettingsV1& value);
[[nodiscard]] MediaDeterminismV1 encodeDeterminism(const EncodeSettingsV1& value);
[[nodiscard]] Digest encodeTolerance(const EncodeSettingsV1& value);
[[nodiscard]] Result<Digest> digest(const EncodeSettingsV1& value);
} // namespace bloom::media::provider
