#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_IMPLEMENTATION
#include <minimp3_ex.h>

#include "mp3_backend.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
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

size_t readFile(void* buffer, size_t bytes, void* user) {
    auto& stream = *static_cast<std::ifstream*>(user);
    const auto count = static_cast<std::streamsize>(
        std::min(bytes, static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())));
    stream.read(static_cast<char*>(buffer), count);
    return static_cast<size_t>(stream.gcount());
}

int seekFile(std::uint64_t position, void* user) {
    if (position > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
        return -1;
    auto& stream = *static_cast<std::ifstream*>(user);
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(position), std::ios::beg);
    return stream.good() ? 0 : -1;
}

[[nodiscard]] mp3dec_io_t fileIo(std::ifstream& input) noexcept {
    mp3dec_io_t io{};
    io.read = readFile;
    io.read_data = &input;
    io.seek = seekFile;
    io.seek_data = &input;
    return io;
}

[[nodiscard]] AudioResult<AudioBuffer> decodeOpened(mp3dec_ex_t& decoder,
                                                    const AudioDecodeLimits& limits) noexcept {
    if (decoder.info.channels <= 0 || decoder.info.hz <= 0 || decoder.samples == 0 ||
        decoder.samples > std::numeric_limits<std::size_t>::max())
        return AudioResult<AudioBuffer>::failure(
            AudioError::codeOnly(AudioErrorCode::InvalidAudioFormat));
    if (decoder.samples > limits.sampleBudget)
        return AudioResult<AudioBuffer>::failure(AudioError::limited(
            AudioErrorCode::SampleBudgetExceeded, decoder.samples, limits.sampleBudget));
    const auto channels = static_cast<std::size_t>(decoder.info.channels);
    const auto samples = static_cast<std::size_t>(decoder.samples);
    const auto frames = samples / channels;
    if (frames == 0 || decoder.samples % channels != 0 ||
        frames > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        return AudioResult<AudioBuffer>::failure(
            AudioError::codeOnly(AudioErrorCode::InvalidAudioFormat));
    try {
        AudioBuffer buffer;
        buffer.rate = static_cast<std::uint32_t>(decoder.info.hz);
        buffer.channels = static_cast<std::uint32_t>(decoder.info.channels);
        buffer.frames = frames;
        buffer.planes.resize(channels);
        for (auto& plane : buffer.planes)
            plane.resize(frames);
        constexpr std::size_t kFramesPerChunk = 4096;
        auto interleaved = std::vector<float>(std::min(kFramesPerChunk, frames) * channels);
        for (std::size_t start = 0; start < frames;) {
            const auto count = std::min(kFramesPerChunk, frames - start);
            const auto chunkSamples = count * channels;
            if (mp3dec_ex_read(&decoder, interleaved.data(), chunkSamples) != chunkSamples)
                return AudioResult<AudioBuffer>::failure(
                    AudioError::codeOnly(AudioErrorCode::DecoderFailure));
            for (std::size_t frame = 0; frame < count; ++frame)
                for (std::size_t channel = 0; channel < channels; ++channel)
                    buffer.planes[channel][start + frame] = interleaved[frame * channels + channel];
            start += count;
        }
        return AudioResult<AudioBuffer>::success(std::move(buffer));
    } catch (const std::bad_alloc&) {
        return AudioResult<AudioBuffer>::failure(
            AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    }
}

} // namespace

namespace bloom::media::audio::detail {

AudioResult<AudioProbe> probeMp3File(const std::filesystem::path& path) noexcept {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return AudioResult<AudioProbe>::failure(
                AudioError::codeOnly(AudioErrorCode::FileReadFailed));
        auto io = fileIo(input);
        mp3dec_ex_t decoder{};
        const auto status = mp3dec_ex_open_cb(&decoder, &io, 0);
        if (status != 0) {
            mp3dec_ex_close(&decoder);
            return AudioResult<AudioProbe>::failure(
                AudioError::codeOnly(status == MP3D_E_MEMORY ? AudioErrorCode::AllocationFailure
                                                             : AudioErrorCode::MalformedFile));
        }
        auto result = probeOpened(decoder);
        mp3dec_ex_close(&decoder);
        return result;
    } catch (const std::bad_alloc&) {
        return AudioResult<AudioProbe>::failure(
            AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    }
}

AudioResult<AudioBuffer> decodeMp3File(const std::filesystem::path& path,
                                       const AudioDecodeLimits& limits) noexcept {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return AudioResult<AudioBuffer>::failure(
                AudioError::codeOnly(AudioErrorCode::FileReadFailed));
        auto io = fileIo(input);
        mp3dec_ex_t decoder{};
        const auto status = mp3dec_ex_open_cb(&decoder, &io, 0);
        if (status != 0) {
            mp3dec_ex_close(&decoder);
            return AudioResult<AudioBuffer>::failure(
                AudioError::codeOnly(status == MP3D_E_MEMORY ? AudioErrorCode::AllocationFailure
                                                             : AudioErrorCode::MalformedFile));
        }
        auto result = decodeOpened(decoder, limits);
        mp3dec_ex_close(&decoder);
        return result;
    } catch (const std::bad_alloc&) {
        return AudioResult<AudioBuffer>::failure(
            AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    }
}

} // namespace bloom::media::audio::detail
