#include <array>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/media/video/audio.hpp>
#include <bloom/media/video/colour.hpp>
#include <bloom/media/video/session.hpp>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void run(const std::filesystem::path& directory) {
    using namespace bloom::media;
    provider::StreamDescriptor fractionalAudio;
    fractionalAudio.sampleRate = 48'000;
    fractionalAudio.channelLayout = {"L", "R"};
    fractionalAudio.duration = {1001, 48'000'000};
    check(video::audioClipFrameCount(fractionalAudio) == 2,
          "fractional final audio sample is included in the admitted range");
    fractionalAudio.duration = {std::numeric_limits<std::int64_t>::max(), 1};
    check(!video::audioClipFrameCount(fractionalAudio),
          "unrepresentable audio sample count refuses before allocation");
    video::VideoDecodeSession session(directory / "numbered-h264.mp4");
    const auto probe = std::get<provider::ProbeResult>(session.probe());
    video::DecodedVideoCache cache(4608);
    const auto first = std::get<std::shared_ptr<const provider::FrameProduct>>(
        session.frame(probe, 0, 12, 0, &cache));
    check(cache.residentBytes() == 4608 && cache.byteBudget() == 4608,
          "video cache has an independent exact byte budget");
    const auto cached = std::get<std::shared_ptr<const provider::FrameProduct>>(
        session.frame(probe, 0, 12, 0, &cache));
    check(first == cached, "decoded frame cache hit");
    const auto next = std::get<std::shared_ptr<const provider::FrameProduct>>(
        session.frame(probe, 0, 13, 0, &cache));
    check(cache.residentBytes() == 4608 && !cache.find({probe.sourceDigest, 0, 12, 0, {}, {}, {}}),
          "LRU eviction obeys byte budget");
    check(!cache.find({probe.sourceDigest, 0, 13, 1, {}, {}, {}}),
          "interpretation is part of the cache key");
    const provider::Digest configRevision = probe.sourceDigest;
    const video::FrameKey transformedKey{probe.sourceDigest, 0, 12, 0, "sRGB - Texture", "ACEScg",
                                         configRevision};
    cache.store(transformedKey, first);
    check(cache.find(transformedKey) == first,
          "input colour-space, working-space, and config revision form the video cache key");
    check(!cache.find({probe.sourceDigest, 0, 12, 0, "sRGB - Texture", "ACEScg", {}}),
          "a changed config revision misses the video frame cache");
    cache.setByteBudget(0);
    check(cache.residentBytes() == 0, "budget shrink releases resident frames");
    const auto window = bloom::render::ImageWindow::create(0, 0, 64, 48);
    check(static_cast<bool>(window), "test image extent");
    const auto descriptor = bloom::render::Rgba32fImageDescriptor::create(
        *window.value(), *window.value(), bloom::core::PixelAspectRatio::square());
    check(static_cast<bool>(descriptor), "test image descriptor");
    const auto converted = bloom::media::video::videoToSceneLinear(*first, 0, *descriptor.value(),
                                                                   1, 1, std::size_t{64} * 48 * 16);
    check(std::holds_alternative<bloom::render::Rgba32fImage>(converted),
          "Rec.709 YUV converts through color boundary");
    const auto& image = std::get<bloom::render::Rgba32fImage>(converted);
    const auto pixel = image.pixels().front();
    check(pixel.red() == pixel.green() && pixel.green() == pixel.blue() && pixel.alpha() == 1,
          "neutral scene-linear opaque pixel");
    check(std::abs(pixel.red() - 0.0418131874F) < 0.000001F,
          "Rec.709 inverse transfer reference value");
    const auto srgb = bloom::media::video::videoToSceneLinear(*first, 1, *descriptor.value(), 1, 1,
                                                              std::size_t{64} * 48 * 16);
    check(std::holds_alternative<bloom::render::Rgba32fImage>(srgb) &&
              std::abs(std::get<bloom::render::Rgba32fImage>(srgb).pixels().front().red() -
                       0.0230719284F) < 0.000001F,
          "sRGB inverse transfer reference value");
    provider::FrameProduct alphaFrame;
    alphaFrame.format = provider::PixelFormat::Yuva444p16;
    alphaFrame.colour = {1, 1, 1, 1};
    for (unsigned code : {32768U, 32768U, 32768U, 32768U}) {
        provider::CpuPlane plane;
        plane.width = plane.height = 1;
        plane.stride = 2;
        plane.bytes = {static_cast<std::byte>(code & 255), static_cast<std::byte>(code >> 8)};
        plane.digest = provider::digestBytes(plane.bytes);
        alphaFrame.planes.push_back(plane);
    }
    const auto alphaImage =
        bloom::media::video::videoToSceneLinear(alphaFrame, 0, *descriptor.value(), 1, 1, 64);
    check(std::holds_alternative<bloom::render::Rgba32fImage>(alphaImage),
          "codec alpha plane converts");
    const auto alphaPixel = std::get<bloom::render::Rgba32fImage>(alphaImage).pixels().front();
    check(alphaPixel.alpha() == 32768.0F / 65535.0F &&
              std::abs(alphaPixel.red() / alphaPixel.alpha() - 0.270711307F) < 0.000001F,
          "alpha is retained and RGB premultiplied once");
    alphaFrame.colour.range = 2;
    for (unsigned i : {0U, 3U}) {
        alphaFrame.planes[i].bytes = {std::byte{255}, std::byte{255}};
        alphaFrame.planes[i].digest = provider::digestBytes(alphaFrame.planes[i].bytes);
    }
    const auto fullImage =
        bloom::media::video::videoToSceneLinear(alphaFrame, 0, *descriptor.value(), 1, 1, 64);
    check(std::holds_alternative<bloom::render::Rgba32fImage>(fullImage),
          "full-range 16-bit YUV converts");
    const auto fullPixel = std::get<bloom::render::Rgba32fImage>(fullImage).pixels().front();
    check(std::abs(fullPixel.red() - 1.0F) < 0.000001F && fullPixel.red() == fullPixel.green() &&
              fullPixel.green() == fullPixel.blue(),
          "16-bit full range retains neutral white and chroma midpoint");
    for (const provider::ColourTags tags :
         {provider::ColourTags{1, 16, 1, 1}, provider::ColourTags{1, 18, 1, 1},
          provider::ColourTags{9, 1, 9, 1}}) {
        auto hdr = *next;
        hdr.colour = tags;
        for (unsigned overrideTransfer : {0U, 1U, 2U, 3U}) {
            const auto convertedHdr = bloom::media::video::videoToSceneLinear(
                hdr, overrideTransfer, *descriptor.value(), 1, 1, std::size_t{64} * 48 * 16);
            const auto* refused = std::get_if<provider::Unavailable>(&convertedHdr);
            check(refused && refused->reason == provider::Error::Unavailable,
                  "Rec.2020, HLG and PQ remain typed unavailable under transfer overrides");
        }
    }
    const auto cancelled = session.frame(probe, 0, 12, 0, &cache, [] { return true; });
    check(std::get<provider::Unavailable>(cancelled).reason == provider::Error::Cancelled,
          "decode cancellation");
    const auto copy = directory / "changed.mp4";
    std::filesystem::copy_file(directory / "numbered-h264.mp4", copy,
                               std::filesystem::copy_options::overwrite_existing);
    video::VideoDecodeSession changed(copy);
    cache.setByteBudget(4608);
    const auto loaded = changed.frame(probe, 0, 0, 0, &cache);
    check(std::holds_alternative<std::shared_ptr<const provider::FrameProduct>>(loaded),
          "warm source cache");
    {
        std::ofstream file(copy, std::ios::app | std::ios::binary);
        file << "changed";
    }
    const auto stale = changed.frame(probe, 0, 0, 0, &cache);
    check(std::get<provider::Unavailable>(stale).reason == provider::Error::SourceChanged,
          "changed bytes invalidate cached frame");
}
// Issue #366: retained preview decode keeps its position inside a GOP, queues rather than
// refusing concurrent callers, and routes hardware only with explicit provenance.
using Frame = std::shared_ptr<const bloom::media::provider::FrameProduct>;
unsigned frameLuma(const Frame& frame) {
    return std::to_integer<unsigned>(frame->planes[0].bytes[0]);
}
void retainedRouting(const std::filesystem::path& directory) {
    using namespace bloom::media;
    // Exercise the hardware route wherever a render node exists, whatever the core count.
    ::setenv("BLOOM_VIDEO_HARDWARE_DECODE", "1", 1);
    const video::SourceRevisionAccess preview{video::SourceRevisionPolicy::Retained, true};
    video::VideoDecodeSession session(directory / "numbered-h264.mp4");
    const auto probe = std::get<provider::ProbeResult>(session.probe());
    // GOP 12: 13 -> 15 -> 23 decode forward without a seek, 30 crosses the keyframe at 24,
    // 14 is backward. Every result is the exact numbered frame.
    for (const auto n : {13U, 15U, 23U, 30U, 14U, 16U, 47U, 0U}) {
        const auto result = session.frame(probe, 0, n, 0, nullptr, {}, {}, {}, {}, preview);
        check(std::holds_alternative<Frame>(result), "retained forward/backward decode");
        check(frameLuma(std::get<Frame>(result)) == 16 + n * 3,
              "forward decode within the GOP returns the exact frame");
    }
    // Concurrent callers for different frames wait for the decoder instead of failing Busy.
    std::array<provider::Result<Frame>, 4> results{provider::Unavailable{}, provider::Unavailable{},
                                                   provider::Unavailable{},
                                                   provider::Unavailable{}};
    {
        std::vector<std::jthread> threads;
        for (unsigned i = 0; i < results.size(); ++i)
            threads.emplace_back([&, i] {
                results[i] =
                    session.frame(probe, 0, 20 + i * 5, 0, nullptr, {}, {}, {}, {}, preview);
            });
    }
    for (unsigned i = 0; i < results.size(); ++i) {
        check(std::holds_alternative<Frame>(results[i]), "concurrent requests are queued");
        check(frameLuma(std::get<Frame>(results[i])) == 16 + (20 + i * 5) * 3,
              "queued request returns its own frame");
    }
    // Hardware is preview-only and never silent: either a discovered VA-API decoder returns the
    // software-identical frame, or the session reports an explicit typed software fallback.
    const auto reference =
        std::get<Frame>(session.frame(probe, 0, 26, 0, nullptr, {}, {}, {}, {}, preview));
    video::FrameDecodeProvenance provenance;
    video::VideoDecodeSession accelerated(directory / "numbered-h264.mp4");
    const auto routed =
        accelerated.frame(probe, 0, 26, 0, nullptr, {}, {}, {}, {}, preview, {true, &provenance});
    check(std::holds_alternative<Frame>(routed), "hardware preference always yields a frame");
    check(*std::get<Frame>(routed) == *reference,
          "hardware and software decoded pixels are identical");
    if (provenance.implementation == provider::Implementation::Hardware)
        check(provenance.description == "vaapi" && !provenance.fallback, "VA-API provenance");
    else if (video::VideoDecodeSession::hardwareDecodePossible())
        check(provenance.fallback && provenance.description.starts_with("software (fallback: ") &&
                  provenance.description.size() > 21,
              "hardware refusal is an explicit typed software fallback");
    else
        check(provenance.description == "software" && !provenance.fallback,
              "no render node: software without a hardware attempt");
    std::cout << "hardware route on this host: " << provenance.description << '\n';
    // Final rendering (revalidated revisions) never takes the hardware route.
    video::FrameDecodeProvenance final;
    const auto rendered =
        accelerated.frame(probe, 0, 26, 0, nullptr, {}, {}, {}, {}, {}, {true, &final});
    check(std::holds_alternative<Frame>(rendered) &&
              final.implementation == provider::Implementation::Software,
          "final decode stays on the software reference");
    // ProRes has no hardware declaration, so a preference is not a fallback.
    video::VideoDecodeSession prores(directory / "numbered-prores.mov");
    const auto proresProbe = std::get<provider::ProbeResult>(prores.probe());
    video::FrameDecodeProvenance intra;
    check(std::holds_alternative<Frame>(prores.frame(proresProbe, 0, 5, 0, nullptr, {}, {}, {}, {},
                                                     preview, {true, &intra})) &&
              intra.description == "software" && !intra.fallback,
          "codecs without hardware declarations decode in software");
    // Decoded-video retention defaults scale with physical memory inside fixed bounds.
    check(video::automaticDecodedVideoCacheBytes(0) == std::size_t{256} << 20U &&
              video::automaticDecodedVideoCacheBytes(std::size_t{16} << 30U) == std::size_t{1}
                                                                                    << 30U &&
              video::automaticDecodedVideoCacheBytes(std::size_t{256} << 30U) == std::size_t{4}
                                                                                     << 30U,
          "automatic decoded-video cache share");
    check(video::estimatedDecodeWorkerBytes(1920, 1080) <
                  video::estimatedDecodeWorkerBytes(3840, 2160) &&
              video::estimatedDecodeWorkerBytes(16384, 16384) == std::size_t{2} << 30U,
          "worker estimate scales with extent and is bounded by the worker address space");
}
// Issue #365: matrix selection, explicit interpretation of untagged streams, filtered 4:2:0
// chroma, and tag plumbing from the generated colour fixtures.
bloom::media::provider::FrameProduct yuv420(std::array<unsigned, 2> cb, std::array<unsigned, 2> cr,
                                            bloom::media::provider::ColourTags tags) {
    using namespace bloom::media::provider;
    FrameProduct frame;
    frame.format = PixelFormat::Yuv420p8;
    frame.colour = tags;
    const auto plane = [](std::uint32_t w, std::uint32_t h, std::vector<unsigned> values) {
        CpuPlane p;
        p.width = w;
        p.height = h;
        p.stride = w;
        for (const auto v : values)
            p.bytes.push_back(static_cast<std::byte>(v));
        p.digest = digestBytes(p.bytes);
        return p;
    };
    frame.planes.push_back(plane(4, 2, {126, 126, 126, 126, 126, 126, 126, 126}));
    frame.planes.push_back(plane(2, 1, {cb[0], cb[1]}));
    frame.planes.push_back(plane(2, 1, {cr[0], cr[1]}));
    return frame;
}
void colourInterpretation(const std::filesystem::path& directory) {
    using namespace bloom::media;
    const auto window = bloom::render::ImageWindow::create(0, 0, 4, 2);
    const auto descriptor = bloom::render::Rgba32fImageDescriptor::create(
        *window.value(), *window.value(), bloom::core::PixelAspectRatio::square());
    const auto convert = [&](const provider::FrameProduct& frame, std::string_view id) {
        return video::videoToInputColorSpace(frame, 0, id, *descriptor.value(), 1, 1,
                                             std::size_t{4} * 2 * 16);
    };
    // BT.2020 and BT.709 matrices differ on the same chroma; the explicit-space path accepts both.
    const auto saturated709 = convert(yuv420({200, 200}, {60, 60}, {1, 1, 1, 1}), "space");
    const auto saturated2020 = convert(yuv420({200, 200}, {60, 60}, {9, 16, 9, 1}), "space");
    check(std::holds_alternative<bloom::render::Rgba32fImage>(saturated709) &&
              std::holds_alternative<bloom::render::Rgba32fImage>(saturated2020),
          "BT.709 and BT.2020 matrices convert through a config-managed space");
    const auto red2020 = std::get<bloom::render::Rgba32fImage>(saturated2020).pixels()[0].red();
    const auto red709 = std::get<bloom::render::Rgba32fImage>(saturated709).pixels()[0].red();
    const auto expected2020 =
        (126.0F - 16.0F) / 219.0F + 2.0F * (1.0F - 0.2627F) * (-68.0F / 224.0F);
    check(std::abs(red2020 - expected2020) < 1e-5F && std::abs(red709 - red2020) > 1e-3F,
          "BT.2020 non-constant-luminance matrix reference value");
    // Untagged: refused on the unmanaged path with named tags, accepted with an explicit space.
    const auto untagged = yuv420({128, 128}, {128, 128}, {2, 2, 2, 0});
    const auto refused =
        video::videoToSceneLinear(untagged, 0, *descriptor.value(), 1, 1, std::size_t{4} * 2 * 16);
    check(std::holds_alternative<provider::Unavailable>(refused) &&
              std::get<provider::Unavailable>(refused).detail.find("matrix coefficients") !=
                  std::string::npos,
          "an untagged frame names its missing tags");
    const auto interpreted = convert(untagged, "Rec.2100-PQ - Display");
    check(std::holds_alternative<bloom::render::Rgba32fImage>(interpreted) &&
              std::abs(std::get<bloom::render::Rgba32fImage>(interpreted).pixels()[0].red() -
                       110.0F / 219.0F) < 1e-5F,
          "an explicit space interprets an untagged frame as limited range");
    const auto constant = convert(yuv420({128, 128}, {128, 128}, {9, 1, 10, 1}), "space");
    check(std::holds_alternative<provider::Unavailable>(constant), "BT.2020 CL is refused");
    // Filtered chroma: odd columns interpolate between left-sited chroma samples.
    const auto ramp = convert(yuv420({100, 200}, {128, 128}, {1, 1, 1, 1}), "space");
    const auto& pixels = std::get<bloom::render::Rgba32fImage>(ramp).pixels();
    const auto blue = [&](unsigned x) { return pixels[x].blue(); };
    check(std::abs(blue(1) - (blue(0) + blue(2)) / 2.0F) < 1e-5F && blue(0) < blue(1) &&
              blue(1) < blue(2) && std::abs(blue(3) - blue(2)) < 1e-6F,
          "4:2:0 chroma is reconstructed with a filter, not nearest neighbour");
    // The decoder reports the generated fixtures' tags exactly.
    for (const auto& [name, tags] : std::array<std::pair<const char*, provider::ColourTags>, 3>{
             {{"untagged-h264.mp4", {2, 2, 2, 0}},
              {"pq2020-h264.mp4", {9, 16, 9, 1}},
              {"hlg2020-h264.mp4", {9, 18, 9, 1}}}}) {
        video::VideoDecodeSession session(directory / name);
        const auto probe = std::get<provider::ProbeResult>(session.probe());
        const auto decoded = session.frame(probe, 0, 3, 0, nullptr);
        check(std::holds_alternative<Frame>(decoded) && std::get<Frame>(decoded)->colour == tags,
              "colour fixtures carry their tags through decode");
    }
}
// Issue #367: variable cadence, display orientation, interlace and RGB-coded streams.
unsigned high(const bloom::media::provider::CpuPlane& plane, std::uint32_t x, std::uint32_t y) {
    return std::to_integer<unsigned>(
        plane.bytes[static_cast<std::size_t>(y) * plane.stride + std::size_t{x} * 2U + 1U]);
}
void timingAndLayout(const std::filesystem::path& directory) {
    using namespace bloom::media;
    const video::SourceRevisionAccess preview{video::SourceRevisionPolicy::Retained, true};
    {
        video::VideoDecodeSession session(directory / "vfr-h264.mp4");
        const auto probe = std::get<provider::ProbeResult>(session.probe());
        check(probe.streams[0].frameCount == 21,
              "a variable-cadence stream exposes its nominal 24/1 slots");
        // Packets at 0,1,2,4,5,8,9,10,14,15,16,20: each slot shows the last frame at or before it.
        for (const auto& [slot, shown] : std::array<std::pair<unsigned, unsigned>, 8>{
                 {{0, 0}, {3, 2}, {4, 4}, {7, 5}, {13, 10}, {16, 16}, {19, 16}, {20, 20}}}) {
            const auto decoded = session.frame(probe, 0, slot, 0, nullptr, {}, {}, {}, {}, preview);
            check(std::holds_alternative<Frame>(decoded) &&
                      frameLuma(std::get<Frame>(decoded)) == 16 + shown * 3,
                  "variable cadence maps a slot to the frame presented at its time");
        }
        check(std::holds_alternative<provider::Unavailable>(
                  session.frame(probe, 0, 21, 0, nullptr, {}, {}, {}, {}, preview)),
              "a slot past the duration is refused");
    }
    {
        video::VideoDecodeSession session(directory / "rotated-h264.mp4");
        const auto probe = std::get<provider::ProbeResult>(session.probe());
        check(probe.streams[0].width == 48 && probe.streams[0].height == 64,
              "a 90-degree display matrix reports display dimensions");
        const auto decoded = session.frame(probe, 0, 7, 0, nullptr);
        check(std::holds_alternative<Frame>(decoded), "rotated decode");
        const auto& frame = *std::get<Frame>(decoded);
        check(frame.format == provider::PixelFormat::Yuva444p16 && frame.planes[0].width == 48 &&
                  frame.planes[0].height == 64,
              "the rotated product is display-oriented");
        // Coded (x, y) is displayed at (47 - y, x) after a clockwise quarter turn.
        for (const auto& [x, y] :
             std::array<std::pair<unsigned, unsigned>, 3>{{{0, 0}, {2, 40}, {20, 13}}}) {
            const auto coded = x == 20 && y == 10 ? 220U : 16U + 7U * 3U;
            check(high(frame.planes[0], 47 - y, x) == coded,
                  "pixels land at their display-oriented position");
        }
    }
    {
        video::VideoDecodeSession session(directory / "anamorphic-h264.mp4");
        const auto probe = std::get<provider::ProbeResult>(session.probe());
        check(probe.streams[0].width == 128 && probe.streams[0].height == 48,
              "a 2:1 sample aspect ratio reports square-pixel display dimensions");
        const auto decoded = session.frame(probe, 0, 7, 0, nullptr);
        check(std::holds_alternative<Frame>(decoded), "anamorphic decode");
        const auto& plane = std::get<Frame>(decoded)->planes[0];
        check(plane.width == 128 && plane.height == 48 && high(plane, 4, 30) == 16 + 7 * 3 &&
                  high(plane, 46, 10) == 220,
              "anamorphic pixels are resampled to square pixels at decode");
    }
    {
        video::VideoDecodeSession session(directory / "interlaced-prores.mov");
        const auto probe = std::get<provider::ProbeResult>(session.probe());
        const auto decoded = session.frame(probe, 0, 3, 0, nullptr);
        check(std::holds_alternative<Frame>(decoded), "interlaced decode is no longer refused");
        const auto& plane = std::get<Frame>(decoded)->planes[0];
        const auto background = high(plane, 0, 2);
        check(std::abs(static_cast<int>(background) - static_cast<int>(16 + 3 * 3)) <= 2 &&
                  std::abs(static_cast<int>(high(plane, 0, 1)) - static_cast<int>(background)) <= 1,
              "the dominant (top) field is kept and the other field rebuilt from it");
    }
    {
        video::VideoDecodeSession session(directory / "rgb-tiff.mov");
        const auto probe = std::get<provider::ProbeResult>(session.probe());
        const auto decoded = session.frame(probe, 0, 2, 0, nullptr);
        check(std::holds_alternative<Frame>(decoded), "RGB-coded decode is no longer refused");
        const auto& frame = *std::get<Frame>(decoded);
        check(frame.format == provider::PixelFormat::Rgba16 && frame.colour.matrix == 0,
              "RGB-coded video keeps RGB with its identity matrix tag");
        const auto window = bloom::render::ImageWindow::create(0, 0, 64, 48);
        const auto descriptor = bloom::render::Rgba32fImageDescriptor::create(
            *window.value(), *window.value(), bloom::core::PixelAspectRatio::square());
        const auto converted = video::videoToInputColorSpace(frame, 0, "space", *descriptor.value(),
                                                             1, 1, std::size_t{64} * 48 * 16);
        check(std::holds_alternative<bloom::render::Rgba32fImage>(converted), "RGB converts");
        const auto pixel = std::get<bloom::render::Rgba32fImage>(converted).pixels()[0];
        check(std::abs(pixel.red() - (16.0F + 6.0F) * 256.0F / 65535.0F) < 1e-6F &&
                  pixel.green() == 0.0F && std::abs(pixel.blue() - 0.5F) < 1e-4F &&
                  pixel.alpha() == 1.0F,
              "RGB samples pass through without a YUV matrix");
    }
}
} // namespace
int main(int argc, char** argv) {
    try {
        check(argc == 2, "fixture directory");
        run(argv[1]);
        retainedRouting(argv[1]);
        colourInterpretation(argv[1]);
        timingAndLayout(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
