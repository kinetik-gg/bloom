#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/media/video/colour.hpp>
#include <bloom/media/video/session.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void run(const std::filesystem::path& directory) {
    using namespace bloom::media;
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
} // namespace
int main(int argc, char** argv) {
    try {
        check(argc == 2, "fixture directory");
        run(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
