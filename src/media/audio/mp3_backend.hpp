#pragma once

#include <bloom/media/audio/audio.hpp>

#include <cstddef>
#include <span>

namespace bloom::media::audio::detail {

[[nodiscard]] AudioResult<AudioProbe> probeMp3(std::span<const std::byte> bytes) noexcept;

[[nodiscard]] AudioResult<AudioBuffer> decodeMp3(std::span<const std::byte> bytes,
                                                 const AudioDecodeLimits& limits) noexcept;

} // namespace bloom::media::audio::detail
