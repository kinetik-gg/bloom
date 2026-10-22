#pragma once

#include <bloom/core/rational_time.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::media::audio {

enum class AudioContainer : std::uint8_t {
    Wav,
    Mp3,
};

enum class AudioErrorCode : std::uint8_t {
    InvalidPath,
    FileNotFound,
    FileTooLarge,
    FileReadFailed,
    InvalidMagic,
    UnsupportedContainer,
    MalformedFile,
    DecoderFailure,
    InvalidAudioFormat,
    SampleBudgetExceeded,
    InvalidBucketCount,
    InvalidBuffer,
    AllocationFailure,
    BackendUnavailable,
    BackendStartFailed,
    InvalidState,
    Cancelled,
};

struct AudioError final {
    AudioErrorCode code;
    std::uint64_t observed = 0;
    std::uint64_t limit = 0;

    [[nodiscard]] static constexpr AudioError codeOnly(const AudioErrorCode value) noexcept {
        return AudioError{value, 0, 0};
    }
    [[nodiscard]] static constexpr AudioError limited(const AudioErrorCode value,
                                                      const std::uint64_t actual,
                                                      const std::uint64_t maximum) noexcept {
        return AudioError{value, actual, maximum};
    }

    friend constexpr bool operator==(const AudioError&, const AudioError&) noexcept = default;
};
[[nodiscard]] std::string describeAudioError(const AudioError& error);

template <typename T> class [[nodiscard]] AudioResult final {
  public:
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "AudioResult values must be nothrow move constructible");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "AudioResult values must be nothrow destructible");

    [[nodiscard]] static AudioResult success(T value) noexcept {
        return AudioResult(std::move(value));
    }
    [[nodiscard]] static AudioResult failure(const AudioError error) noexcept {
        return AudioResult(error);
    }

    [[nodiscard]] bool hasValue() const noexcept { return std::holds_alternative<T>(storage_); }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] T* value() & noexcept { return std::get_if<T>(&storage_); }
    [[nodiscard]] const T* value() const& noexcept { return std::get_if<T>(&storage_); }
    [[nodiscard]] T* value() && = delete;
    [[nodiscard]] const T* value() const&& = delete;
    [[nodiscard]] AudioError* error() & noexcept { return std::get_if<AudioError>(&storage_); }
    [[nodiscard]] const AudioError* error() const& noexcept {
        return std::get_if<AudioError>(&storage_);
    }
    [[nodiscard]] AudioError* error() && = delete;
    [[nodiscard]] const AudioError* error() const&& = delete;

  private:
    explicit AudioResult(T&& value) noexcept : storage_(std::in_place_index<0>, std::move(value)) {}
    explicit AudioResult(const AudioError error) noexcept
        : storage_(std::in_place_index<1>, error) {}

    std::variant<T, AudioError> storage_;
};

struct AudioDecodeLimits final {
    // Streaming decoders need no encoded-file cap. Callers may still set one explicitly.
    static constexpr std::uint64_t kDefaultFileSizeCap = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t fileSizeCap = kDefaultFileSizeCap;
    // A decoder caller owns this admission. Zero refuses decode; probe needs no sample storage.
    std::uint64_t sampleBudget = 0;
};
using CancelAudioWork = std::function<bool()>;

struct AudioProbe final {
    AudioContainer container = AudioContainer::Wav;
    std::uint32_t rate = 0;
    std::uint32_t channels = 0;
    std::uint64_t frames = 0;
    core::RationalTime duration{};
};

// Owns one persistent decoder and a bounded interleaved PCM window. Calls may perform file I/O
// and must be made only on a worker (never on the audio device callback or UI thread).
class AudioStream {
  public:
    static constexpr std::size_t kWindowFrames = 4096;
    AudioStream(AudioProbe probe, std::size_t windowFrames);
    virtual ~AudioStream() = default;
    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;

    [[nodiscard]] const AudioProbe& probe() const noexcept { return probe_; }
    [[nodiscard]] std::uint64_t retainedBytes() const noexcept;
    [[nodiscard]] std::optional<AudioError> diagnostic() const noexcept;
    [[nodiscard]] AudioResult<std::size_t>
    readInterleaved(std::uint64_t firstFrame, std::span<float> output,
                    const CancelAudioWork& cancel = {}) noexcept;
    [[nodiscard]] std::optional<float> sample(std::uint64_t frame, std::uint32_t channel,
                                              const CancelAudioWork& cancel = {}) noexcept;

  protected:
    [[nodiscard]] virtual AudioResult<std::size_t> fill(std::uint64_t firstFrame,
                                                        std::span<float> output,
                                                        const CancelAudioWork& cancel) noexcept = 0;

  private:
    [[nodiscard]] bool ensureWindow(std::uint64_t frame, const CancelAudioWork& cancel) noexcept;
    AudioProbe probe_;
    std::vector<float> window_;
    std::size_t windowFrames_ = 0;
    std::uint64_t windowStart_ = 0;
    std::size_t windowCount_ = 0;
    std::optional<AudioError> diagnostic_;
    std::atomic<int> diagnosticCode_{-1};
    mutable std::mutex mutex_;
};

struct AudioBuffer final {
    std::uint32_t rate = 0;
    std::uint32_t channels = 0;
    std::uint64_t frames = 0;
    std::vector<std::vector<float>> planes;
    // When present, frames is the full source duration while planes stays empty.
    std::shared_ptr<AudioStream> stream;
};

struct WaveformRange final {
    float minimum = 0.0F;
    float maximum = 0.0F;

    friend constexpr bool operator==(const WaveformRange&, const WaveformRange&) noexcept = default;
};

struct WaveformSummary final {
    std::uint32_t channels = 0;
    std::uint64_t frames = 0;
    // Outer index is the bucket; inner index is the channel.
    std::vector<std::vector<WaveformRange>> buckets;
};

[[nodiscard]] AudioResult<AudioProbe> probeAudio(const std::filesystem::path& path,
                                                 AudioDecodeLimits limits = AudioDecodeLimits{},
                                                 const CancelAudioWork& cancel = {}) noexcept;

[[nodiscard]] AudioResult<AudioBuffer> decodeAudio(const std::filesystem::path& path,
                                                   AudioDecodeLimits limits,
                                                   const CancelAudioWork& cancel = {}) noexcept;

[[nodiscard]] AudioResult<AudioBuffer>
openWindowedAudio(const std::filesystem::path& path, AudioDecodeLimits limits,
                  std::size_t windowFrames = AudioStream::kWindowFrames,
                  const CancelAudioWork& cancel = {}) noexcept;

[[nodiscard]] AudioResult<WaveformSummary>
waveformSummary(AudioStream& stream, std::size_t bucketCount,
                const CancelAudioWork& cancel = {}) noexcept;

[[nodiscard]] AudioResult<WaveformSummary> waveformSummary(const AudioBuffer& buffer,
                                                           std::size_t bucketCount) noexcept;

static_assert(std::is_nothrow_move_constructible_v<AudioBuffer>);
static_assert(std::is_nothrow_move_constructible_v<WaveformSummary>);

} // namespace bloom::media::audio
