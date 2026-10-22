#define DR_WAV_NO_STDIO
#define DR_WAV_NO_WCHAR
#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

#include "wav_backend.hpp"

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
using bloom::media::audio::CancelAudioWork;

struct FileReader {
    std::ifstream& input;
    const CancelAudioWork& cancel;
    bool cancelled = false;
};

bool cancelled(FileReader& reader) noexcept {
    if (reader.cancelled || !reader.cancel)
        return reader.cancelled;
    try {
        reader.cancelled = reader.cancel();
    } catch (...) {
        reader.cancelled = true;
    }
    return reader.cancelled;
}

[[nodiscard]] RationalTime duration(const std::uint64_t frames, const std::uint32_t rate) noexcept {
    const auto result = RationalTime::create(static_cast<std::int64_t>(frames), rate);
    return result.value_or(RationalTime{});
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

size_t readFile(void* user, void* buffer, size_t bytes) {
    auto& reader = *static_cast<FileReader*>(user);
    if (cancelled(reader))
        return 0;
    auto& stream = reader.input;
    const auto count = static_cast<std::streamsize>(
        std::min(bytes, static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())));
    stream.read(static_cast<char*>(buffer), count);
    return static_cast<size_t>(stream.gcount());
}

drwav_bool32 seekFile(void* user, int offset, drwav_seek_origin origin) {
    auto& reader = *static_cast<FileReader*>(user);
    if (cancelled(reader))
        return DRWAV_FALSE;
    auto& stream = reader.input;
    stream.clear();
    stream.seekg(offset, origin == DRWAV_SEEK_SET ? std::ios::beg : std::ios::cur);
    return stream.good() ? DRWAV_TRUE : DRWAV_FALSE;
}

[[nodiscard]] bool openFile(FileReader& reader, drwav& wav) noexcept {
    return drwav_init(&wav, readFile, seekFile, nullptr, &reader, nullptr) != DRWAV_FALSE;
}

[[nodiscard]] AudioResult<AudioBuffer> decodeOpened(drwav& wav, const AudioDecodeLimits& limits,
                                                    FileReader& reader) noexcept {
    if (wav.channels == 0 || wav.sampleRate == 0 || wav.totalPCMFrameCount == 0 ||
        wav.totalPCMFrameCount >
            static_cast<drwav_uint64>(std::numeric_limits<std::size_t>::max()) ||
        wav.totalPCMFrameCount >
            static_cast<drwav_uint64>(std::numeric_limits<std::int64_t>::max()))
        return AudioResult<AudioBuffer>::failure(
            AudioError::codeOnly(AudioErrorCode::InvalidAudioFormat));
    if (wav.totalPCMFrameCount > limits.sampleBudget / wav.channels) {
        const auto observed =
            wav.totalPCMFrameCount > std::numeric_limits<std::uint64_t>::max() / wav.channels
                ? std::numeric_limits<std::uint64_t>::max()
                : wav.totalPCMFrameCount * wav.channels;
        return AudioResult<AudioBuffer>::failure(AudioError::limited(
            AudioErrorCode::SampleBudgetExceeded, observed, limits.sampleBudget));
    }
    try {
        const auto frames = static_cast<std::size_t>(wav.totalPCMFrameCount);
        const auto channels = static_cast<std::size_t>(wav.channels);
        if (frames > std::numeric_limits<std::size_t>::max() / channels)
            return AudioResult<AudioBuffer>::failure(AudioError::limited(
                AudioErrorCode::SampleBudgetExceeded, std::numeric_limits<std::uint64_t>::max(),
                std::numeric_limits<std::size_t>::max()));
        AudioBuffer buffer;
        buffer.rate = wav.sampleRate;
        buffer.channels = wav.channels;
        buffer.frames = wav.totalPCMFrameCount;
        buffer.planes.resize(channels);
        for (auto& plane : buffer.planes)
            plane.resize(frames);
        constexpr std::size_t kFramesPerChunk = 4096;
        auto interleaved = std::vector<float>(std::min(kFramesPerChunk, frames) * channels);
        for (std::size_t start = 0; start < frames;) {
            if (cancelled(reader))
                return AudioResult<AudioBuffer>::failure(
                    AudioError::codeOnly(AudioErrorCode::Cancelled));
            const auto count = std::min(kFramesPerChunk, frames - start);
            if (drwav_read_pcm_frames_f32(&wav, count, interleaved.data()) != count)
                return AudioResult<AudioBuffer>::failure(AudioError::codeOnly(
                    reader.cancelled ? AudioErrorCode::Cancelled : AudioErrorCode::MalformedFile));
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

AudioResult<AudioProbe> probeWavFile(const std::filesystem::path& path,
                                     const CancelAudioWork& cancel) noexcept {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return AudioResult<AudioProbe>::failure(
                AudioError::codeOnly(AudioErrorCode::FileReadFailed));
        drwav wav{};
        FileReader reader{input, cancel};
        if (!openFile(reader, wav))
            return AudioResult<AudioProbe>::failure(AudioError::codeOnly(
                reader.cancelled ? AudioErrorCode::Cancelled : AudioErrorCode::MalformedFile));
        auto result = probeOpened(wav);
        drwav_uninit(&wav);
        if (reader.cancelled)
            return AudioResult<AudioProbe>::failure(
                AudioError::codeOnly(AudioErrorCode::Cancelled));
        return result;
    } catch (const std::bad_alloc&) {
        return AudioResult<AudioProbe>::failure(
            AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    }
}

AudioResult<AudioBuffer> decodeWavFile(const std::filesystem::path& path,
                                       const AudioDecodeLimits& limits,
                                       const CancelAudioWork& cancel) noexcept {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return AudioResult<AudioBuffer>::failure(
                AudioError::codeOnly(AudioErrorCode::FileReadFailed));
        drwav wav{};
        FileReader reader{input, cancel};
        if (!openFile(reader, wav))
            return AudioResult<AudioBuffer>::failure(AudioError::codeOnly(
                reader.cancelled ? AudioErrorCode::Cancelled : AudioErrorCode::MalformedFile));
        auto result = decodeOpened(wav, limits, reader);
        drwav_uninit(&wav);
        if (reader.cancelled)
            return AudioResult<AudioBuffer>::failure(
                AudioError::codeOnly(AudioErrorCode::Cancelled));
        return result;
    } catch (const std::bad_alloc&) {
        return AudioResult<AudioBuffer>::failure(
            AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    }
}

} // namespace bloom::media::audio::detail
