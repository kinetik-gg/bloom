#include <bloom/media/audio/audio.hpp>

#include "mp3_backend.hpp"
#include "wav_backend.hpp"

#include <algorithm>
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

[[nodiscard]] AudioResult<std::vector<std::byte>>
readFile(const std::filesystem::path& path, const AudioDecodeLimits& limits) noexcept {
    if (path.empty()) {
        return AudioResult<std::vector<std::byte>>::failure(
            AudioError::codeOnly(AudioErrorCode::InvalidPath));
    }

    std::error_code status;
    const auto size = std::filesystem::file_size(path, status);
    if (status) {
        return AudioResult<std::vector<std::byte>>::failure(
            AudioError::codeOnly(AudioErrorCode::FileNotFound));
    }
    if (size > limits.fileSizeCap) {
        return AudioResult<std::vector<std::byte>>::failure(
            AudioError::limited(AudioErrorCode::FileTooLarge, size, limits.fileSizeCap));
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return AudioResult<std::vector<std::byte>>::failure(
            AudioError::codeOnly(AudioErrorCode::FileTooLarge));
    }

    try {
        auto bytes = std::vector<std::byte>(static_cast<std::size_t>(size));
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return AudioResult<std::vector<std::byte>>::failure(
                AudioError::codeOnly(AudioErrorCode::FileReadFailed));
        }
        if (!bytes.empty()) {
            input.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
                return AudioResult<std::vector<std::byte>>::failure(
                    AudioError::codeOnly(AudioErrorCode::FileReadFailed));
            }
        }
        return AudioResult<std::vector<std::byte>>::success(std::move(bytes));
    } catch (const std::bad_alloc&) {
        return AudioResult<std::vector<std::byte>>::failure(
            AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    }
}

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

template <typename ProbeOrBuffer>
[[nodiscard]] AudioResult<ProbeOrBuffer> readAndDispatch(const std::filesystem::path& path,
                                                         const AudioDecodeLimits& limits,
                                                         const bool decode) noexcept {
    if (limits.fileSizeCap == 0 || limits.sampleBudget == 0) {
        return AudioResult<ProbeOrBuffer>::failure(
            AudioError::codeOnly(AudioErrorCode::InvalidAudioFormat));
    }

    auto input = readFile(path, limits);
    if (!input) {
        return AudioResult<ProbeOrBuffer>::failure(*input.error());
    }
    const auto bytes = std::span<const std::byte>{input.value()->data(), input.value()->size()};
    if (isWav(bytes)) {
        if constexpr (std::is_same_v<ProbeOrBuffer, bloom::media::audio::AudioProbe>) {
            return bloom::media::audio::detail::probeWav(bytes);
        } else if (decode) {
            return bloom::media::audio::detail::decodeWav(bytes, limits);
        }
    }
    if (isMp3(bytes)) {
        if constexpr (std::is_same_v<ProbeOrBuffer, bloom::media::audio::AudioProbe>) {
            return bloom::media::audio::detail::probeMp3(bytes);
        } else if (decode) {
            return bloom::media::audio::detail::decodeMp3(bytes, limits);
        }
    }
    return AudioResult<ProbeOrBuffer>::failure(AudioError::codeOnly(AudioErrorCode::InvalidMagic));
}

} // namespace

namespace bloom::media::audio {

AudioResult<AudioProbe> probeAudio(const std::filesystem::path& path,
                                   const AudioDecodeLimits limits) noexcept {
    return readAndDispatch<AudioProbe>(path, limits, false);
}

AudioResult<AudioBuffer> decodeAudio(const std::filesystem::path& path,
                                     const AudioDecodeLimits limits) noexcept {
    return readAndDispatch<AudioBuffer>(path, limits, true);
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
