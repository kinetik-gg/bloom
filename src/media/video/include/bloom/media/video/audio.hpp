#pragma once
#include <bloom/media/audio/audio.hpp>
#include <bloom/media/video/session.hpp>
namespace bloom::media::video {
// Adapter to the existing bounded clip mixer; no device or codec ownership crosses here.
[[nodiscard]] provider::Result<audio::AudioBuffer>
decodeAudioClip(VideoDecodeSession& session, const provider::ProbeResult& probe,
                const Cancel& cancel = {});
} // namespace bloom::media::video
