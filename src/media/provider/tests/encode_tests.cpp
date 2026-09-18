#include "support.hpp"
#include <bloom/media/provider/encode_session.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <unistd.h>
#include <utility>
using namespace bloom::media::provider;
namespace {
void ok(const std::optional<Unavailable>& e) {
    if (e)
        throw std::runtime_error(e->detail);
}
template <typename T> T get(Result<T> result) {
    if (const auto* e = std::get_if<Unavailable>(&result))
        throw std::runtime_error(e->detail);
    return std::get<T>(std::move(result));
}
FrameProduct image(const EncodeSettingsV1& settings, std::uint64_t index) {
    FrameProduct frame;
    frame.format = PixelFormat::Rgba16;
    const auto divisor = std::gcd(index * static_cast<std::uint64_t>(settings.rate.denominator),
                                  static_cast<std::uint64_t>(settings.rate.numerator));
    frame.pts = {static_cast<std::int64_t>(
                     index * static_cast<std::uint64_t>(settings.rate.denominator) / divisor),
                 settings.rate.numerator / static_cast<std::int64_t>(divisor)};
    CpuPlane plane;
    plane.width = settings.width;
    plane.height = settings.height;
    plane.stride = plane.width * 8;
    plane.bytes.resize(static_cast<std::size_t>(plane.stride) * plane.height);
    for (std::uint32_t y = 0; y < plane.height; ++y)
        for (std::uint32_t x = 0; x < plane.width; ++x)
            for (unsigned c = 0; c < 4; ++c) {
                const auto value = c == 3 ? (settings.profile.starts_with("4444")
                                                 ? (x < settings.width / 2 ? 20000U : 50000U)
                                                 : 65535U)
                                          : (x + index < settings.width / 2 ? 20000U : 40000U);
                const auto offset = static_cast<std::size_t>(y) * plane.stride +
                                    static_cast<std::size_t>(x) * 8U +
                                    static_cast<std::size_t>(c) * 2U;
                plane.bytes[offset] = static_cast<std::byte>(value & 255U);
                plane.bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
            }
    plane.digest = digestBytes(plane.bytes);
    frame.planes.push_back(std::move(plane));
    return frame;
}
AudioBlock sound(const EncodeSettingsV1& settings, std::uint64_t offset, std::uint32_t count) {
    AudioBlock block;
    const auto divisor = std::gcd(offset, static_cast<std::uint64_t>(settings.sampleRate));
    block.pts = {static_cast<std::int64_t>(offset / divisor),
                 static_cast<std::int64_t>(settings.sampleRate / divisor)};
    block.sampleRate = settings.sampleRate;
    block.channelLayout = {"FL", "FR"};
    block.channels.resize(2, std::vector<float>(count));
    for (unsigned i = 0; i < count; ++i)
        for (unsigned c = 0; c < 2; ++c)
            block.channels[c][i] =
                settings.audioCodec == "aac"
                    ? static_cast<float>(0.2 * std::sin(static_cast<double>(offset + i + c) * 0.02))
                    : static_cast<float>(static_cast<int>((offset + i + c) % 997) - 498) / 1024.0F;
    return block;
}
Bytes run(const EncodeSettingsV1& settings, const std::filesystem::path& destination,
          EncodeSessionOptionsV1 options = {}) {
    EncodeSessionV1 session(std::move(options));
    ok(session.begin(settings));
    for (std::uint64_t i = 0; i < settings.frames; ++i) {
        ok(session.video(image(settings, i)));
        if (!settings.audioCodec.empty())
            ok(session.audio(sound(settings, i * 2000, 2000)));
    }
    if (settings.frames == 0)
        for (std::uint64_t offset = 0; offset < settings.audioSamples; offset += 2000)
            ok(session.audio(sound(settings, offset,
                                   static_cast<std::uint32_t>(std::min<std::uint64_t>(
                                       2000, settings.audioSamples - offset)))));
    const auto qc = get(session.finish());
    test::check(qc.frames == settings.frames && qc.audioSamples == settings.audioSamples,
                "reopen counts exact");
    const auto durationNumerator =
        settings.frames ? settings.frames * static_cast<std::uint64_t>(settings.rate.denominator)
                        : settings.audioSamples;
    const auto durationDenominator =
        settings.frames ? static_cast<std::uint64_t>(settings.rate.numerator) : settings.sampleRate;
    const auto divisor = std::gcd(durationNumerator, durationDenominator);
    test::check(qc.duration == Rational{static_cast<std::int64_t>(durationNumerator / divisor),
                                        static_cast<std::int64_t>(durationDenominator / divisor)},
                "reopen duration exact");
    Bytes artifact;
    for (std::uint64_t offset = 0; offset < qc.bytes;) {
        auto chunk = get(session.read(offset));
        offset += chunk.bytes.size();
        artifact.insert(artifact.end(), chunk.bytes.begin(), chunk.bytes.end());
    }
    test::check(digestBytes(artifact) == qc.artifact, "transferred artifact digest");
    ok(session.close());
    std::ofstream file(destination, std::ios::binary);
    file.write(reinterpret_cast<const char*>(artifact.data()),
               static_cast<std::streamsize>(artifact.size()));
    const auto toleranceHex = encodeTolerance(settings).toLowercaseHex();
    std::cout << destination.filename() << " frames=" << qc.frames << " samples=" << qc.audioSamples
              << " max=" << qc.maximumError << " mean=" << qc.meanError
              << " tolerance=" << std::string(toleranceHex.data(), toleranceHex.size()) << '\n';
    return artifact;
}
void tests(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    EncodeSettingsV1 settings;
    settings.width = 256;
    settings.height = 128;
    settings.frames = 48;
    settings.audioCodec = "pcm_s16le";
    settings.audioSamples = 96000;
    (void)run(settings, directory / "export-prores.mov");
    settings.videoCodec = "dnxhd";
    settings.profile = "dnxhr_hq";
    settings.container = "mxf";
    (void)run(settings, directory / "export-dnxhr.mxf");
    settings.videoCodec.clear();
    settings.profile.clear();
    settings.container = "wav";
    settings.frames = 0;
    const auto wav = run(settings, directory / "export.wav");
    // Independent RIFF/PCM reference, including the muxer's reserved RF64 JUNK chunk.
    Bytes oracle;
    const auto text = [&](std::string_view value) {
        for (const char c : value)
            oracle.push_back(static_cast<std::byte>(c));
    };
    const auto number = [&](std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i)
            oracle.push_back(static_cast<std::byte>((value >> (i * 8U)) & 255U));
    };
    text("RIFF");
    number(384072, 4);
    text("WAVEJUNK");
    number(28, 4);
    oracle.resize(48);
    text("fmt ");
    number(16, 4);
    number(1, 2);
    number(2, 2);
    number(48000, 4);
    number(192000, 4);
    number(4, 2);
    number(16, 2);
    text("data");
    number(384000, 4);
    for (std::uint64_t i = 0; i < 96000; ++i)
        for (unsigned c = 0; c < 2; ++c)
            number(static_cast<std::uint16_t>((static_cast<int>((i + c) % 997) - 498) * 32), 2);
    test::check(wav == oracle, "WAV matches independent RIFF/PCM encoder byte for byte");
    test::check(wav == run(settings, directory / "export-repeat.wav"), "WAV whole-file byte exact");
    settings.bwfDescription = "Bloom test reference";
    const auto bwf = run(settings, directory / "export-bwf.wav");
    const std::string bytes(reinterpret_cast<const char*>(bwf.data()), bwf.size());
    test::check(bytes.find("bext") != std::string::npos &&
                    bytes.find(settings.bwfDescription) != std::string::npos,
                "BWF description chunk");
    settings.bwfDescription.clear();
    settings.audioCodec = "pcm_s24le";
    (void)run(settings, directory / "export24.wav");
    settings.audioCodec = "aac";
    settings.container = "mov";
    (void)run(settings, directory / "export-aac.mov");
    EncodeSessionV1 excessive({});
    ok(excessive.begin(settings));
    for (std::uint64_t offset = 0; offset < settings.audioSamples; offset += 2000) {
        auto block = sound(settings, offset, 2000);
        for (auto& channel : block.channels)
            for (auto& sample : channel)
                sample *= 2;
        ok(excessive.audio(std::move(block)));
    }
    const auto rejectedAac = excessive.finish();
    test::check(std::holds_alternative<Unavailable>(rejectedAac),
                "AAC outside immutable tolerance fails closed");
    settings = {};
    settings.width = 256;
    settings.height = 128;
    for (const auto* profile : {"proxy", "lt", "422", "hq", "4444", "4444xq"}) {
        settings.profile = profile;
        (void)run(settings, directory / (std::string(profile) + ".mov"));
    }
    settings.videoCodec = "dnxhd";
    settings.container = "mxf";
    for (const auto* profile : {"dnxhr_lb", "dnxhr_sq", "dnxhr_hq", "dnxhr_hqx", "dnxhr_444"}) {
        settings.profile = profile;
        (void)run(settings, directory / (std::string(profile) + ".mxf"));
    }
    settings.profile = "dnxhd";
    settings.width = 1920;
    settings.height = 1080;
    settings.rate = {25, 1};
    (void)run(settings, directory / "export-dnxhd.mxf");
    settings = {};
    settings.width = 256;
    settings.height = 128;
    settings.frames = 50;
    settings.rate = {25, 1};
    settings.container = "matroska";
    (void)run(settings, directory / "export.mkv");
    settings = {};
    settings.width = 13;
    settings.height = 7;
    settings.videoCodec = "tiff";
    settings.profile = "rgba16";
    settings.container = "tiff";
    (void)run(settings, directory / "export.tiff");
    settings.videoCodec = "h264";
#ifdef BLOOM_OPENH264_TEST_RUNTIME
    if (std::filesystem::is_directory(BLOOM_OPENH264_TEST_RUNTIME)) {
        settings = {};
        settings.width = 64;
        settings.height = 48;
        settings.frames = 48;
        settings.videoCodec = "h264";
        settings.profile = "high";
        settings.container = "mov";
        settings.audioCodec = "pcm_s16le";
        settings.audioSamples = 96000;
        EncodeSessionOptionsV1 openh264;
        openh264.openh264Directory = BLOOM_OPENH264_TEST_RUNTIME;
        (void)run(settings, directory / "export-h264.mov", std::move(openh264));
        const auto render = std::filesystem::exists("/dev/dri/renderD128") ||
                            std::filesystem::exists("/dev/dri/renderD129");
        if (render) {
            auto vaapiSettings = settings;
            vaapiSettings.width = 128;
            vaapiSettings.height = 128;
            EncodeSessionOptionsV1 vaapi;
            vaapi.vaapi = true;
            try {
                (void)run(vaapiSettings, directory / "export-h264-vaapi.mov", std::move(vaapi));
            } catch (const std::runtime_error& error) {
                std::cout << "SKIP: VA-API H.264 round trip — driver refused the encode context: "
                          << error.what() << '\n';
            }
        } else {
            std::cout << "SKIP: VA-API H.264 round trip — no render node\n";
        }
    } else {
        std::cout << "SKIP: H.264 round trip — Cisco OpenH264 binary is not staged; shim only\n";
    }
#else
    std::cout << "SKIP: H.264 round trip — Cisco OpenH264 binary is not staged; shim only\n";
#endif
    EncodeSessionV1 rejected({});
    const auto unavailable = rejected.begin(settings);
    test::check(unavailable && unavailable->reason == Error::Unavailable &&
                    unavailable->detail == "H.264 encoder not installed",
                "H264 typed unavailable");
    settings = {};
    settings.width = 64;
    settings.height = 48;
    settings.frames = 2;
    bool cancelled = false;
    EncodeSessionV1 stopped({}, [&] { return cancelled; });
    ok(stopped.begin(settings));
    ok(stopped.video(image(settings, 0)));
    cancelled = true;
    const auto cancellation = stopped.video(image(settings, 1));
    test::check(cancellation && cancellation->reason == Error::Cancelled,
                "mid-encode cancellation");
    std::int64_t pid = 0;
    EncodeSessionOptionsV1 options;
    options.launched = [&](auto value) { pid = value; };
    EncodeSessionV1 crashed(options);
    ok(crashed.begin(settings));
    ok(crashed.video(image(settings, 0)));
    test::check(::kill(static_cast<pid_t>(pid), SIGKILL) == 0, "kill encode worker");
    const auto failure = crashed.video(image(settings, 1));
    test::check(failure && failure->reason == Error::Crashed, "typed encode crash");
    EncodeSessionV1 retry({});
    ok(retry.begin(settings));
    ok(retry.video(image(settings, 0)));
    ok(retry.video(image(settings, 1)));
    test::check(get(retry.finish()).frames == 2, "fresh encode retries cleanly");
}
} // namespace
int main(int argc, char** argv) {
    try {
        test::check(argc == 2, "test output directory");
        tests(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
