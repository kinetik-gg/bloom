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
using bloom::media::audio::CancelAudioWork;

enum class AudioKind : std::uint8_t { Wav, Mp3 };

[[nodiscard]] bool cancelled(const CancelAudioWork& cancel) noexcept {
    if (!cancel)
        return false;
    try {
        return cancel();
    } catch (...) {
        return true;
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

[[nodiscard]] AudioResult<AudioKind> readKind(const std::filesystem::path& path,
                                              const AudioDecodeLimits& limits, const bool decoding,
                                              const CancelAudioWork& cancel) noexcept {
    if (cancelled(cancel))
        return AudioResult<AudioKind>::failure(AudioError::codeOnly(AudioErrorCode::Cancelled));
    if (path.empty())
        return AudioResult<AudioKind>::failure(AudioError::codeOnly(AudioErrorCode::InvalidPath));
    if (limits.fileSizeCap == 0 || (decoding && limits.sampleBudget == 0))
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
        if (cancelled(cancel))
            return AudioResult<AudioKind>::failure(AudioError::codeOnly(AudioErrorCode::Cancelled));
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

AudioStream::AudioStream(AudioProbe probe, const std::size_t windowFrames)
    : probe_(probe), window_(windowFrames * probe.channels), windowFrames_(windowFrames) {}

std::uint64_t AudioStream::retainedBytes() const noexcept {
    // Includes decoder state, its bounded callback I/O buffer, file/path objects and control
    // blocks. The allowance is held for the source's entire lifetime, independent of duration.
    return sizeof(*this) + static_cast<std::uint64_t>(window_.capacity()) * sizeof(float) +
           256U * 1024U;
}

std::optional<AudioError> AudioStream::diagnostic() const noexcept {
    const auto code = diagnosticCode_.load(std::memory_order_acquire);
    return code < 0
               ? std::nullopt
               : std::optional<AudioError>{AudioError::codeOnly(static_cast<AudioErrorCode>(code))};
}

bool AudioStream::ensureWindow(const std::uint64_t frame, const CancelAudioWork& cancel) noexcept {
    if (diagnostic_ && diagnostic_->code == AudioErrorCode::Cancelled)
        diagnostic_.reset();
    if (!diagnostic_)
        diagnosticCode_.store(-1, std::memory_order_release);
    if (diagnostic_)
        return false;
    if (frame >= windowStart_ && frame - windowStart_ < windowCount_)
        return true;
    const auto count =
        static_cast<std::size_t>(std::min<std::uint64_t>(windowFrames_, probe_.frames - frame));
    auto result = fill(frame, std::span<float>(window_.data(), count * probe_.channels), cancel);
    if (!result || *result.value() != count) {
        diagnostic_ =
            result ? AudioError::codeOnly(AudioErrorCode::DecoderFailure) : *result.error();
        if (diagnostic_->code != AudioErrorCode::Cancelled)
            diagnosticCode_.store(static_cast<int>(diagnostic_->code), std::memory_order_release);
        return false;
    }
    windowStart_ = frame;
    windowCount_ = count;
    return true;
}

AudioResult<std::size_t> AudioStream::readInterleaved(const std::uint64_t firstFrame,
                                                      const std::span<float> output,
                                                      const CancelAudioWork& cancel) noexcept {
    if (probe_.channels == 0 || output.size() % probe_.channels != 0 || firstFrame > probe_.frames)
        return AudioResult<std::size_t>::failure(
            AudioError::codeOnly(AudioErrorCode::InvalidAudioFormat));
    std::scoped_lock lock(mutex_);
    const auto count =
        std::min<std::uint64_t>(output.size() / probe_.channels, probe_.frames - firstFrame);
    std::size_t done = 0;
    while (done < count) {
        if (cancelled(cancel))
            return AudioResult<std::size_t>::failure(
                AudioError::codeOnly(AudioErrorCode::Cancelled));
        const auto frame = firstFrame + done;
        if (!ensureWindow(frame, cancel))
            return AudioResult<std::size_t>::failure(*diagnostic_);
        const auto offset = static_cast<std::size_t>(frame - windowStart_);
        const auto available = std::min<std::size_t>(count - done, windowCount_ - offset);
        std::copy_n(window_.data() + offset * probe_.channels, available * probe_.channels,
                    output.data() + done * probe_.channels);
        done += available;
    }
    return AudioResult<std::size_t>::success(done);
}

std::optional<float> AudioStream::sample(const std::uint64_t frame, const std::uint32_t channel,
                                         const CancelAudioWork& cancel) noexcept {
    if (frame >= probe_.frames || channel >= probe_.channels)
        return std::nullopt;
    std::scoped_lock lock(mutex_);
    if (!ensureWindow(frame, cancel))
        return std::nullopt;
    return window_[(frame - windowStart_) * probe_.channels + channel];
}

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
    case AudioErrorCode::Cancelled:
        return "Audio operation was cancelled";
    }
    return "Audio error";
}

AudioResult<AudioProbe> probeAudio(const std::filesystem::path& path,
                                   const AudioDecodeLimits limits,
                                   const CancelAudioWork& cancel) noexcept {
    const auto kind = readKind(path, limits, false, cancel);
    if (!kind)
        return AudioResult<AudioProbe>::failure(*kind.error());
    return *kind.value() == AudioKind::Wav ? detail::probeWavFile(path, cancel)
                                           : detail::probeMp3File(path, cancel);
}

AudioResult<AudioBuffer> decodeAudio(const std::filesystem::path& path,
                                     const AudioDecodeLimits limits,
                                     const CancelAudioWork& cancel) noexcept {
    const auto kind = readKind(path, limits, true, cancel);
    if (!kind)
        return AudioResult<AudioBuffer>::failure(*kind.error());
    return *kind.value() == AudioKind::Wav ? detail::decodeWavFile(path, limits, cancel)
                                           : detail::decodeMp3File(path, limits, cancel);
}

AudioResult<AudioBuffer> openWindowedAudio(const std::filesystem::path& path,
                                           const AudioDecodeLimits limits,
                                           const std::size_t windowFrames,
                                           const CancelAudioWork& cancel) noexcept {
    if (windowFrames == 0 || limits.sampleBudget == 0)
        return AudioResult<AudioBuffer>::failure(
            AudioError::codeOnly(AudioErrorCode::InvalidAudioFormat));
    const auto kind = readKind(path, limits, false, cancel);
    if (!kind)
        return AudioResult<AudioBuffer>::failure(*kind.error());
    const auto probe = probeAudio(path, limits, cancel);
    if (!probe)
        return AudioResult<AudioBuffer>::failure(*probe.error());
    if (probe.value()->channels == 0 ||
        windowFrames > std::numeric_limits<std::size_t>::max() / probe.value()->channels ||
        windowFrames > limits.sampleBudget / probe.value()->channels)
        return AudioResult<AudioBuffer>::failure(
            AudioError::limited(AudioErrorCode::SampleBudgetExceeded,
                                static_cast<std::uint64_t>(windowFrames) * probe.value()->channels,
                                limits.sampleBudget));
    return *kind.value() == AudioKind::Wav
               ? detail::openWindowedWavFile(path, windowFrames, cancel)
               : detail::openWindowedMp3File(path, windowFrames, cancel);
}

AudioResult<WaveformSummary> waveformSummary(AudioStream& stream, const std::size_t bucketCount,
                                             const CancelAudioWork& cancel) noexcept {
    const auto& probe = stream.probe();
    if (bucketCount == 0 || probe.frames == 0 || probe.channels == 0)
        return AudioResult<WaveformSummary>::failure(AudioError::codeOnly(
            bucketCount == 0 ? AudioErrorCode::InvalidBucketCount : AudioErrorCode::InvalidBuffer));
    try {
        WaveformSummary summary;
        summary.channels = probe.channels;
        summary.frames = probe.frames;
        summary.buckets.resize(bucketCount, std::vector<WaveformRange>(probe.channels));
        std::vector<bool> seen(bucketCount, false);
        std::vector<float> chunk(AudioStream::kWindowFrames * probe.channels);
        const auto bucketStart = [frames = probe.frames, bucketCount](const std::size_t bucket) {
            const auto quotient = frames / bucketCount;
            const auto remainder = frames % bucketCount;
            return quotient * bucket + (remainder * bucket) / bucketCount;
        };
        std::size_t bucket = 0;
        for (std::uint64_t start = 0; start < probe.frames;) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(AudioStream::kWindowFrames, probe.frames - start));
            const auto result = stream.readInterleaved(
                start, std::span<float>(chunk.data(), count * probe.channels), cancel);
            if (!result)
                return AudioResult<WaveformSummary>::failure(*result.error());
            for (std::size_t frame = 0; frame < count; ++frame) {
                while (bucket + 1 < bucketCount && start + frame >= bucketStart(bucket + 1))
                    ++bucket;
                auto& ranges = summary.buckets[bucket];
                for (std::size_t channel = 0; channel < probe.channels; ++channel) {
                    const auto sample = chunk[frame * probe.channels + channel];
                    auto& range = ranges[channel];
                    if (!seen[bucket])
                        range = {sample, sample};
                    else {
                        range.minimum = std::min(range.minimum, sample);
                        range.maximum = std::max(range.maximum, sample);
                    }
                }
                seen[bucket] = true;
            }
            start += count;
        }
        // A bucket can be empty when the source is shorter than the requested summary.
        for (std::size_t emptyBucket = 0; emptyBucket < bucketCount; ++emptyBucket)
            if (!seen[emptyBucket]) {
                const auto frame =
                    std::min<std::uint64_t>(probe.frames - 1, bucketStart(emptyBucket));
                for (std::size_t channel = 0; channel < probe.channels; ++channel) {
                    const auto sample = stream.sample(frame, static_cast<std::uint32_t>(channel));
                    if (!sample)
                        return AudioResult<WaveformSummary>::failure(
                            AudioError::codeOnly(AudioErrorCode::DecoderFailure));
                    summary.buckets[emptyBucket][channel] = {*sample, *sample};
                }
            }
        return AudioResult<WaveformSummary>::success(std::move(summary));
    } catch (const std::bad_alloc&) {
        return AudioResult<WaveformSummary>::failure(
            AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    }
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
