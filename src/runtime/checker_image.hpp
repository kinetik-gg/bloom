#pragma once

#include <bloom/render/image_types.hpp>

#include <cstdint>

namespace bloom::runtime::detail {

// Integer-rational subpixel coordinates are shared with CheckerV1's resident shader. With every
// dimension at most 16384, (8*x+7)*compositionWidth fits unsigned 32-bit on the GPU. The CPU
// uses uint64 for the same expression. No GPU Float64 capability or frequency-dependent host
// geometry is needed; each output pixel costs exactly sixteen integer sample classifications.
inline constexpr std::uint32_t kCheckerMaximumNativeDimension = 16384;

struct CheckerGeometry final {
    std::uint32_t authorWidth = 0, authorHeight = 0;
    std::uint32_t cellWidth = 0, cellHeight = 0;
    std::uint32_t compositionWidth = 0, compositionHeight = 0;
    std::uint32_t proxyWidth = 0, proxyHeight = 0;

    [[nodiscard]] bool valid() const noexcept {
        for (const auto value : {authorWidth, authorHeight, cellWidth, cellHeight,
                                 compositionWidth, compositionHeight, proxyWidth, proxyHeight})
            if (value == 0 || value > 16777216)
                return false;
        return true;
    }
};

struct CheckerSampleCounts final {
    std::uint32_t a = 0, b = 0;
};

[[nodiscard]] inline CheckerSampleCounts checkerSampleCounts(const CheckerGeometry& g,
                                                            const std::uint32_t x,
                                                            const std::uint32_t y) noexcept {
    CheckerSampleCounts result;
    for (std::uint32_t sy = 0; sy < 4; ++sy) {
        const auto yNumerator = (8ULL * y + 2ULL * sy + 1ULL) * g.compositionHeight;
        const auto yDenominator = 8ULL * g.proxyHeight;
        if (yNumerator >= yDenominator * g.authorHeight)
            continue;
        const auto cellY = yNumerator / (yDenominator * g.cellHeight);
        for (std::uint32_t sx = 0; sx < 4; ++sx) {
            const auto xNumerator = (8ULL * x + 2ULL * sx + 1ULL) * g.compositionWidth;
            const auto xDenominator = 8ULL * g.proxyWidth;
            if (xNumerator >= xDenominator * g.authorWidth)
                continue;
            const auto cellX = xNumerator / (xDenominator * g.cellWidth);
            if (((cellX + cellY) & 1ULL) != 0)
                ++result.b;
            else
                ++result.a;
        }
    }
    return result;
}

[[nodiscard]] inline render::ImageResult<render::Rgba32f>
checkerPixel(const render::Rgba32f a, const render::Rgba32f b,
             const CheckerSampleCounts samples) noexcept {
    const auto wa = static_cast<float>(samples.a) * (1.0F / 16.0F);
    const auto wb = static_cast<float>(samples.b) * (1.0F / 16.0F);
    return render::Rgba32f::fromPremultiplied(a.red() * wa + b.red() * wb,
                                              a.green() * wa + b.green() * wb,
                                              a.blue() * wa + b.blue() * wb,
                                              a.alpha() * wa + b.alpha() * wb);
}

} // namespace bloom::runtime::detail
