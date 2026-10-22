#include <bloom/media/audio/audio.hpp>

#include "mp3_backend.hpp"
#include "wav_backend.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <new>
#include <system_error>
#include <utility>

namespace {

using bloom::media::audio::AudioDecodeLimits;
using bloom::media::audio::AudioError;
using bloom::media::audio::AudioErrorCode;
using bloom::media::audio::AudioResult;

enum class AudioKind : std::uint8_t { Wav, Mp3 };

[[nodiscard]] bool hasBytes(const std::span<const std::byte> bytes,
                            const char (&text)[5]) noexcept {
    if (bytes.size() < 4) {
        return false;
    }
    for (std::size_t index = 0; index < 4; ++index) {
        if (std::to_integer<unsigned char>(bytes[index]) !=
            static_cast<unsigned char>(text[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isWav(const std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < 12 || (!hasBytes(bytes, "RIFF") && !hasBytes(bytes, "RF64"))) {
        return false;
    }
    return hasBytes(bytes.subspan(8), "WAVE");
}

[[nodiscard]] bool isMp3(const std::span<const std::byte> bytes) noexcept {
    if (bytes.size() >= 3 && std::to_integer<unsigned char>(bytes[0]) == 'I' &&
        std::to_integer<unsigned char>(bytes[1]) == 'D' &&
        std::to_integer<unsigned char>(bytes[2]) == '3') {
        return true;
    }
    if (bytes.size() < 2) {
        return false;
    }
    return std::to_integer<unsigned char>(bytes[0]) == 0xFFU &&
           (std::to_integer<unsigned char>(bytes[1]) & 0xE0U) == 0xE0U;
}

[[nodiscard]] AudioResult<AudioKind> readKind(const std::filesystem::path& path,
                                              const AudioDecodeLimits& limits) noexcept {
    if (path.empty())
        return AudioResult<AudioKind>::failure(AudioError::codeOnly(AudioErrorCode::InvalidPath));
    if (limits.fileSizeCap == 0 || limits.sampleBudget == 0)
        return AudioResult<AudioKind>::failure(
            AudioError::codeOnly(AudioErrorCode::InvalidAudioFormat));
    std::error_code status;
    const auto size = std::filesystem::file_size(path, status);
    if (status)
        return AudioResult<AudioKind>::failure(AudioError::codeOnly(AudioErrorCode::FileNotFound));
    if (size > limits.fileSizeCap)
        return AudioResult<AudioKind>::failure(
            AudioError::limited(AudioErrorCode::FileTooLarge, size, limits.fileSizeCap));
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return AudioResult<AudioKind>::failure(
                AudioError::codeOnly(AudioErrorCode::FileReadFailed));
        std::array<std::byte, 12> header{};
        input.read(reinterpret_cast<char*>(header.data()),
                   static_cast<std::streamsize>(header.size()));
        if (input.bad())
            return AudioResult<AudioKind>::failure(
                AudioError::codeOnly(AudioErrorCode::FileReadFailed));
        const auto bytes =
            std::span<const std::byte>(header.data(), static_cast<std::size_t>(input.gcount()));
        if (isWav(bytes))
            return AudioResult<AudioKind>::success(AudioKind::Wav);
        if (isMp3(bytes))
            return AudioResult<AudioKind>::success(AudioKind::Mp3);
        return AudioResult<AudioKind>::failure(AudioError::codeOnly(AudioErrorCode::InvalidMagic));
    } catch (const std::bad_alloc&) {
        return AudioResult<AudioKind>::failure(
            AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    }
}

} // namespace

namespace bloom::media::audio {

std::string describeAudioError(const AudioError& error) {
    switch (error.code) {
    case AudioErrorCode::InvalidPath:
        return "Audio path is empty";
    case AudioErrorCode::FileNotFound:
        return "Audio file is unavailable";
    case AudioErrorCode::FileTooLarge:
        return "Audio file is " + std::to_string(error.observed) +
               " bytes; configured encoded-file limit is " + std::to_string(error.limit) + " bytes";
    case AudioErrorCode::FileReadFailed:
        return "Audio file could not be read";
    case AudioErrorCode::InvalidMagic:
        return "File is not a supported WAV or MP3";
    case AudioErrorCode::UnsupportedContainer:
        return "Audio container is unsupported";
    case AudioErrorCode::MalformedFile:
        return "Audio file is malformed or truncated";
    case AudioErrorCode::DecoderFailure:
        return "Audio decoder failed";
    case AudioErrorCode::InvalidAudioFormat:
        return "Audio format or decode limits are invalid";
    case AudioErrorCode::SampleBudgetExceeded:
        return "Audio has " + std::to_string(error.observed) +
               " interleaved samples; whole-buffer decode limit is " + std::to_string(error.limit);
    case AudioErrorCode::InvalidBucketCount:
        return "Waveform bucket count is invalid";
    case AudioErrorCode::InvalidBuffer:
        return "Decoded audio buffer is invalid";
    case AudioErrorCode::AllocationFailure:
        return "Audio allocation failed";
    case AudioErrorCode::BackendUnavailable:
        return "Audio backend is unavailable";
    case AudioErrorCode::BackendStartFailed:
        return "Audio backend could not start";
    case AudioErrorCode::InvalidState:
        return "Audio backend state is invalid";
    }
    return "Audio error";
}

AudioResult<AudioProbe> probeAudio(const std::filesystem::path& path,
                                   const AudioDecodeLimits limits) noexcept {
    const auto kind = readKind(path, limits);
    if (!kind)
        return AudioResult<AudioProbe>::failure(*kind.error());
    return *kind.value() == AudioKind::Wav ? detail::probeWavFile(path)
                                           : detail::probeMp3File(path);
}

AudioResult<AudioBuffer> decodeAudio(const std::filesystem::path& path,
                                     const AudioDecodeLimits limits) noexcept {
    const auto kind = readKind(path, limits);
    if (!kind)
        return AudioResult<AudioBuffer>::failure(*kind.error());
    return *kind.value() == AudioKind::Wav ? detail::decodeWavFile(path, limits)
                                           : detail::decodeMp3File(path, limits);
}

AudioResult<WaveformSummary> waveformSummary(const AudioBuffer& buffer,
                                             const std::size_t bucketCount) noexcept {
    if (bucketCount == 0 || buffer.rate == 0 || buffer.channels == 0 || buffer.frames == 0 ||
        buffer.planes.size() != buffer.channels) {
        return AudioResult<WaveformSummary>::failure(AudioError::codeOnly(
            bucketCount == 0 ? AudioErrorCode::InvalidBucketCount : AudioErrorCode::InvalidBuffer));
    }
    if (buffer.frames > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return AudioResult<WaveformSummary>::failure(
            AudioError::codeOnly(AudioErrorCode::InvalidBuffer));
    }
    for (const auto& plane : buffer.planes) {
        if (plane.size() != static_cast<std::size_t>(buffer.frames)) {
            return AudioResult<WaveformSummary>::failure(
                AudioError::codeOnly(AudioErrorCode::InvalidBuffer));
        }
    }

    try {
        WaveformSummary summary;
        summary.channels = buffer.channels;
        summary.frames = buffer.frames;
        summary.buckets.resize(bucketCount,
                               std::vector<WaveformRange>(buffer.channels, WaveformRange{}));
        for (std::size_t bucket = 0; bucket < bucketCount; ++bucket) {
            const auto begin = (bucket * static_cast<std::size_t>(buffer.frames)) / bucketCount;
            const auto end =
                ((bucket + 1U) * static_cast<std::size_t>(buffer.frames)) / bucketCount;
            const auto first = std::min(begin, end - (end == begin ? 0U : 1U));
            const auto last = std::max(end, begin + 1U);
            for (std::size_t channel = 0; channel < buffer.channels; ++channel) {
                auto& range = summary.buckets[bucket][channel];
                range.minimum = buffer.planes[channel][first];
                range.maximum = range.minimum;
                for (std::size_t frame = first + 1U; frame < last; ++frame) {
                    range.minimum = std::min(range.minimum, buffer.planes[channel][frame]);
                    range.maximum = std::max(range.maximum, buffer.planes[channel][frame]);
                }
            }
        }
        return AudioResult<WaveformSummary>::success(std::move(summary));
    } catch (const std::bad_alloc&) {
        return AudioResult<WaveformSummary>::failure(
            AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    }
}

} // namespace bloom::media::audio
