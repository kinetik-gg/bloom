#include "support.hpp"
#include <bloom/media/provider/encode_session.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/media_workers.hpp>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <string_view>
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
// The MCP repro: black with a centred 10x10 saturated red square, whose left edge falls on an
// odd column and so shares every 4:2:2/4:2:0 chroma sample with black.
bool redSquare = false;
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
                const bool inside = x + 5 >= settings.width / 2 && x < settings.width / 2 + 5 &&
                                    y + 5 >= settings.height / 2 && y < settings.height / 2 + 5;
                const auto value = c == 3      ? (settings.profile.starts_with("4444") && !redSquare
                                                      ? (x < settings.width / 2 ? 20000U : 50000U)
                                                      : 65535U)
                                   : redSquare ? (inside && c == 0 ? 65535U : 0U)
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
          EncodeSessionOptionsV1 options = {}, MediaProviderProvenanceV1* provenance = nullptr,
          EncodeQcV1* reopened = nullptr) {
    EncodeSessionV1 session(std::move(options));
    ok(session.begin(settings));
    if (provenance != nullptr)
        *provenance = session.provenance();
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
    if (reopened != nullptr)
        *reopened = qc;
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
    // H.264 ships with Bloom (owner, 2026-10-31): the bundled OpenH264 encodes it with nothing
    // installed, in MOV with PCM and in MP4 with AAC.
    settings = {};
    settings.width = 64;
    settings.height = 48;
    settings.frames = 48;
    settings.videoCodec = "h264";
    settings.profile = "high";
    settings.container = "mov";
    settings.audioCodec = "pcm_s16le";
    settings.audioSamples = 96000;
    {
        MediaProviderProvenanceV1 served;
        (void)run(settings, directory / "export-h264.mov", {}, &served);
        test::check(served.provider == kFfmpegProviderId && !served.fellBack(),
                    "software H.264 is served by the FFmpeg worker's bundled OpenH264");
        auto mp4 = settings;
        mp4.container = "mp4";
        mp4.audioCodec = "aac";
        (void)run(mp4, directory / "export-h264.mp4");
    }
    // VA-API requested where no render node encodes falls back to OpenH264 without a refusal: the
    // fallback is recorded in provenance, never shown.
    {
        auto absent = settings;
        absent.vaapiDevice = "/dev/dri/renderD-absent";
        EncodeSessionOptionsV1 vaapi;
        vaapi.vaapi = true;
        MediaProviderProvenanceV1 served;
        (void)run(absent, directory / "export-h264-fallback.mov", std::move(vaapi), &served);
        test::check(served.provider == kFfmpegProviderId && served.fellBack() &&
                        served.fallbacks.front().provider == kFfmpegVaapiProviderId,
                    "H.264 falls back from VA-API to the bundled OpenH264 silently");
    }
    // HEVC ships with Bloom too: the bundled Kvazaar encodes Main in MOV and MP4, serves a Main 10
    // request as 8-bit Main, pads an extent that is not whole 8x8 blocks (cropped again by the
    // conformance window), and is VA-API's silent fallback.
    {
        auto hevc = settings;
        hevc.videoCodec = "hevc";
        hevc.profile = "main";
        MediaProviderProvenanceV1 served;
        (void)run(hevc, directory / "export-hevc.mov", {}, &served);
        test::check(served.provider == kFfmpegProviderId && !served.fellBack(),
                    "software HEVC is served by the FFmpeg worker's bundled Kvazaar");
        auto mp4 = hevc;
        mp4.container = "mp4";
        mp4.audioCodec = "aac";
        (void)run(mp4, directory / "export-hevc.mp4");
        EncodeQcV1 delivered;
        (void)run(hevc, directory / "export-hevc-main.mov", {}, nullptr, &delivered);
        test::check(delivered.profile == "Main" && delivered.bitDepth == 8 &&
                        delivered.adjustment.empty(),
                    "an HEVC Main request reports the Main 8-bit stream it delivered");
        auto main10 = hevc;
        main10.profile = "main10";
        (void)run(main10, directory / "export-hevc-main10.mov", {}, nullptr, &delivered);
        test::check(
            delivered.profile == "Main" && delivered.bitDepth == 8 &&
                delivered.adjustment.find("Main 10") != std::string::npos,
            "a Main 10 request served by Kvazaar reports 8-bit Main and why, never silently");
        // 1918x1078 is not whole 8x8 blocks: Kvazaar codes the padded picture and crops it with
        // the conformance window, and the reopen check sees the exact extent.
        auto unaligned = hevc;
        unaligned.width = 1918;
        unaligned.height = 1078;
        unaligned.frames = 6;
        unaligned.audioCodec.clear();
        unaligned.audioSamples = 0;
        (void)run(unaligned, directory / "export-hevc-1918x1078.mov");
        auto absent = hevc;
        absent.vaapiDevice = "/dev/dri/renderD-absent";
        EncodeSessionOptionsV1 vaapi;
        vaapi.vaapi = true;
        MediaProviderProvenanceV1 fell;
        (void)run(absent, directory / "export-hevc-fallback.mov", std::move(vaapi), &fell);
        test::check(fell.provider == kFfmpegProviderId && fell.fellBack() &&
                        fell.fallbacks.front().provider == kFfmpegVaapiProviderId,
                    "HEVC falls back from VA-API to the bundled Kvazaar silently");
    }
    if (std::filesystem::exists("/dev/dri/renderD128") ||
        std::filesystem::exists("/dev/dri/renderD129")) {
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
    // Acknowledgements are collected lazily: the crash is reported by the frame write if the pipe
    // is already closed, otherwise by the next call that collects acknowledgements.
    auto failure = crashed.video(image(settings, 1));
    if (!failure) {
        const auto finished = crashed.finish();
        if (const auto* error = std::get_if<Unavailable>(&finished))
            failure = *error;
    }
    test::check(failure && failure->reason == Error::Crashed, "typed encode crash");
    EncodeSessionV1 retry({});
    ok(retry.begin(settings));
    ok(retry.video(image(settings, 0)));
    ok(retry.video(image(settings, 1)));
    test::check(get(retry.finish()).frames == 2, "fresh encode retries cleanly");
}
// Every lossy preset reopens a tiny saturated feature within tolerance profile v2: the verifier
// models the codec's own chroma sampling instead of failing what 4:2:2/4:2:0 removes by design.
void chromaSampling(const std::filesystem::path& directory) {
    redSquare = true;
    EncodeSettingsV1 settings;
    settings.width = 640;
    settings.height = 360;
    settings.frames = 12;
    for (const auto* profile : {"proxy", "lt", "422", "hq", "4444", "4444xq"}) {
        settings.profile = profile;
        (void)run(settings, directory / (std::string("red-square-prores-") + profile + ".mov"));
    }
    settings.videoCodec = "dnxhd";
    settings.container = "mxf";
    for (const auto* profile : {"dnxhr_lb", "dnxhr_sq", "dnxhr_hq", "dnxhr_hqx", "dnxhr_444"}) {
        settings.profile = profile;
        (void)run(settings, directory / (std::string("red-square-") + profile + ".mxf"));
    }
    settings.profile = "dnxhd";
    settings.width = 1920;
    settings.height = 1080;
    settings.rate = {25, 1};
    (void)run(settings, directory / "red-square-dnxhd.mxf");
    settings = {};
    settings.width = 640;
    settings.height = 360;
    settings.frames = 12;
    settings.videoCodec = "h264";
    settings.profile = "high";
    (void)run(settings, directory / "red-square-h264.mov");
    // The bundled Kvazaar at 640x360 and at 1920x1080, where its encoder threads must fit the
    // worker's address-space ceiling.
    {
        auto hevc = settings;
        hevc.videoCodec = "hevc";
        hevc.profile = "main";
        (void)run(hevc, directory / "red-square-hevc.mov");
        hevc.width = 1920;
        hevc.height = 1080;
        (void)run(hevc, directory / "red-square-hevc-1080p.mov");
    }
    if (!std::filesystem::exists("/dev/dri/renderD128") &&
        !std::filesystem::exists("/dev/dri/renderD129")) {
        std::cout << "SKIP: VA-API red square — no render node\n";
        return;
    }
    // Issue #394: 24 frames, six times the worker's four VA-API input surfaces, so a surface that
    // is not returned to the pool fails the upload. The driver refusing the device, its frame
    // context or the encoder skips; an upload failure or a verification failure is a failure.
    settings.frames = 24;
    for (const auto& [codec, profile] :
         {std::pair{"h264", "high"}, std::pair{"hevc", "main"}, std::pair{"hevc", "main10"}}) {
        settings.videoCodec = codec;
        settings.profile = profile;
        EncodeSessionOptionsV1 vaapi;
        vaapi.vaapi = true;
        try {
            (void)run(settings,
                      directory /
                          (std::string("red-square-") + codec + "-" + profile + "-vaapi.mov"),
                      std::move(vaapi));
        } catch (const std::runtime_error& error) {
            const std::string_view detail = error.what();
            if ((!detail.starts_with("VA-API") || detail == "VA-API frame upload failed") &&
                detail != "Encoder refused profile or stream layout" &&
                detail != "Encoder is not in the intake")
                throw;
            std::cout << "SKIP: VA-API " << codec << ' ' << profile
                      << " red square — the driver refused: " << error.what() << '\n';
        }
    }
}
} // namespace
int main(int argc, char** argv) {
    try {
        test::check(argc == 2, "test output directory");
        tests(argv[1]);
        chromaSampling(argv[1]);
        // Issue #352: one 8K UHD ProRes frame exports and passes reopen quality check.
        EncodeSettingsV1 eightK;
        eightK.width = 7680;
        eightK.height = 4320;
        eightK.frames = 1;
        (void)run(eightK, std::filesystem::path(argv[1]) / "export-8k-prores.mov");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
