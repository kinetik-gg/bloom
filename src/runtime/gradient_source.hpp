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
    const double span = std::hypot(dx, dy);
    double t = 0.0;
    if (gradient.start.x != gradient.end.x || gradient.start.y != gradient.end.y) {
        // Squaring a valid span can overflow or underflow and must not turn a nonzero
        // gradient into the first colour. Normalize before taking the linear projection.
        if (!(span > 0.0) || !std::isfinite(span) || !std::isfinite(px) ||
            !std::isfinite(py))
            return render::ImageResult<render::Rgba32f>::failure(
                render::ImageError::codeOnly(render::ImageErrorCode::NonFiniteResult));
        t = gradient.radial ? std::hypot(px, py) / span
                            : (dx == 0.0 ? 0.0 : (px / span) * (dx / span)) +
                                  (dy == 0.0 ? 0.0 : (py / span) * (dy / span));
    }
    // Clamp can saturate an infinite ratio; periodic modes need a finite phase.
    if (std::isnan(t) || (gradient.repeat != 0 && !std::isfinite(t)))
        return render::ImageResult<render::Rgba32f>::failure(
            render::ImageError::codeOnly(render::ImageErrorCode::NonFiniteResult));
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
