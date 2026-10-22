#pragma once

#include <bloom/media/audio/audio.hpp>

#include <cstddef>

namespace bloom::media::audio::detail {

[[nodiscard]] AudioResult<AudioProbe> probeWavFile(const std::filesystem::path& path,
                                                   const CancelAudioWork& cancel) noexcept;

[[nodiscard]] AudioResult<AudioBuffer> decodeWavFile(const std::filesystem::path& path,
                                                     const AudioDecodeLimits& limits,
                                                     const CancelAudioWork& cancel) noexcept;

} // namespace bloom::media::audio::detail
