#pragma once

#include <bloom/core/color.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/render/image_types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bloom::runtime::detail {

struct ResolvedGradient final {
    core::Color4d first;
    core::Color4d second;
    document::Vec2d start;
    document::Vec2d end;
    std::int64_t repeat = 0;
    bool radial = false;
    double aspect = 1.0;
};

// Samples at the full-resolution pixel centre. Straight scene-linear colour and independent alpha
// interpolate first; the final process pixel is premultiplied exactly once. Degenerate geometry
// yields the first colour. Repeat=1 wraps at integer boundaries; repeat=2 mirrors there.
[[nodiscard]] inline render::ImageResult<render::Rgba32f>
gradientPixel(const ResolvedGradient& gradient, const double authorX,
              const double authorY) noexcept {
    const double dx = (gradient.end.x - gradient.start.x) * gradient.aspect;
    const double dy = gradient.end.y - gradient.start.y;
    const double px = (authorX - gradient.start.x) * gradient.aspect;
    const double py = authorY - gradient.start.y;
    const double span2 = dx * dx + dy * dy;
    double t = 0.0;
    if (span2 > 0.0 && std::isfinite(span2)) {
        t = gradient.radial ? std::hypot(px, py) / std::sqrt(span2)
                            : (px * dx + py * dy) / span2;
    }
    if (gradient.repeat == 1)
        t -= std::floor(t);
    else if (gradient.repeat == 2) {
        const double unit = t - 2.0 * std::floor(t / 2.0);
        t = unit <= 1.0 ? unit : 2.0 - unit;
    } else
        t = std::clamp(t, 0.0, 1.0);
    const double u = 1.0 - t;
    const double a = u * gradient.first.alpha + t * gradient.second.alpha;
    return render::Rgba32f::fromPremultiplied(
        static_cast<float>((u * gradient.first.red + t * gradient.second.red) * a),
        static_cast<float>((u * gradient.first.green + t * gradient.second.green) * a),
        static_cast<float>((u * gradient.first.blue + t * gradient.second.blue) * a),
        static_cast<float>(a));
}
} // namespace bloom::runtime::detail
