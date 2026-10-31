#include "support.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/worker_pool.hpp>
#include <bloom/platform/file_snapshot.hpp>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <thread>
#include <unistd.h>
using namespace bloom::media::provider;
namespace {
struct Fixture {
    CapabilityRegistry registry;
    Handshake hello = ffmpegHandshake();
    bloom::runtime::TaskScheduler scheduler;
    std::atomic<std::int64_t> pid{0};
    MediaWorkerPool pool;
    Fixture() : pool(options()) {
        for (const auto& d : hello.declarations) {
            test::check(std::holds_alternative<Digest>(registry.registerProvider(d)),
                        "register FFmpeg");
            const auto qualification = registry.qualifyPipeline(ffmpegPipeline(d));
            if (d.capability.purpose == Purpose::Export) {
                const auto* error = std::get_if<Unavailable>(&qualification);
                test::check(error && error->reason == Error::Unavailable,
                            "preview export does not qualify a strict export pipeline");
            } else {
                test::check(std::holds_alternative<Digest>(qualification), "qualify FFmpeg read");
            }
        }
    }
    WorkerPoolOptions options() {
        WorkerPoolOptions o;
        o.process.executable = BLOOM_FFMPEG_WORKER;
        o.expected = hello;
        o.launched = [this](std::int64_t value) { pid.store(value); };
        o.timeout = std::chrono::seconds(15);
        return o;
    }
    WorkerReply run(Role role, const std::filesystem::path& path,
                    const ProbeResult* probe = nullptr, std::uint64_t position = 0,
                    unsigned stream = 0) {
        const auto found = std::ranges::find_if(hello.declarations, [&](const auto& d) {
            return d.capability.role == role &&
                   (!probe || role == Role::DemuxIndex ||
                    d.capability.codec == probe->streams[stream].codec);
        });
        test::check(found != hello.declarations.end(), "declared codec");
        auto attempt = std::get<MediaAttempt>(registry.begin(ffmpegPipeline(*found)));
        CallRequest request;
        request.capability = found->capability;
        request.source = path.string();
        request.frame = position;
        request.stream = stream;
        if (probe) {
            request.sourceDigest = probe->sourceDigest;
            request.width = probe->streams[stream].width;
            request.height = probe->streams[stream].height;
        }
        auto ticket = pool.submit(
            scheduler,
            {bloom::runtime::TaskOwnerKind::Application, bloom::runtime::TaskOwnerId::fromRaw(913)},
            attempt, 0, request);
        test::check(ticket.submission.accepted(), "FFmpeg task admission");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!ticket.submission.handle.tryTakeResult()) {
            test::check(std::chrono::steady_clock::now() < deadline, "FFmpeg task watchdog");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto reply = ticket.result();
        test::check(reply != nullptr, "FFmpeg reply");
        if (const auto* error = std::get_if<Unavailable>(reply.get()))
            std::cerr << "worker result: " << error->detail << '\n';
        return *reply;
    }
};
void run(const std::filesystem::path& directory) {
    // The bundled OpenH264 is always the software H.264 encoder: no verified download, no
    // entitlement suffix, nothing to install.
    const auto software = ffmpegHandshake(false);
    const auto h264 = std::ranges::find_if(software.declarations, [](const auto& declaration) {
        return declaration.capability.role == Role::VideoEncode &&
               declaration.capability.codec == "h264";
    });
    test::check(h264 != software.declarations.end() && h264->capability.profile == "high" &&
                    h264->capability.bitDepth == 8 && h264->capability.chroma == "yuv420p" &&
                    h264->capability.range == "limited" &&
                    h264->execution.entitlement.find("openh264") == std::string::npos,
                "the software handshake always declares the bundled H.264 encoder");
    for (const auto& handshake : {software, ffmpegHandshake(true)})
        test::check(std::ranges::count_if(handshake.declarations,
                                          [](const auto& declaration) {
                                              return declaration.capability.container == "mp4" &&
                                                     (declaration.capability.role == Role::Mux ||
                                                      declaration.capability.role ==
                                                          Role::ReopenDecode);
                                          }) == 2,
                    "MP4 is muxed and reopened by both FFmpeg generations");
    Fixture fixture;
    const auto hardware = ffmpegHandshake(true);
    for (const auto& declaration : hardware.declarations) {
        const auto pipeline = ffmpegPipeline(declaration);
        test::check(declaration.execution.implementation == Implementation::Hardware &&
                        declaration.evidence.result == QcResult::Incomplete &&
                        pipeline.result == QcResult::Incomplete &&
                        pipeline.determinism == MediaDeterminismV1::NoDeterminismClaim,
                    "VA-API never inherits software qualification or determinism");
    }
    // Preview VA-API decode: discovered device, H.264/HEVC decode only, admitted for preview
    // through its own hardware execution key and never claiming determinism.
    const auto decodeHardware = ffmpegHardwareDecodeHandshake();
    test::check(decodeHardware.declarations.size() == 2 &&
                    decodeHardware.execution.device == "vaapi-render-node-discovered" &&
                    decodeHardware.execution.implementation == Implementation::Hardware,
                "hardware decode handshake names discovery, not a fixed render node");
    CapabilityRegistry hardwareRegistry;
    for (const auto& declaration : decodeHardware.declarations) {
        test::check(declaration.capability.role == Role::VideoDecode &&
                        declaration.capability.purpose == Purpose::Preview &&
                        (declaration.capability.codec == "h264" ||
                         declaration.capability.codec == "hevc") &&
                        declaration.execution == decodeHardware.execution,
                    "hardware decode is preview H.264/HEVC only");
        const auto pipeline = ffmpegPipeline(declaration);
        test::check(pipeline.determinism == MediaDeterminismV1::NoDeterminismClaim,
                    "hardware decode makes no determinism claim");
        test::check(
            std::holds_alternative<Digest>(hardwareRegistry.registerProvider(declaration)) &&
                std::holds_alternative<Digest>(hardwareRegistry.qualifyPipeline(pipeline)),
            "hardware preview decode is selectable by capability");
    }
    for (const auto* filename : {"numbered-h264.mp4", "numbered-prores.mov"}) {
        const auto path = directory / filename;
        const auto probe = std::get<ProbeResult>(fixture.run(Role::Probe, path));
        const auto& stream = probe.streams.front();
        test::check(stream.width == 64 && stream.height == 48 && stream.rate == Rational{24, 1} &&
                        stream.duration == Rational{2, 1} && stream.frameCount == 48,
                    "pinned probe dimensions, rate, duration, frame count");
        test::check(stream.timebase == Rational{1, 12288}, "pinned MP4/MOV timebase");
        test::check(stream.colour == ColourTags{1, 1, 1, 1}, "pinned Rec.709 tags");
        test::check(stream.timecode == "01:00:00:00", "pinned timecode");
        test::check(!stream.appleAuthorized, "ProRes is not Apple authorized");
        const auto index = std::get<DemuxIndex>(fixture.run(Role::DemuxIndex, path, &probe));
        test::check(!index.keyframes.empty() && valid(index), "bounded exact keyframe index");
        unsigned keys = 0;
        for (const auto& key : index.keyframes) {
            if (key.stream != 0)
                continue;
            const auto frame = keys * (stream.codec == "h264" ? 12U : 1U);
            const auto divisor = std::gcd(frame, 24U);
            test::check(key.pts == Rational{frame / divisor, 24U / divisor} && key.dts == key.pts,
                        "per-stream keyframe index has exact presentation and decode times");
            ++keys;
        }
        test::check(keys == (stream.codec == "h264" ? 4U : 48U), "all video keyframes are indexed");
        for (const auto n : {0U, 47U, 12U, 1U, 35U, 11U, 24U, 2U, 46U, 0U}) {
            const auto product =
                std::get<FrameProduct>(fixture.run(Role::VideoDecode, path, &probe, n));
            const auto divisor = std::gcd(n, 24U);
            test::check(product.pts == Rational{n / divisor, 24U / divisor},
                        "frame-accurate forward/backward seek");
            const auto first = std::to_integer<unsigned>(product.planes[0].bytes[0]);
            if (stream.codec == "h264")
                test::check(first == 16 + n * 3, "lossless numbered H.264 luma oracle");
            else {
                const auto value =
                    first + (std::to_integer<unsigned>(product.planes[0].bytes[1]) << 8U);
                test::check(
                    std::abs(static_cast<int>(value) - static_cast<int>((16 + n * 3) * 256)) <= 256,
                    "numbered ProRes luma oracle");
            }
            // Read the two seven-segment glyphs, independently of the background-luma oracle.
            constexpr std::array<unsigned, 10> digitMasks{63,  6,   91, 79,  102,
                                                          109, 125, 7,  127, 111};
            constexpr std::array<std::pair<unsigned, unsigned>, 7> segments{
                {{7, 1}, {13, 7}, {13, 19}, {7, 25}, {0, 19}, {0, 7}, {7, 13}}};
            unsigned number = 0;
            for (unsigned origin : {16U, 34U}) {
                unsigned mask = 0;
                const auto& plane = product.planes.front();
                for (std::size_t bit = 0; bit < segments.size(); ++bit) {
                    const auto [x, y] = segments[bit];
                    const bool wide = product.format == PixelFormat::Yuva444p16;
                    const auto offset = static_cast<std::size_t>(y + 10) * plane.stride +
                                        static_cast<std::size_t>(origin + x) * (wide ? 2U : 1U);
                    const auto luma = wide ? std::to_integer<unsigned>(plane.bytes[offset + 1])
                                           : std::to_integer<unsigned>(plane.bytes[offset]);
                    if (luma > 190)
                        mask |= 1U << bit;
                }
                const auto* digit = std::ranges::find(digitMasks, mask);
                test::check(digit != digitMasks.end(), "burned-in digit is legible");
                number = number * 10 + static_cast<unsigned>(digit - digitMasks.begin());
            }
            test::check(number == n, "frame N visibly carries number N after random seek");
            const auto again =
                std::get<FrameProduct>(fixture.run(Role::VideoDecode, path, &probe, n));
            test::check(product == again, "byte-stable software decode");
        }
        if (stream.codec == "prores") {
            test::check(probe.streams[1].sampleRate == 48000 &&
                            probe.streams[1].codec == "pcm_s16le",
                        "PCM probe");
            for (const auto sample : {0U, 2000U, 41001U, 80003U}) {
                const auto block =
                    std::get<AudioBlock>(fixture.run(Role::AudioDecode, path, &probe, sample, 1));
                test::check(valid(block) && block.channels.size() == 1, "planar audio product");
                for (unsigned i = 0; i < 1024; ++i)
                    test::check(block.channels[0][i] ==
                                    static_cast<float>(static_cast<int>((sample + i) % 997) - 498) /
                                        1024.0F,
                                "sample-accurate audio placement");
            }
        }
        auto changed = probe;
        changed.sourceDigest = Digest{};
        test::check(std::get<Unavailable>(fixture.run(Role::VideoDecode, path, &changed)).reason ==
                        Error::SourceChanged,
                    "source digest change is typed");
    }
    const auto alphaPath = directory / "alpha-prores.mov";
    const auto alphaProbe = std::get<ProbeResult>(fixture.run(Role::Probe, alphaPath));
    const auto alphaProduct =
        std::get<FrameProduct>(fixture.run(Role::VideoDecode, alphaPath, &alphaProbe, 23));
    test::check(alphaProduct.format == PixelFormat::Yuva444p16 && alphaProduct.planes.size() == 4,
                "ProRes 4444 carries a bounded alpha plane");
    const auto& alpha = alphaProduct.planes[3];
    const auto opacity = std::to_integer<unsigned>(alpha.bytes[0]) +
                         (std::to_integer<unsigned>(alpha.bytes[1]) << 8U);
    test::check(opacity > 32600 && opacity < 33000, "ProRes decoder retains half alpha");
    // Keep source hashing active long enough to deterministically kill an admitted
    // VideoDecode request after the worker has opened its input, not during spawn.
    const auto crashPath = directory / "crash-in-flight.mp4";
    std::filesystem::copy_file(directory / "numbered-h264.mp4", crashPath,
                               std::filesystem::copy_options::overwrite_existing);
    {
        std::ofstream appended(crashPath, std::ios::app | std::ios::binary);
        const std::array<char, 65536> zeros{};
        for (unsigned i = 0; i < 1024; ++i)
            appended.write(zeros.data(), zeros.size());
    }
    const auto originalPath = directory / "numbered-h264.mp4";
    const auto originalProbe = std::get<ProbeResult>(fixture.run(Role::Probe, originalPath));
    fixture.pid.store(0);
    std::atomic<bool> killed{false};
    std::jthread killer([&](const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            const auto pid = fixture.pid.load();
            if (pid > 0) {
                std::error_code error;
                const auto descriptors =
                    std::filesystem::path("/proc") / std::to_string(pid) / "fd";
                for (std::filesystem::directory_iterator it(descriptors, error), end;
                     !error && it != end; it.increment(error)) {
                    std::error_code linkError;
                    if (std::filesystem::read_symlink(it->path(), linkError) == crashPath &&
                        !linkError) {
                        killed.store(::kill(static_cast<pid_t>(pid), SIGKILL) == 0);
                        return;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    const auto crashed = fixture.run(Role::VideoDecode, crashPath, &originalProbe, 35);
    killer.request_stop();
    killer.join();
    test::check(killed.load(), "worker killed during admitted video read");
    test::check(std::holds_alternative<Unavailable>(crashed) &&
                    std::get<Unavailable>(crashed).reason == Error::Crashed,
                "mid-decode crash is typed");
    test::check(std::holds_alternative<FrameProduct>(
                    fixture.run(Role::VideoDecode, originalPath, &originalProbe, 35)),
                "same pool recovers on next decode");
    std::filesystem::remove(crashPath);
    const auto malformed = directory / "malformed.mp4";
    {
        std::ofstream stream(malformed);
        stream << "not a movie";
    }
    const auto refused = fixture.run(Role::Probe, malformed);
    const auto* error = std::get_if<Unavailable>(&refused);
    test::check(error && error->reason == Error::Corrupt, "corrupt input refused");
}
// Issue #366: a retained worker continues sequential audio blocks from the previous block's end,
// so decoding N blocks of a stream is linear in N rather than redecoding from the start each time.
void sequentialAudioIsLinear(const std::filesystem::path& directory) {
    constexpr std::uint32_t rate = 48000, block = 65536, blocks = 256;
    constexpr std::uint64_t samples = std::uint64_t{block} * blocks;
    const auto path = directory / "linear-audio.wav";
    {
        std::ofstream wav(path, std::ios::binary | std::ios::trunc);
        const auto u32 = [&](std::uint32_t v) {
            for (unsigned i = 0; i < 4; ++i)
                wav.put(static_cast<char>((v >> (8U * i)) & 255U));
        };
        const auto u16 = [&](std::uint16_t v) {
            wav.put(static_cast<char>(v & 255U));
            wav.put(static_cast<char>(v >> 8U));
        };
        const auto data = static_cast<std::uint32_t>(samples * 2U);
        wav.write("RIFF", 4);
        u32(36U + data);
        wav.write("WAVEfmt ", 8);
        u32(16);
        u16(1);
        u16(1);
        u32(rate);
        u32(rate * 2U);
        u16(2);
        u16(16);
        wav.write("data", 4);
        u32(data);
        std::vector<char> bytes(samples * 2U);
        for (std::uint64_t n = 0; n < samples; ++n) {
            const auto value = static_cast<std::uint16_t>(
                static_cast<std::int16_t>((static_cast<int>(n % 997) - 498) * 32));
            bytes[n * 2] = static_cast<char>(value & 255U);
            bytes[n * 2 + 1] = static_cast<char>(value >> 8U);
        }
        wav.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        test::check(wav.good(), "write linear-audio fixture");
    }
    const auto quota = std::make_shared<bloom::platform::FileSnapshotQuota>(1ULL << 36U, 2);
    auto captured =
        bloom::platform::ReadOnlyFileSnapshot::capture(path, directory, quota, Limits::sourceBytes);
    auto* owned = std::get_if<std::unique_ptr<bloom::platform::ReadOnlyFileSnapshot>>(&captured);
    test::check(owned != nullptr, "audio snapshot");
    const std::shared_ptr<const bloom::platform::ReadOnlyFileSnapshot> snapshot(std::move(*owned));
    const auto hello = ffmpegHandshake();
    CapabilityRegistry registry;
    for (const auto& d : hello.declarations) {
        (void)registry.registerProvider(d);
        (void)registry.qualifyPipeline(ffmpegPipeline(d));
    }
    WorkerPoolOptions options;
    options.process.executable = BLOOM_FFMPEG_WORKER;
    options.expected = hello;
    options.capacity = 1;
    options.timeout = std::chrono::seconds(30);
    options.retainedVideo = true;
    MediaWorkerPool pool(options);
    const auto declaration = [&](Role role, std::string_view codec) {
        const auto found = std::ranges::find_if(hello.declarations, [&](const auto& d) {
            return d.capability.role == role && d.capability.purpose == Purpose::Preview &&
                   (codec.empty() || d.capability.codec == codec);
        });
        test::check(found != hello.declarations.end(), "declared audio capability");
        return *found;
    };
    const auto probeDeclaration = declaration(Role::Probe, {});
    CallRequest probeRequest;
    probeRequest.capability = probeDeclaration.capability;
    probeRequest.source = path.string();
    const auto probe = std::get<ProbeResult>(
        pool.call(std::get<MediaAttempt>(registry.begin(ffmpegPipeline(probeDeclaration))), 0,
                  probeRequest, snapshot, {}, true));
    test::check(probe.streams.size() == 1 && probe.streams[0].codec == "pcm_s16le",
                "PCM WAV probe");
    const auto audioDeclaration = declaration(Role::AudioDecode, "pcm_s16le");
    std::vector<double> milliseconds;
    for (std::uint32_t b = 0; b < blocks; ++b) {
        CallRequest request;
        request.capability = audioDeclaration.capability;
        request.source = path.string();
        request.sourceDigest = probe.sourceDigest;
        request.frame = std::uint64_t{b} * block;
        request.samples = block;
        const auto start = std::chrono::steady_clock::now();
        auto reply =
            pool.call(std::get<MediaAttempt>(registry.begin(ffmpegPipeline(audioDeclaration))), 0,
                      std::move(request), snapshot, {}, true);
        milliseconds.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count());
        const auto* audio = std::get_if<AudioBlock>(&reply);
        test::check(audio != nullptr && audio->channels.size() == 1 &&
                        audio->channels[0].size() == block,
                    "sequential retained audio block");
        for (const auto i : {0U, 1U, block / 2U, block - 1U}) {
            const auto n = std::uint64_t{b} * block + i;
            test::check(audio->channels[0][i] ==
                            static_cast<float>(static_cast<int>(n % 997) - 498) / 1024.0F,
                        "sample-exact sequential audio continuation");
        }
    }
    const auto mean = [&](std::size_t from) {
        double total = 0;
        for (std::size_t i = from; i < from + 16; ++i)
            total += milliseconds[i];
        return total / 16.0;
    };
    // Redecoding from the start would make the last blocks read ~250 blocks of audio each.
    const auto early = mean(1), late = mean(blocks - 16);
    std::cout << "audio block mean early " << early << " ms, late " << late << " ms\n";
    test::check(late < early * 3.0 + 10.0, "sequential audio decode is linear, not quadratic");
    // A non-continuing request reopens and still returns exact samples.
    CallRequest jump;
    jump.capability = audioDeclaration.capability;
    jump.source = path.string();
    jump.sourceDigest = probe.sourceDigest;
    jump.frame = 1234567;
    jump.samples = 1000;
    const auto jumped = std::get<AudioBlock>(
        pool.call(std::get<MediaAttempt>(registry.begin(ffmpegPipeline(audioDeclaration))), 0, jump,
                  snapshot, {}, true));
    test::check(jumped.channels[0][0] ==
                    static_cast<float>(static_cast<int>(1234567 % 997) - 498) / 1024.0F,
                "reopened audio request is sample exact");
    std::filesystem::remove(path);
}
} // namespace
int main(int argc, char** argv) {
    try {
        test::check(argc == 2, "fixture directory");
        run(argv[1]);
        sequentialAudioIsLinear(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
