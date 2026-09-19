// CPU oracle and shader-pin tests for the resident present-image sampler. No GPU, window, or Qt is
// needed here: the corrected fragment composition (display-space checkerboard, destination/source
// mapping, channel remap per texel BEFORE the filter, premultiplied bilinear filtering of the
// straight RGBA8 source, premultiplied overlay) is reproduced on the CPU and checked against
// hand-computed pixels, and the embedded SPIR-V is hashed against the pinned manifest digests. The
// independent QPainter SmoothPixmapTransform oracle lives in the test-only UI file
// (gpu_present_image_qpainter_oracle_tests.cpp) because only Qt can produce it.

#include <bloom/core/sha256.hpp>
#include <bloom/render/gpu_present_image.hpp>

#include "../vulkan/shaders/viewer_present_spirv.inc"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <source_location>
#include <span>
#include <string>
#include <string_view>

namespace {

using bloom::render::GpuPresentBackground;
using bloom::render::GpuPresentChannel;
using bloom::render::GpuPresentColor;
using bloom::render::GpuPresentImageParams;
using bloom::render::GpuPresentOverlay;
using bloom::render::GpuPresentRect;
using bloom::render::GpuPresentSourceWindow;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] bool near(const float a, const float b) { return std::fabs(a - b) < 1.0e-4F; }

struct Pixel final {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
};

// 2x2 straight-RGBA8 source with clamped texel access.
struct SourceImage final {
    std::array<Pixel, 4> texels{};
    [[nodiscard]] Pixel texel(const int x, const int y) const {
        const int cx = x < 0 ? 0 : (x > 1 ? 1 : x);
        const int cy = y < 0 ? 0 : (y > 1 ? 1 : y);
        return texels[static_cast<std::size_t>(cy) * 2U + static_cast<std::size_t>(cx)];
    }
};

// The viewer channel remap is applied to each texel BEFORE the filter (opaque modes only; RGBA
// keeps its straight colour and alpha).
[[nodiscard]] Pixel remap(const Pixel source, const GpuPresentChannel channel) {
    switch (channel) {
    case GpuPresentChannel::Rgb:
        return Pixel{source.r, source.g, source.b, 1.0F};
    case GpuPresentChannel::Red:
        return Pixel{source.r, source.r, source.r, 1.0F};
    case GpuPresentChannel::Green:
        return Pixel{source.g, source.g, source.g, 1.0F};
    case GpuPresentChannel::Blue:
        return Pixel{source.b, source.b, source.b, 1.0F};
    case GpuPresentChannel::Alpha:
        return Pixel{source.a, source.a, source.a, 1.0F};
    case GpuPresentChannel::Rgba:
    default:
        return source;
    }
}

// Straight tap -> premultiplied, after remap. Filtering happens in premultiplied space, matching
// QPainter's SmoothPixmapTransform on a straight RGBA8888 image.
[[nodiscard]] Pixel premultiplied(const Pixel tap) {
    return Pixel{tap.r * tap.a, tap.g * tap.a, tap.b * tap.a, tap.a};
}

[[nodiscard]] Pixel samplePremultiplied(const SourceImage& source, const double x, const double y,
                                        const GpuPresentChannel channel) {
    const double baseX = x - 0.5;
    const double baseY = y - 0.5;
    const int lowerX = static_cast<int>(std::floor(baseX));
    const int lowerY = static_cast<int>(std::floor(baseY));
    const float fx = static_cast<float>(baseX - static_cast<double>(lowerX));
    const float fy = static_cast<float>(baseY - static_cast<double>(lowerY));
    const auto tap = [&](const int tx, const int ty) {
        return premultiplied(remap(source.texel(tx, ty), channel));
    };
    const Pixel top = [&] {
        const Pixel a = tap(lowerX, lowerY);
        const Pixel b = tap(lowerX + 1, lowerY);
        return Pixel{a.r + (b.r - a.r) * fx, a.g + (b.g - a.g) * fx, a.b + (b.b - a.b) * fx,
                     a.a + (b.a - a.a) * fx};
    }();
    const Pixel bottom = [&] {
        const Pixel a = tap(lowerX, lowerY + 1);
        const Pixel b = tap(lowerX + 1, lowerY + 1);
        return Pixel{a.r + (b.r - a.r) * fx, a.g + (b.g - a.g) * fx, a.b + (b.b - a.b) * fx,
                     a.a + (b.a - a.a) * fx};
    }();
    return Pixel{top.r + (bottom.r - top.r) * fy, top.g + (bottom.g - top.g) * fy,
                 top.b + (bottom.b - top.b) * fy, top.a + (bottom.a - top.a) * fy};
}

[[nodiscard]] Pixel background(const GpuPresentImageParams& params, const float x, const float y) {
    if (params.background == GpuPresentBackground::Black) {
        return Pixel{0.0F, 0.0F, 0.0F, 1.0F};
    }
    if (params.background == GpuPresentBackground::White) {
        return Pixel{1.0F, 1.0F, 1.0F, 1.0F};
    }
    if (params.background == GpuPresentBackground::Checkerboard) {
        const float tile = params.checkerTilePixels > 0.0F ? params.checkerTilePixels : 1.0F;
        const int cx = static_cast<int>(std::floor((x - params.checkerOriginX) / tile));
        const int cy = static_cast<int>(std::floor((y - params.checkerOriginY) / tile));
        const bool raised = ((cx + cy) & 1) == 0;
        const GpuPresentColor color = raised ? params.checkerColorB : params.checkerColorA;
        return Pixel{color.red, color.green, color.blue, color.alpha};
    }
    return Pixel{params.backgroundColor.red, params.backgroundColor.green,
                 params.backgroundColor.blue, params.backgroundColor.alpha};
}

[[nodiscard]] Pixel compose(const GpuPresentImageParams& params, const SourceImage& source,
                            const Pixel overlay, const bool hasOverlay, const float x,
                            const float y) {
    const Pixel bg = background(params, x, y);
    Pixel color = bg;
    const GpuPresentRect& d = params.destination;
    const bool inside = d.width > 0.0F && d.height > 0.0F && x >= d.x && x < d.x + d.width &&
                        y >= d.y && y < d.y + d.height;
    if (inside) {
        const double localX =
            (static_cast<double>(x) - static_cast<double>(d.x)) / static_cast<double>(d.width);
        const double localY =
            (static_cast<double>(y) - static_cast<double>(d.y)) / static_cast<double>(d.height);
        const double sourceX = params.source.x + localX * params.source.width;
        const double sourceY = params.source.y + localY * params.source.height;
        const Pixel texel = samplePremultiplied(source, sourceX, sourceY, params.channel);
        color.r = texel.r + bg.r * (1.0F - texel.a);
        color.g = texel.g + bg.g * (1.0F - texel.a);
        color.b = texel.b + bg.b * (1.0F - texel.a);
        color.a = texel.a + bg.a * (1.0F - texel.a);
    }
    if (hasOverlay) {
        color.r = overlay.r + color.r * (1.0F - overlay.a);
        color.g = overlay.g + color.g * (1.0F - overlay.a);
        color.b = overlay.b + color.b * (1.0F - overlay.a);
        color.a = overlay.a + color.a * (1.0F - overlay.a);
    }
    return color;
}

[[nodiscard]] bool digestMatches(const std::span<const std::uint32_t> words,
                                 const std::string_view expectedHex) {
    const auto digest = bloom::core::Sha256Hasher::hash(std::as_bytes(words));
    if (!digest.has_value()) {
        return false;
    }
    const auto hex = digest->toLowercaseHex();
    return std::string_view(hex.data(), hex.size()) == expectedHex;
}

void shaderPins(Expectations& expectations) {
    expectations.expect(
        digestMatches(
            std::span<const std::uint32_t>(bloom::render::vulkan_detail::kViewerPresentVertSpirv),
            "02530de5a4b2abf9944d5b9ee40c610069c97b20fdbbfdc27e9f99f2f9aa415a"),
        "the embedded vertex SPIR-V matches the pinned manifest digest");
    expectations.expect(
        digestMatches(
            std::span<const std::uint32_t>(bloom::render::vulkan_detail::kViewerPresentFragSpirv),
            "8a2120e1c7580034e85c1954c8cce9702ceb4344868d5458e9f9980a08b15105"),
        "the embedded fragment SPIR-V matches the corrected pinned manifest digest");
    expectations.expect(bloom::render::vulkan_detail::kViewerPresentVertWordCount ==
                                bloom::render::vulkan_detail::kViewerPresentVertByteCount / 4U &&
                            bloom::render::vulkan_detail::kViewerPresentFragWordCount ==
                                bloom::render::vulkan_detail::kViewerPresentFragByteCount / 4U,
                        "the pinned byte and word counts are consistent");
}

// Opaque red next to fully transparent blue at a fractional sample position: the premultiplied
// filter yields a half-alpha red (no blue halo), which the straight-RGB-then-multiply bug would
// instead mix toward purple. This is the exact case the independent QPainter oracle pins too.
void premultipliedEdge(Expectations& expectations) {
    GpuPresentImageParams params;
    params.destination = GpuPresentRect{0.0F, 0.0F, 2.0F, 2.0F};
    params.source = GpuPresentSourceWindow{0.0, 0.0, 2.0, 2.0};
    params.background = GpuPresentBackground::Black;
    SourceImage image;
    image.texels[0] = Pixel{1.0F, 0.0F, 0.0F, 1.0F}; // opaque red
    image.texels[1] = Pixel{0.0F, 0.0F, 1.0F, 0.0F}; // transparent blue
    image.texels[2] = Pixel{0.0F, 0.0F, 0.0F, 0.0F};
    image.texels[3] = Pixel{0.0F, 0.0F, 0.0F, 0.0F};
    // Destination pixel centre maps to source x = 1.0 (the exact midpoint of texel 0 and texel 1
    // centres); y = 0.5 keeps the filter horizontal only (the red/blue row, no vertical blend).
    const Pixel mid = compose(params, image, Pixel{}, false, 1.0F, 0.5F);
    expectations.expect(near(mid.r, 0.5F) && near(mid.g, 0.0F) && near(mid.b, 0.0F) &&
                            near(mid.a, 1.0F),
                        "premultiplied filtering keeps a red edge clean (no blue halo)");
    const Pixel straightBug = Pixel{0.5F, 0.0F, 0.5F, 0.5F}; // what straight-then-multiply gives
    expectations.expect(!near(straightBug.b, mid.b),
                        "the corrected filter differs from the straight-then-multiply bug");
}

void channelAndBackground(Expectations& expectations) {
    GpuPresentImageParams params;
    params.destination = GpuPresentRect{0.0F, 0.0F, 2.0F, 2.0F};
    params.source = GpuPresentSourceWindow{0.0, 0.0, 2.0, 2.0};
    params.background = GpuPresentBackground::Checkerboard;
    params.checkerTilePixels = 10.0F;
    params.checkerColorA = GpuPresentColor{0.25F, 0.25F, 0.25F, 1.0F};
    params.checkerColorB = GpuPresentColor{0.75F, 0.75F, 0.75F, 1.0F};
    const Pixel raised = background(params, 0.5F, 0.5F);
    const Pixel base = background(params, 10.5F, 0.5F);
    expectations.expect(near(raised.r, 0.75F) && near(base.r, 0.25F),
                        "checkerboard parity matches the viewer (row+column even is raised)");

    params.background = GpuPresentBackground::Solid;
    params.backgroundColor = GpuPresentColor{0.2F, 0.2F, 0.2F, 1.0F};
    SourceImage image;
    image.texels[0] = Pixel{0.4F, 0.6F, 0.8F, 0.5F};
    const Pixel texel = image.texel(0, 0);
    params.channel = GpuPresentChannel::Rgba;
    Pixel p = compose(params, image, Pixel{}, false, 0.5F, 0.5F);
    expectations.expect(near(p.r, texel.r * texel.a + 0.2F * (1.0F - texel.a)),
                        "RGBA composes straight alpha over the background");
    params.channel = GpuPresentChannel::Rgb;
    p = compose(params, image, Pixel{}, false, 0.5F, 0.5F);
    expectations.expect(near(p.r, 0.4F) && near(p.g, 0.6F) && near(p.b, 0.8F) && near(p.a, 1.0F),
                        "RGB forces opaque and shows the colour");
    params.channel = GpuPresentChannel::Red;
    p = compose(params, image, Pixel{}, false, 0.5F, 0.5F);
    expectations.expect(near(p.r, 0.4F) && near(p.g, 0.4F) && near(p.b, 0.4F),
                        "Red shows the red channel as grey");
    params.channel = GpuPresentChannel::Alpha;
    p = compose(params, image, Pixel{}, false, 0.5F, 0.5F);
    expectations.expect(near(p.r, 0.5F) && near(p.g, 0.5F) && near(p.b, 0.5F),
                        "Alpha shows alpha as luminance");
}

void overlay(Expectations& expectations) {
    GpuPresentImageParams params;
    params.destination = GpuPresentRect{0.0F, 0.0F, 2.0F, 2.0F};
    params.source = GpuPresentSourceWindow{0.0, 0.0, 2.0, 2.0};
    params.background = GpuPresentBackground::Black;
    SourceImage image;
    image.texels[0] = Pixel{0.5F, 0.5F, 0.5F, 1.0F};
    const Pixel premultOverlay{0.3F, 0.1F, 0.2F, 0.4F};
    const Pixel composed = compose(params, image, premultOverlay, true, 0.5F, 0.5F);
    expectations.expect(near(composed.r, 0.3F + 0.5F * 0.6F) &&
                            near(composed.g, 0.1F + 0.5F * 0.6F) &&
                            near(composed.b, 0.2F + 0.5F * 0.6F) && near(composed.a, 1.0F),
                        "premultiplied overlay composites over the composed image");
    GpuPresentOverlay none;
    expectations.expect(none.token == 0 && none.pixels == nullptr,
                        "a default overlay means no upload is requested");
}

} // namespace

int main() {
    Expectations expectations;
    shaderPins(expectations);
    premultipliedEdge(expectations);
    channelAndBackground(expectations);
    overlay(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
