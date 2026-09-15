#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_IMPLEMENTATION
#include "../third_party/minimp3/minimp3_ex.h"

#include "mp3_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

namespace {

using bloom::core::RationalTime;
using bloom::media::audio::AudioBuffer;
using bloom::media::audio::AudioContainer;
using bloom::media::audio::AudioDecodeLimits;
using bloom::media::audio::AudioError;
using bloom::media::audio::AudioErrorCode;
using bloom::media::audio::AudioProbe;
using bloom::media::audio::AudioResult;

[[nodiscard]] RationalTime duration(const std::uint64_t frames, const int rate) noexcept {
    const auto result = RationalTime::create(static_cast<std::int64_t>(frames), rate);
    return result.value_or(RationalTime{});
}

[[nodiscard]] AudioResult<mp3dec_ex_t> open(std::span<const std::byte> bytes) noexcept {
    mp3dec_ex_t decoder{};
    const auto result = mp3dec_ex_open_buf(
        &decoder, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), 0);
    if (result != 0) {
        return AudioResult<mp3dec_ex_t>::failure(
            AudioError::codeOnly(result == MP3D_E_MEMORY ? AudioErrorCode::AllocationFailure
                                                         : AudioErrorCode::MalformedFile));
    }
    return AudioResult<mp3dec_ex_t>::success(decoder);
}

[[nodiscard]] AudioResult<AudioProbe> probeOpened(mp3dec_ex_t& decoder) noexcept {
    if (decoder.info.channels <= 0 || decoder.info.hz <= 0 || decoder.samples == 0 ||
        decoder.samples / static_cast<std::uint64_t>(decoder.info.channels) >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return AudioResult<AudioProbe>::failure(
            AudioError::codeOnly(AudioErrorCode::InvalidAudioFormat));
    }
    const auto frames = decoder.samples / static_cast<std::uint64_t>(decoder.info.channels);
    return AudioResult<AudioProbe>::success(AudioProbe{
        AudioContainer::Mp3,
        static_cast<std::uint32_t>(decoder.info.hz),
        static_cast<std::uint32_t>(decoder.info.channels),
        frames,
        duration(frames, decoder.info.hz),
    });
}

} // namespace

namespace bloom::media::audio::detail {

AudioResult<AudioProbe> probeMp3(const std::span<const std::byte> bytes) noexcept {
    auto opened = open(bytes);
    if (!opened) {
        return AudioResult<AudioProbe>::failure(*opened.error());
    }
    auto result = probeOpened(*opened.value());
    mp3dec_ex_close(opened.value());
    return result;
}

AudioResult<AudioBuffer> decodeMp3(const std::span<const std::byte> bytes,
                                   const AudioDecodeLimits& limits) noexcept {
    auto opened = open(bytes);
    if (!opened) {
        return AudioResult<AudioBuffer>::failure(*opened.error());
    }
    auto& decoder = *opened.value();
    const auto finish = [&decoder](AudioResult<AudioBuffer> result) {
        mp3dec_ex_close(&decoder);
        return result;
    };
    if (decoder.info.channels <= 0 || decoder.info.hz <= 0 || decoder.samples == 0) {
        return finish(AudioResult<AudioBuffer>::failure(
            AudioError::codeOnly(AudioErrorCode::InvalidAudioFormat)));
    }
    if (decoder.samples > limits.sampleBudget) {
        return finish(AudioResult<AudioBuffer>::failure(AudioError::limited(
            AudioErrorCode::SampleBudgetExceeded, decoder.samples, limits.sampleBudget)));
    }
    const auto channels = static_cast<std::size_t>(decoder.info.channels);
    const auto samples = static_cast<std::size_t>(decoder.samples);
    const auto frames = samples / channels;
    if (frames == 0 || decoder.samples % channels != 0 ||
        frames > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        return finish(AudioResult<AudioBuffer>::failure(
            AudioError::codeOnly(AudioErrorCode::InvalidAudioFormat)));
    }

    try {
        auto interleaved = std::vector<float>(samples);
        const auto readSamples = mp3dec_ex_read(&decoder, interleaved.data(), samples);
        if (readSamples != samples) {
            return finish(AudioResult<AudioBuffer>::failure(
                AudioError::codeOnly(AudioErrorCode::DecoderFailure)));
        }
        AudioBuffer buffer;
        buffer.rate = static_cast<std::uint32_t>(decoder.info.hz);
        buffer.channels = static_cast<std::uint32_t>(decoder.info.channels);
        buffer.frames = frames;
        buffer.planes.resize(channels);
        for (auto& plane : buffer.planes) {
            plane.resize(frames);
        }
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                buffer.planes[channel][frame] = interleaved[frame * channels + channel];
            }
        }
        return finish(AudioResult<AudioBuffer>::success(std::move(buffer)));
    } catch (const std::bad_alloc&) {
        return finish(AudioResult<AudioBuffer>::failure(
            AudioError::codeOnly(AudioErrorCode::AllocationFailure)));
    }
}

} // namespace bloom::media::audio::detail
