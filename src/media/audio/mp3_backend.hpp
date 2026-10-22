#pragma once

#include <bloom/media/audio/audio.hpp>

#include <cstddef>

namespace bloom::media::audio::detail {

[[nodiscard]] AudioResult<AudioProbe> probeMp3File(const std::filesystem::path& path,
                                                   const CancelAudioWork& cancel) noexcept;

[[nodiscard]] AudioResult<AudioBuffer> decodeMp3File(const std::filesystem::path& path,
                                                     const AudioDecodeLimits& limits,
                                                     const CancelAudioWork& cancel) noexcept;

[[nodiscard]] AudioResult<AudioBuffer> openWindowedMp3File(const std::filesystem::path& path,
                                                           std::size_t windowFrames,
                                                           const CancelAudioWork& cancel) noexcept;

} // namespace bloom::media::audio::detail
