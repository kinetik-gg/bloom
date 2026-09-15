#define DR_WAV_NO_STDIO
#define DR_WAV_NO_WCHAR
#define DR_WAV_IMPLEMENTATION
#include "../third_party/dr_wav/dr_wav.h"

#include "wav_backend.hpp"

#include <algorithm>
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

[[nodiscard]] RationalTime duration(const std::uint64_t frames, const std::uint32_t rate) noexcept {
    const auto result = RationalTime::create(static_cast<std::int64_t>(frames), rate);
    return result.value_or(RationalTime{});
}

[[nodiscard]] AudioResult<drwav> open(std::span<const std::byte> bytes) noexcept {
    drwav wav{};
    if (bytes.empty() || !drwav_init_memory(&wav, bytes.data(), bytes.size(), nullptr)) {
        return AudioResult<drwav>::failure(AudioError::codeOnly(AudioErrorCode::MalformedFile));
    }
    auto result = AudioResult<drwav>::success(wav);
    // dr_wav's memory callbacks keep the owning decoder in pUserData. Repair that self-pointer
    // after the value moves into the Bloom result wrapper.
    result.value()->pUserData = result.value();
    return result;
}

[[nodiscard]] AudioResult<AudioProbe> probeOpened(drwav& wav) noexcept {
    if (wav.channels == 0 || wav.sampleRate == 0 ||
        wav.totalPCMFrameCount >
            static_cast<drwav_uint64>(std::numeric_limits<std::int64_t>::max())) {
        return AudioResult<AudioProbe>::failure(
            AudioError::codeOnly(AudioErrorCode::InvalidAudioFormat));
    }
    return AudioResult<AudioProbe>::success(AudioProbe{
        AudioContainer::Wav,
        wav.sampleRate,
        wav.channels,
        wav.totalPCMFrameCount,
        duration(wav.totalPCMFrameCount, wav.sampleRate),
    });
}

} // namespace

namespace bloom::media::audio::detail {

AudioResult<AudioProbe> probeWav(const std::span<const std::byte> bytes) noexcept {
    auto opened = open(bytes);
    if (!opened) {
        return AudioResult<AudioProbe>::failure(*opened.error());
    }
    auto result = probeOpened(*opened.value());
    drwav_uninit(opened.value());
    return result;
}

AudioResult<AudioBuffer> decodeWav(const std::span<const std::byte> bytes,
                                   const AudioDecodeLimits& limits) noexcept {
    auto opened = open(bytes);
    if (!opened) {
        return AudioResult<AudioBuffer>::failure(*opened.error());
    }
    auto& wav = *opened.value();
    const auto finish = [&wav](AudioResult<AudioBuffer> result) {
        drwav_uninit(&wav);
        return result;
    };
    if (wav.channels == 0 || wav.sampleRate == 0 ||
        wav.totalPCMFrameCount >
            static_cast<drwav_uint64>(std::numeric_limits<std::size_t>::max()) ||
        wav.totalPCMFrameCount >
            static_cast<drwav_uint64>(std::numeric_limits<std::int64_t>::max())) {
        return finish(AudioResult<AudioBuffer>::failure(
            AudioError::codeOnly(AudioErrorCode::InvalidAudioFormat)));
    }
    if (wav.totalPCMFrameCount > limits.sampleBudget / wav.channels) {
        return finish(AudioResult<AudioBuffer>::failure(
            AudioError::limited(AudioErrorCode::SampleBudgetExceeded,
                                wav.totalPCMFrameCount * static_cast<std::uint64_t>(wav.channels),
                                limits.sampleBudget)));
    }

    try {
        const auto frames = static_cast<std::size_t>(wav.totalPCMFrameCount);
        const auto channels = static_cast<std::size_t>(wav.channels);
        auto interleaved = std::vector<float>(frames * channels);
        const auto readFrames =
            drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, interleaved.data());
        if (readFrames != wav.totalPCMFrameCount) {
            return finish(AudioResult<AudioBuffer>::failure(
                AudioError::codeOnly(AudioErrorCode::MalformedFile)));
        }
        AudioBuffer buffer;
        buffer.rate = wav.sampleRate;
        buffer.channels = wav.channels;
        buffer.frames = wav.totalPCMFrameCount;
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
