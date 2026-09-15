#include <bloom/media/audio/audio.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numbers>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef BLOOM_AUDIO_FIXTURE_DIR
#error "BLOOM_AUDIO_FIXTURE_DIR must name the checked-in audio fixtures"
#endif

namespace {

using bloom::core::RationalTime;
using bloom::media::audio::AudioContainer;
using bloom::media::audio::AudioDecodeLimits;
using bloom::media::audio::AudioErrorCode;
using bloom::media::audio::AudioProbe;
using bloom::media::audio::AudioResult;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

class TemporaryFile final {
  public:
    explicit TemporaryFile(const std::string_view suffix) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("bloom-audio-" + std::to_string(stamp) + std::string{suffix});
    }

    ~TemporaryFile() {
        std::error_code status;
        std::filesystem::remove(path_, status);
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

void writeU16(std::ofstream& output, const std::uint16_t value) {
    const std::array<std::byte, 2> bytes{
        std::byte{static_cast<unsigned char>(value & 0xFFU)},
        std::byte{static_cast<unsigned char>((value >> 8U) & 0xFFU)},
    };
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void writeU32(std::ofstream& output, const std::uint32_t value) {
    const std::array<std::byte, 4> bytes{
        std::byte{static_cast<unsigned char>(value & 0xFFU)},
        std::byte{static_cast<unsigned char>((value >> 8U) & 0xFFU)},
        std::byte{static_cast<unsigned char>((value >> 16U) & 0xFFU)},
        std::byte{static_cast<unsigned char>((value >> 24U) & 0xFFU)},
    };
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

struct GeneratedWav final {
    std::vector<float> left;
    std::vector<float> right;
};

GeneratedWav writeSineWav(const std::filesystem::path& path) {
    constexpr std::uint32_t rate = 48'000;
    constexpr std::uint32_t frames = rate;
    GeneratedWav samples;
    samples.left.reserve(frames);
    samples.right.reserve(frames);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const auto phase =
            2.0 * std::numbers::pi * 440.0 * static_cast<double>(frame) / static_cast<double>(rate);
        const auto value = static_cast<float>(std::sin(phase) * 0.5);
        samples.left.push_back(value);
        samples.right.push_back(-value);
    }

    const auto dataBytes = frames * 2U * static_cast<std::uint32_t>(sizeof(float));
    std::ofstream output(path, std::ios::binary);
    output.write("RIFF", 4);
    writeU32(output, 36U + dataBytes);
    output.write("WAVE", 4);
    output.write("fmt ", 4);
    writeU32(output, 16);
    writeU16(output, 3); // IEEE float
    writeU16(output, 2);
    writeU32(output, rate);
    writeU32(output, rate * 2U * static_cast<std::uint32_t>(sizeof(float)));
    writeU16(output, 2U * static_cast<std::uint16_t>(sizeof(float)));
    writeU16(output, 32);
    output.write("data", 4);
    writeU32(output, dataBytes);
    for (std::size_t frame = 0; frame < samples.left.size(); ++frame) {
        const auto left = std::bit_cast<std::uint32_t>(samples.left[frame]);
        const auto right = std::bit_cast<std::uint32_t>(samples.right[frame]);
        writeU32(output, left);
        writeU32(output, right);
    }
    output.close();
    return samples;
}

void testWavRoundTripAndWaveform(Expectations& expectations) {
    TemporaryFile file{".wav"};
    const auto expected = writeSineWav(file.path());
    const auto probe = bloom::media::audio::probeAudio(file.path());
    const auto expectedDuration = RationalTime::create(1, 1);
    expectations.expect(probe && probe.value()->container == AudioContainer::Wav &&
                            probe.value()->rate == 48'000 && probe.value()->channels == 2 &&
                            probe.value()->frames == 48'000 && expectedDuration.has_value() &&
                            probe.value()->duration == *expectedDuration,
                        "generated WAV probe reports exact stereo 48 kHz one-second metadata");

    const auto decoded = bloom::media::audio::decodeAudio(file.path());
    expectations.expect(decoded && decoded.value()->planes.size() == 2 &&
                            decoded.value()->frames == expected.left.size(),
                        "generated WAV decodes into two complete planar channels");
    if (decoded) {
        for (std::size_t frame = 0; frame < expected.left.size(); ++frame) {
            expectations.expect(
                std::bit_cast<std::uint32_t>(decoded.value()->planes[0][frame]) ==
                        std::bit_cast<std::uint32_t>(expected.left[frame]) &&
                    std::bit_cast<std::uint32_t>(decoded.value()->planes[1][frame]) ==
                        std::bit_cast<std::uint32_t>(expected.right[frame]),
                "WAV float samples round-trip bit-exactly");
            if (expectations.failures() != 0 && frame > 4) {
                break;
            }
        }

        const auto waveform = bloom::media::audio::waveformSummary(*decoded.value(), 8);
        expectations.expect(waveform && waveform.value()->buckets.size() == 8 &&
                                waveform.value()->buckets.front().size() == 2,
                            "waveform summary has one min/max pair per channel and bucket");
        if (waveform) {
            expectations.expect(waveform.value()->buckets[0][0].minimum < 0.0F &&
                                    waveform.value()->buckets[0][0].maximum > 0.0F &&
                                    waveform.value()->buckets[0][0].minimum <
                                        waveform.value()->buckets[0][0].maximum,
                                "waveform bucket captures the sine range");
        }
    }
}

void testMp3Fixture(Expectations& expectations) {
    const auto path = std::filesystem::path{BLOOM_AUDIO_FIXTURE_DIR} / "sine-stereo.mp3";
    const auto probe = bloom::media::audio::probeAudio(path);
    expectations.expect(probe && probe.value()->container == AudioContainer::Mp3 &&
                            probe.value()->rate == 48'000 && probe.value()->channels == 2 &&
                            probe.value()->frames > 0,
                        "checked-in MP3 fixture probes as a non-empty stereo 48 kHz stream");
    const auto decoded = bloom::media::audio::decodeAudio(path);
    expectations.expect(decoded && decoded.value()->frames > 0 &&
                            decoded.value()->planes.size() == 2,
                        "checked-in MP3 fixture decodes into planar samples");
}

void testHostileInputsAndLimits(Expectations& expectations) {
    TemporaryFile wav{".wav"};
    const auto expected = writeSineWav(wav.path());
    std::ifstream input(wav.path(), std::ios::binary);
    const std::vector<char> source((std::istreambuf_iterator<char>(input)), {});

    TemporaryFile truncated{".wav"};
    std::ofstream truncatedOutput(truncated.path(), std::ios::binary);
    truncatedOutput.write(source.data(), 24);
    truncatedOutput.close();
    const auto truncatedResult = bloom::media::audio::decodeAudio(truncated.path());
    expectations.expect(!truncatedResult &&
                            truncatedResult.error()->code == AudioErrorCode::MalformedFile,
                        "truncated WAV fails cleanly without a partial buffer");

    TemporaryFile wrongMagic{".bin"};
    std::ofstream wrongMagicOutput(wrongMagic.path(), std::ios::binary);
    wrongMagicOutput.write("not audio", 9);
    wrongMagicOutput.close();
    const auto wrongMagicResult = bloom::media::audio::probeAudio(wrongMagic.path());
    expectations.expect(!wrongMagicResult &&
                            wrongMagicResult.error()->code == AudioErrorCode::InvalidMagic,
                        "wrong magic fails before any decoder call");

    AudioDecodeLimits sampleLimit;
    sampleLimit.sampleBudget = expected.left.size();
    const auto budgetResult = bloom::media::audio::decodeAudio(wav.path(), sampleLimit);
    expectations.expect(!budgetResult &&
                            budgetResult.error()->code == AudioErrorCode::SampleBudgetExceeded,
                        "sample budget rejects an interleaved sample count above the limit");

    AudioDecodeLimits fileLimit;
    fileLimit.fileSizeCap = 16;
    const auto fileResult = bloom::media::audio::decodeAudio(wav.path(), fileLimit);
    expectations.expect(!fileResult && fileResult.error()->code == AudioErrorCode::FileTooLarge,
                        "file-size cap rejects input before it is read");
}

} // namespace

int main() {
    Expectations expectations;
    testWavRoundTripAndWaveform(expectations);
    testMp3Fixture(expectations);
    testHostileInputsAndLimits(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
