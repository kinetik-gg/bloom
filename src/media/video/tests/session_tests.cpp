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
} // namespace
int main(int argc, char** argv) {
    try {
        check(argc == 2, "fixture directory");
        run(argv[1]);
        retainedRouting(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
