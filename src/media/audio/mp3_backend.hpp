#pragma once

#include <bloom/media/audio/audio.hpp>

#include <cstddef>

namespace bloom::media::audio::detail {

[[nodiscard]] AudioResult<AudioProbe> probeMp3File(const std::filesystem::path& path) noexcept;

[[nodiscard]] AudioResult<AudioBuffer> decodeMp3File(const std::filesystem::path& path,
                                                     const AudioDecodeLimits& limits) noexcept;

} // namespace bloom::media::audio::detail
