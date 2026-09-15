#pragma once

#include <bloom/media/audio/audio.hpp>

#include <cstddef>
#include <span>

namespace bloom::media::audio::detail {

[[nodiscard]] AudioResult<AudioProbe> probeWav(std::span<const std::byte> bytes) noexcept;

[[nodiscard]] AudioResult<AudioBuffer> decodeWav(std::span<const std::byte> bytes,
                                                 const AudioDecodeLimits& limits) noexcept;

} // namespace bloom::media::audio::detail
