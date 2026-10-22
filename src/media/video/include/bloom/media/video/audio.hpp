#pragma once
#include <bloom/media/audio/audio.hpp>
#include <bloom/media/video/session.hpp>
#include <cstdint>
#include <optional>
namespace bloom::media::video {
// Adapter to the existing bounded clip mixer; no device or codec ownership crosses here.
[[nodiscard]] std::optional<std::uint64_t>
audioClipFrameCount(const provider::StreamDescriptor& stream) noexcept;
[[nodiscard]] provider::Result<audio::AudioBuffer>
decodeAudioClip(VideoDecodeSession& session, const provider::ProbeResult& probe,
                std::uint64_t admittedSamples, const Cancel& cancel = {});
} // namespace bloom::media::video
