#pragma once

#include <bloom/core/rational_time.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
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
    static constexpr std::uint64_t kDefaultFileSizeCap =
        std::uint64_t{64} * std::uint64_t{1024} * std::uint64_t{1024};
    static constexpr std::uint64_t kDefaultSampleBudget = 48'000'000U;

    std::uint64_t fileSizeCap = kDefaultFileSizeCap;
    std::uint64_t sampleBudget = kDefaultSampleBudget;
};

struct AudioProbe final {
    AudioContainer container = AudioContainer::Wav;
    std::uint32_t rate = 0;
    std::uint32_t channels = 0;
    std::uint64_t frames = 0;
    core::RationalTime duration{};
};

struct AudioBuffer final {
    std::uint32_t rate = 0;
    std::uint32_t channels = 0;
    std::uint64_t frames = 0;
    std::vector<std::vector<float>> planes;
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

[[nodiscard]] AudioResult<AudioProbe>
probeAudio(const std::filesystem::path& path,
           AudioDecodeLimits limits = AudioDecodeLimits{}) noexcept;

[[nodiscard]] AudioResult<AudioBuffer>
decodeAudio(const std::filesystem::path& path,
            AudioDecodeLimits limits = AudioDecodeLimits{}) noexcept;

[[nodiscard]] AudioResult<WaveformSummary> waveformSummary(const AudioBuffer& buffer,
                                                           std::size_t bucketCount) noexcept;

static_assert(std::is_nothrow_move_constructible_v<AudioBuffer>);
static_assert(std::is_nothrow_move_constructible_v<WaveformSummary>);

} // namespace bloom::media::audio
