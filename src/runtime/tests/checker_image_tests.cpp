#include "checker_image.hpp"

#include <bloom/render/image_types.hpp>

#include <cmath>
#include <cstdio>
#include <limits>

int main() {
    using bloom::runtime::detail::CheckerGeometry;
    using bloom::runtime::detail::checkerPixel;
    using bloom::runtime::detail::checkerSampleCounts;
    int failures = 0;
    const auto expect = [&](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "%s\n", message);
            ++failures;
        }
    };
    const CheckerGeometry full{4, 4, 1, 1, 4, 4, 4, 4};
    expect(full.valid(), "full geometry should be valid");
    expect(checkerSampleCounts(full, 0, 0).a == 16 &&
               checkerSampleCounts(full, 1, 0).b == 16 &&
               checkerSampleCounts(full, 1, 1).a == 16,
           "unit cells must alternate without a seam");
    const CheckerGeometry half{4, 4, 1, 1, 4, 4, 2, 2};
    const auto halfPixel = checkerSampleCounts(half, 0, 0);
    expect(halfPixel.a == 8 && halfPixel.b == 8,
           "proxy samples must cover the same author-space cells");
    const CheckerGeometry clipped{2, 2, 1, 1, 4, 4, 4, 4};
    const auto outside = checkerSampleCounts(clipped, 2, 0);
    expect(outside.a == 0 && outside.b == 0,
           "samples outside authored data extent must be transparent");
    // This is the exact unsigned 32-bit shader boundary, including the last subpixel.
    const CheckerGeometry maximum{16384, 16384, 1, 1, 16384, 16384, 16384, 16384};
    const auto edge = checkerSampleCounts(maximum, 16383, 16383);
    expect(edge.a + edge.b == 16, "maximum device coordinate must not overflow");
    const auto a = bloom::render::Rgba32f::fromPremultiplied(-4.0F, 0.0F, 0.0F, 1.0F);
    const auto b = bloom::render::Rgba32f::fromPremultiplied(8.0F, 0.0F, 0.0F, 1.0F);
    expect(a && b, "signed HDR colors must remain valid");
    if (a && b) {
        const auto pixel = checkerPixel(*a.value(), *b.value(), halfPixel);
        expect(pixel && std::abs(pixel.value()->red() - 2.0F) < 1e-6F &&
                   pixel.value()->alpha() == 1.0F,
               "checker averaging must preserve signed HDR and premultiplied alpha");
    }
    const auto tinyAlpha = bloom::render::Rgba32f::fromPremultiplied(
        1.0F, 0.0F, 0.0F, std::numeric_limits<float>::denorm_min());
    expect(static_cast<bool>(tinyAlpha), "subnormal coverage is a valid process pixel");
    if (tinyAlpha) {
        const auto pixel = checkerPixel(*tinyAlpha.value(), bloom::render::Rgba32f::transparent(),
                                        halfPixel);
        expect(pixel && *pixel.value() == bloom::render::Rgba32f::transparent(),
               "coverage underflow canonicalizes the checker pixel to transparent black");
    }
    return failures == 0 ? 0 : 1;
}
