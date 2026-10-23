// Local scrub/playback latency probe for the supervised video decode path (issue #366).
// It is built beside the media tests but is not a CTest case: inputs are large, locally generated
// media that is never committed. Usage:
//   bloom_media_video_decode_benchmark <video> [--frames N] [--scrub N] [--seed S] [--hardware]
//                                      [--parity]
// --parity decodes every frame through the preview hardware route and the software reference
// and reports byte mismatches instead of timings.
// Output is one JSON object on stdout.
#include <algorithm>
#include <bloom/media/video/session.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
struct Series final {
    std::vector<double> milliseconds;
    std::size_t failures = 0;
    std::string firstFailure;
};
void json(std::ostream& out, std::string_view name, Series series) {
    auto& v = series.milliseconds;
    std::ranges::sort(v);
    double total = 0;
    for (const auto value : v)
        total += value;
    const auto at = [&](double q) {
        return v.empty() ? 0.0
                         : v[std::min(v.size() - 1,
                                      static_cast<std::size_t>(q * static_cast<double>(v.size())))];
    };
    out << '"' << name << "\":{\"count\":" << v.size() << ",\"failures\":" << series.failures
        << ",\"mean_ms\":" << (v.empty() ? 0.0 : total / static_cast<double>(v.size()))
        << ",\"p50_ms\":" << at(0.5) << ",\"p95_ms\":" << at(0.95)
        << ",\"max_ms\":" << (v.empty() ? 0.0 : v.back()) << ",\"total_ms\":" << total
        << ",\"first_failure\":\"" << series.firstFailure << "\"}";
}
} // namespace

int main(int argc, char** argv) {
    using namespace bloom::media;
    if (argc < 2) {
        std::cerr << "usage: " << argv[0]
                  << " <video> [--frames N] [--scrub N] [--seed S] [--hardware]\n";
        return 2;
    }
    std::uint64_t frames = 240, scrubs = 60, seed = 366;
    bool hardware = false, parity = false;
    for (int i = 2; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--frames" && i + 1 < argc)
            frames = std::strtoull(argv[++i], nullptr, 10);
        else if (argument == "--scrub" && i + 1 < argc)
            scrubs = std::strtoull(argv[++i], nullptr, 10);
        else if (argument == "--seed" && i + 1 < argc)
            seed = std::strtoull(argv[++i], nullptr, 10);
        else if (argument == "--hardware")
            hardware = true;
        else if (argument == "--parity")
            parity = true;
        else
            return 2;
    }
    video::VideoDecodeSession session(argv[1]);
    const auto opened = Clock::now();
    const auto probed = session.probe();
    if (!std::holds_alternative<provider::ProbeResult>(probed)) {
        std::cerr << "probe failed: " << std::get<provider::Unavailable>(probed).detail << '\n';
        return 1;
    }
    const auto& probe = std::get<provider::ProbeResult>(probed);
    const auto stream = static_cast<std::uint32_t>(
        std::ranges::find_if(probe.streams,
                             [](const auto& s) { return s.kind == provider::MediaKind::Video; }) -
        probe.streams.begin());
    if (stream >= probe.streams.size())
        return 1;
    const auto& descriptor = probe.streams[stream];
    const auto probeMs = std::chrono::duration<double, std::milli>(Clock::now() - opened).count();
    frames =
        std::min<std::uint64_t>(frames, descriptor.frameCount ? descriptor.frameCount : frames);
    std::string route;
    if (parity) {
        video::VideoDecodeSession software(argv[1]);
        std::uint64_t mismatches = 0, hardwareFrames = 0, failures = 0;
        for (std::uint64_t i = 0; i < frames; ++i) {
            video::FrameDecodeProvenance provenance;
            const auto accelerated =
                session.frame(probe, stream, i, 0, nullptr, {}, {}, {}, {},
                              {video::SourceRevisionPolicy::Retained, true}, {true, &provenance});
            const auto reference = software.frame(probe, stream, i, 0, nullptr, {}, {}, {}, {},
                                                  {video::SourceRevisionPolicy::Retained, true});
            using Frame = std::shared_ptr<const provider::FrameProduct>;
            if (!std::holds_alternative<Frame>(accelerated) ||
                !std::holds_alternative<Frame>(reference)) {
                ++failures;
                continue;
            }
            hardwareFrames += provenance.implementation == provider::Implementation::Hardware;
            mismatches += !(*std::get<Frame>(accelerated) == *std::get<Frame>(reference));
            route = provenance.description;
        }
        std::cout << "{\"file\":\"" << std::string(argv[1]) << "\",\"frames\":" << frames
                  << ",\"hardware_frames\":" << hardwareFrames << ",\"mismatches\":" << mismatches
                  << ",\"failures\":" << failures << ",\"route\":\"" << route << "\"}\n";
        return mismatches == 0 && failures == 0 ? 0 : 1;
    }
    const auto decode = [&](std::uint64_t index, Series& series) {
        const auto start = Clock::now();
        video::FrameDecodeRoute decodeRoute;
        decodeRoute.preferHardware = hardware;
        video::FrameDecodeProvenance provenance;
        decodeRoute.provenance = &provenance;
        auto result = session.frame(probe, stream, index, 0, nullptr, {}, {}, {}, {},
                                    {video::SourceRevisionPolicy::Retained, true}, decodeRoute);
        series.milliseconds.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        if (const auto* error = std::get_if<provider::Unavailable>(&result)) {
            if (series.failures++ == 0)
                series.firstFailure = error->detail;
        }
        route = provenance.description;
    };
    Series cold, playback, stride, scrub;
    decode(0, cold);
    for (std::uint64_t i = 1; i < frames; ++i)
        decode(i, playback);
    // A 50p source in a 25p composition requests every second frame.
    for (std::uint64_t i = 0; i < frames; i += 2)
        decode(i, stride);
    std::mt19937_64 random(seed);
    std::uniform_int_distribution<std::uint64_t> pick(0, frames - 1);
    for (std::uint64_t i = 0; i < scrubs; ++i)
        decode(pick(random), scrub);
    std::cout << "{\"file\":\"" << std::string(argv[1]) << "\",\"codec\":\"" << descriptor.codec
              << "\",\"width\":" << descriptor.width << ",\"height\":" << descriptor.height
              << ",\"frames\":" << frames << ",\"route\":\"" << route
              << "\",\"probe_ms\":" << probeMs << ',';
    json(std::cout, "cold_first_frame", cold);
    std::cout << ',';
    json(std::cout, "playback", playback);
    std::cout << ',';
    json(std::cout, "stride2", stride);
    std::cout << ',';
    json(std::cout, "scrub", scrub);
    std::cout << "}\n";
    return 0;
}
