#pragma once

// Issue #381 / ADR 0032: compares Bloom's default display of ordinary SDR video with the
// player-equivalent FFmpeg decode of the same frame. The generated fixtures (see the media
// worker's ffmpeg_provider_fixtures.ipp) are twelve uniform 16x16 colour patches coded as
// limited-range Y'CbCr, and an RGB24 file holding FFmpeg/swscale's full-range R'G'B' of the first
// frame. Bloom's working-space frame is shown through the config's default display view (the
// same CPU display processor the Viewer and PNG export use) and quantized like the PNG packing
// rule. Only patch interiors are compared, so the 4:2:0 chroma filters (Bloom's left-sited
// bilinear, swscale's own) never meet at a patch edge.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/render/image_types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::test::video_player_reference {

struct Fixture final {
    std::string_view clip;
    std::string_view reference;
    std::string_view label;
};
inline constexpr std::array kFixtures{
    Fixture{"bars709-h264.mp4", "bars709-h264.rgb24", "BT.709-tagged H.264"},
    Fixture{"bars-untagged-h264.mp4", "bars-untagged-h264.rgb24", "untagged H.264"}};
inline constexpr std::uint32_t kWidth = 64, kHeight = 48, kPatch = 16, kMargin = 3;
// The stated tolerance: every compared 8-bit code within 2 of FFmpeg's, and a mean within 0.75.
// The previous Auto default (encoded R'G'B' read as linear light) missed by up to 33 codes on
// the 75% bars alone.
inline constexpr unsigned kMaximumCodeDifference = 2;
inline constexpr double kMaximumMeanDifference = 0.75;

struct Comparison final {
    bool valid = false;
    unsigned maximum = 0;
    double mean = 0.0;
    std::size_t compared = 0;
    std::string worst;
    [[nodiscard]] bool passes() const noexcept {
        return valid && compared > 0 && maximum <= kMaximumCodeDifference &&
               mean <= kMaximumMeanDifference;
    }
    [[nodiscard]] std::string summary() const {
        return "max " + std::to_string(maximum) + " codes, mean " + std::to_string(mean) +
               " over " + std::to_string(compared) + " samples" +
               (worst.empty() ? std::string{} : "; worst " + worst);
    }
};

[[nodiscard]] inline std::vector<std::uint8_t> readReference(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// `pixels` is a kWidth x kHeight premultiplied working-space frame (opaque video).
[[nodiscard]] inline Comparison compare(const color::ResolvedBloomNeutralConfig& config,
                                        const std::span<const render::Rgba32f> pixels,
                                        const std::filesystem::path& reference) {
    Comparison result;
    const auto expected = readReference(reference);
    const auto display = color::buildBloomNeutralCpuDisplayProcessor(config);
    if (expected.size() != std::size_t{kWidth} * kHeight * 3U ||
        pixels.size() != std::size_t{kWidth} * kHeight || display.handle() == nullptr)
        return result;
    double total = 0.0;
    for (std::uint32_t y = 0; y < kHeight; ++y)
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const auto px = x % kPatch, py = y % kPatch;
            if (px < kMargin || px >= kPatch - kMargin || py < kMargin || py >= kPatch - kMargin)
                continue;
            const auto& pixel = pixels[std::size_t{y} * kWidth + x];
            if (pixel.alpha() <= 0.0F)
                return result;
            const auto alpha = static_cast<double>(pixel.alpha());
            const auto shown = display.handle()->referenceToDisplay(
                {static_cast<double>(pixel.red()) / alpha,
                 static_cast<double>(pixel.green()) / alpha,
                 static_cast<double>(pixel.blue()) / alpha, 1.0});
            if (!shown)
                return result;
            const std::array<double, 3> channels{shown->red, shown->green, shown->blue};
            for (std::size_t c = 0; c < 3; ++c) {
                // frame-output.md's PNG packing: clamp, scale by 255, floor(value + 0.5).
                const auto code = static_cast<unsigned>(
                    std::floor(std::clamp(channels[c], 0.0, 1.0) * 255.0 + 0.5));
                const unsigned want = expected[(std::size_t{y} * kWidth + x) * 3U + c];
                const auto difference = code > want ? code - want : want - code;
                total += difference;
                ++result.compared;
                if (difference > result.maximum) {
                    result.maximum = difference;
                    result.worst = "patch " + std::to_string((y / kPatch) * (kWidth / kPatch) +
                                                             x / kPatch) +
                                   " channel " + std::to_string(c) + ": Bloom " +
                                   std::to_string(code) + ", FFmpeg " + std::to_string(want);
                }
            }
        }
    result.valid = true;
    result.mean = result.compared == 0 ? 0.0 : total / static_cast<double>(result.compared);
    return result;
}

} // namespace bloom::test::video_player_reference
