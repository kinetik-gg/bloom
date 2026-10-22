#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_NO_STDIO
#define MINIMP3_IMPLEMENTATION
#include <minimp3_ex.h>

#include "mp3_backend.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <system_error>
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
    const CancelAudioWork* cancel = nullptr;
    bool cancelled = false;
};

bool cancelled(FileReader& reader) noexcept {
    if (reader.cancelled || !reader.cancel || !*reader.cancel)
        return reader.cancelled;
    try {
        reader.cancelled = (*reader.cancel)();
    } catch (...) {
        reader.cancelled = true;
    }
    return reader.cancelled;
}

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
    auto& reader = *static_cast<FileReader*>(user);
    if (cancelled(reader))
        return 0;
    auto& stream = reader.input;
    const auto count = static_cast<std::streamsize>(
        std::min(bytes, static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())));
    stream.read(static_cast<char*>(buffer), count);
    return static_cast<size_t>(stream.gcount());
}

int seekFile(std::uint64_t position, void* user) {
    if (position > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
        return -1;
    auto& reader = *static_cast<FileReader*>(user);
    if (cancelled(reader))
        return -1;
    auto& stream = reader.input;
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(position), std::ios::beg);
    return stream.good() ? 0 : -1;
}

[[nodiscard]] mp3dec_io_t fileIo(FileReader& reader) noexcept {
    mp3dec_io_t io{};
    io.read = readFile;
    io.read_data = &reader;
    io.seek = seekFile;
    io.seek_data = &reader;
    return io;
}

[[nodiscard]] AudioResult<AudioBuffer>
decodeOpened(mp3dec_ex_t& decoder, const AudioDecodeLimits& limits, FileReader& reader) noexcept {
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
            if (cancelled(reader))
                return AudioResult<AudioBuffer>::failure(
                    AudioError::codeOnly(AudioErrorCode::Cancelled));
            const auto count = std::min(kFramesPerChunk, frames - start);
            const auto chunkSamples = count * channels;
            if (mp3dec_ex_read(&decoder, interleaved.data(), chunkSamples) != chunkSamples)
                return AudioResult<AudioBuffer>::failure(AudioError::codeOnly(
                    reader.cancelled ? AudioErrorCode::Cancelled : AudioErrorCode::DecoderFailure));
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

class Mp3Stream final : public bloom::media::audio::AudioStream {
  public:
    Mp3Stream(const AudioProbe probe, const std::size_t windowFrames,
              const std::filesystem::path& path, const std::uintmax_t size,
              const std::filesystem::file_time_type modified)
        : AudioStream(probe, windowFrames), path_(path), size_(size), modified_(modified),
          reader_{input_, {}} {}

    ~Mp3Stream() override { mp3dec_ex_close(&decoder_); }
    [[nodiscard]] int open(const CancelAudioWork& cancel) {
        input_.open(path_, std::ios::binary);
        if (!input_)
            return MP3D_E_IOERROR;
        reader_.cancel = &cancel;
        io_ = fileIo(reader_);
        const auto status =
            mp3dec_ex_open_cb(&decoder_, &io_, MP3D_SEEK_TO_SAMPLE | MP3D_DO_NOT_SCAN);
        reader_.cancel = nullptr;
        return status;
    }
    [[nodiscard]] bool wasCancelled() const noexcept { return reader_.cancelled; }

  protected:
    [[nodiscard]] AudioResult<std::size_t> fill(const std::uint64_t firstFrame,
                                                const std::span<float> output,
                                                const CancelAudioWork& cancel) noexcept override {
        std::error_code error;
        if (std::filesystem::file_size(path_, error) != size_ || error ||
            std::filesystem::last_write_time(path_, error) != modified_ || error)
            return AudioResult<std::size_t>::failure(
                AudioError::codeOnly(AudioErrorCode::FileReadFailed));
        reader_.cancel = &cancel;
        reader_.cancelled = false;
        const auto firstSample = firstFrame * probe().channels;
        bool seeked = true;
        if (firstSample < decoder_.cur_sample) {
            seeked = mp3dec_ex_seek(&decoder_, 0) == 0;
            if (seeked)
                decoder_.to_skip = decoder_.start_delay;
        }
        std::array<float, 4096> discard{};
        while (seeked && decoder_.cur_sample < firstSample && !cancelled(reader_)) {
            const auto remaining = firstSample - decoder_.cur_sample;
            const auto wanted =
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, discard.size()));
            seeked = mp3dec_ex_read(&decoder_, discard.data(), wanted) == wanted;
        }
        const auto samples = seeked ? mp3dec_ex_read(&decoder_, output.data(), output.size()) : 0;
        reader_.cancel = nullptr;
        if (reader_.cancelled)
            return AudioResult<std::size_t>::failure(
                AudioError::codeOnly(AudioErrorCode::Cancelled));
        if (samples != output.size())
            return AudioResult<std::size_t>::failure(
                AudioError::codeOnly(AudioErrorCode::DecoderFailure));
        return AudioResult<std::size_t>::success(samples / probe().channels);
    }

  private:
    std::filesystem::path path_;
    std::uintmax_t size_;
    std::filesystem::file_time_type modified_;
    std::ifstream input_;
    FileReader reader_;
    mp3dec_io_t io_{};
    mp3dec_ex_t decoder_{};
};

} // namespace

namespace bloom::media::audio::detail {

AudioResult<AudioProbe> probeMp3File(const std::filesystem::path& path,
                                     const CancelAudioWork& cancel) noexcept {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return AudioResult<AudioProbe>::failure(
                AudioError::codeOnly(AudioErrorCode::FileReadFailed));
        FileReader reader{input, &cancel};
        auto io = fileIo(reader);
        mp3dec_ex_t decoder{};
        const auto status =
            mp3dec_ex_open_cb(&decoder, &io, MP3D_SEEK_TO_SAMPLE | MP3D_DO_NOT_SCAN);
        if (status != 0) {
            mp3dec_ex_close(&decoder);
            return AudioResult<AudioProbe>::failure(
                AudioError::codeOnly(reader.cancelled          ? AudioErrorCode::Cancelled
                                     : status == MP3D_E_MEMORY ? AudioErrorCode::AllocationFailure
                                                               : AudioErrorCode::MalformedFile));
        }
        AudioResult<AudioProbe> result = AudioResult<AudioProbe>::failure(
            AudioError::codeOnly(AudioErrorCode::InvalidAudioFormat));
        if (decoder.info.channels > 0 && decoder.info.hz > 0) {
            std::array<float, 4096> chunk{};
            std::uint64_t samples = 0;
            while (!reader.cancelled) {
                const auto read = mp3dec_ex_read(&decoder, chunk.data(), chunk.size());
                if (read == 0)
                    break;
                if (samples > std::numeric_limits<std::uint64_t>::max() - read)
                    break;
                samples += read;
            }
            const auto channels = static_cast<std::uint32_t>(decoder.info.channels);
            const auto frames = samples / channels;
            if (!reader.cancelled && decoder.last_error == 0 && samples > 0 &&
                samples % channels == 0 &&
                frames <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                result = AudioResult<AudioProbe>::success(
                    AudioProbe{AudioContainer::Mp3, static_cast<std::uint32_t>(decoder.info.hz),
                               channels, frames, duration(frames, decoder.info.hz)});
        }
        mp3dec_ex_close(&decoder);
        if (reader.cancelled)
            return AudioResult<AudioProbe>::failure(
                AudioError::codeOnly(AudioErrorCode::Cancelled));
        return result;
    } catch (const std::bad_alloc&) {
        return AudioResult<AudioProbe>::failure(
            AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    }
}

AudioResult<AudioBuffer> decodeMp3File(const std::filesystem::path& path,
                                       const AudioDecodeLimits& limits,
                                       const CancelAudioWork& cancel) noexcept {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return AudioResult<AudioBuffer>::failure(
                AudioError::codeOnly(AudioErrorCode::FileReadFailed));
        FileReader reader{input, &cancel};
        auto io = fileIo(reader);
        mp3dec_ex_t decoder{};
        const auto status = mp3dec_ex_open_cb(&decoder, &io, 0);
        if (status != 0) {
            mp3dec_ex_close(&decoder);
            return AudioResult<AudioBuffer>::failure(
                AudioError::codeOnly(reader.cancelled          ? AudioErrorCode::Cancelled
                                     : status == MP3D_E_MEMORY ? AudioErrorCode::AllocationFailure
                                                               : AudioErrorCode::MalformedFile));
        }
        auto result = decodeOpened(decoder, limits, reader);
        mp3dec_ex_close(&decoder);
        if (reader.cancelled)
            return AudioResult<AudioBuffer>::failure(
                AudioError::codeOnly(AudioErrorCode::Cancelled));
        return result;
    } catch (const std::bad_alloc&) {
        return AudioResult<AudioBuffer>::failure(
            AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    }
}

AudioResult<AudioBuffer> openWindowedMp3File(const std::filesystem::path& path,
                                             const std::size_t windowFrames,
                                             const CancelAudioWork& cancel) noexcept {
    try {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error)
            return AudioResult<AudioBuffer>::failure(
                AudioError::codeOnly(AudioErrorCode::FileReadFailed));
        const auto modified = std::filesystem::last_write_time(path, error);
        if (error)
            return AudioResult<AudioBuffer>::failure(
                AudioError::codeOnly(AudioErrorCode::FileReadFailed));
        const auto probe = probeMp3File(path, cancel);
        if (!probe)
            return AudioResult<AudioBuffer>::failure(*probe.error());
        auto stream =
            std::make_shared<Mp3Stream>(*probe.value(), windowFrames, path, size, modified);
        const auto status = stream->open(cancel);
        if (status != 0)
            return AudioResult<AudioBuffer>::failure(
                AudioError::codeOnly(stream->wasCancelled()    ? AudioErrorCode::Cancelled
                                     : status == MP3D_E_MEMORY ? AudioErrorCode::AllocationFailure
                                                               : AudioErrorCode::MalformedFile));
        if (stream->probe().frames == 0)
            return AudioResult<AudioBuffer>::failure(
                AudioError::codeOnly(AudioErrorCode::InvalidAudioFormat));
        return AudioResult<AudioBuffer>::success(AudioBuffer{probe.value()->rate,
                                                             probe.value()->channels,
                                                             probe.value()->frames,
                                                             {},
                                                             std::move(stream)});
    } catch (const std::bad_alloc&) {
        return AudioResult<AudioBuffer>::failure(
            AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    }
}

} // namespace bloom::media::audio::detail
