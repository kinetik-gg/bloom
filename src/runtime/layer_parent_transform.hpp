#pragma once

#include <algorithm>
#include <array>
#include <bloom/document/parameter.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <cmath>
#include <numbers>

namespace bloom::runtime::detail {
// Author-space affine matrix. Unlike a rotation/scale decomposition this retains shear produced
// by rotated, nonuniformly scaled ancestors. Opacity is deliberately not inherited.
struct LayerMatrix {
    double a = 1, b = 0, c = 0, d = 1, x = 0, y = 0;
    [[nodiscard]] document::Vec2d map(document::Vec2d p) const {
        return {a * p.x + b * p.y + x, c * p.x + d * p.y + y};
    }
    [[nodiscard]] LayerMatrix times(const LayerMatrix& q) const {
        const auto offset = map({q.x, q.y});
        return {a * q.a + b * q.c, a * q.b + b * q.d, c * q.a + d * q.c,
                c * q.b + d * q.d, offset.x,          offset.y};
    }
    [[nodiscard]] static LayerMatrix authored(document::Vec2d position, document::Vec2d pivot,
                                              document::Vec2d scale, double degrees) {
        const double angle = std::remainder(degrees, 360.0);
        double cosine = std::cos(angle * std::numbers::pi / 180.0);
        double sine = std::sin(angle * std::numbers::pi / 180.0);
        if (angle == 0) {
            cosine = 1;
            sine = 0;
        } else if (angle == 90) {
            cosine = 0;
            sine = 1;
        } else if (angle == -90) {
            cosine = 0;
            sine = -1;
        } else if (std::abs(angle) == 180) {
            cosine = -1;
            sine = 0;
        }
        LayerMatrix result{
            cosine * scale.x, -sine * scale.y, sine * scale.x, cosine * scale.y, 0, 0};
        const auto moved = result.map(pivot);
        result.x = position.x - moved.x;
        result.y = position.y - moved.y;
        return result;
    }
};

// Single resample of a parented layer, with the same pixel-edge/proxy convention as LayerTransform.
// The unparented path continues to use the reference primitive unchanged.
class ParentedLayerTransform {
  public:
    ParentedLayerTransform(LayerMatrix matrix, render::ImageWindow window, double sx, double sy,
                           float opacity)
        : matrix_(matrix), window_(window), sx_(sx), sy_(sy), opacity_(opacity),
          determinant_(matrix.a * matrix.d - matrix.b * matrix.c) {}
    [[nodiscard]] bool finite() const {
        return std::isfinite(determinant_) && std::isfinite(matrix_.a) &&
               std::isfinite(matrix_.b) && std::isfinite(matrix_.c) && std::isfinite(matrix_.d) &&
               std::isfinite(matrix_.x) && std::isfinite(matrix_.y);
    }
    [[nodiscard]] bool invertible() const {
        return determinant_ != 0 && std::isfinite(determinant_);
    }
    [[nodiscard]] render::LayerTransform::SamplePoint forwardMap(double x, double y) const {
        const auto p = matrix_.map({(x + static_cast<double>(window_.originX()) + 0.5) / sx_,
                                    (y + static_cast<double>(window_.originY()) + 0.5) / sy_});
        return {p.x * sx_ - 0.5, p.y * sy_ - 0.5};
    }
    [[nodiscard]] std::optional<render::ImageWindow> supportBounds(render::ImageWindow clip) const {
        const double w = window_.extent().width(), h = window_.extent().height();
        const std::array points{forwardMap(-1, -1), forwardMap(w, -1), forwardMap(w, h),
                                forwardMap(-1, h)};
        double left = points[0].x, right = left, top = points[0].y, bottom = top;
        for (const auto p : points) {
            left = std::min(left, p.x);
            right = std::max(right, p.x);
            top = std::min(top, p.y);
            bottom = std::max(bottom, p.y);
        }
        const auto x0 = std::max(clip.originX(), static_cast<std::int64_t>(std::floor(left)));
        const auto y0 = std::max(clip.originY(), static_cast<std::int64_t>(std::floor(top)));
        const auto x1 =
            std::min(clip.maxXExclusive() - 1, static_cast<std::int64_t>(std::ceil(right)));
        const auto y1 =
            std::min(clip.maxYExclusive() - 1, static_cast<std::int64_t>(std::ceil(bottom)));
        if (x1 < x0 || y1 < y0)
            return std::nullopt;
        const auto result =
            render::ImageWindow::create(x0, y0, static_cast<std::uint32_t>(x1 - x0 + 1),
                                        static_cast<std::uint32_t>(y1 - y0 + 1));
        return result ? std::optional{*result.value()} : std::nullopt;
    }
    [[nodiscard]] render::ImageStatus row(render::Rgba32fImageView source,
                                          render::ImageWindow outputWindow, std::int64_t outputY,
                                          std::span<render::Rgba32f> output) const {
        using Pixel = std::array<float, 4>;
        const auto width = window_.extent().width(), height = window_.extent().height();
        const auto sample = [&](std::int64_t x, std::int64_t y) -> Pixel {
            if (x < 0 || y < 0 || x >= width || y >= height)
                return {};
            const auto p =
                source.pixels()[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)];
            return {p.red(), p.green(), p.blue(), p.alpha()};
        };
        const auto lerp = [](Pixel a, Pixel b, float t) {
            if (t == 0)
                return a;
            if (t == 1)
                return b;
            for (std::size_t i = 0; i < a.size(); ++i)
                a[i] = std::fma(t, b[i], (1 - t) * a[i]);
            return a;
        };
        for (std::size_t column = 0; column < output.size(); ++column) {
            const double px =
                (static_cast<double>(outputWindow.originX()) + static_cast<double>(column) + 0.5) /
                    sx_ -
                matrix_.x;
            const double py = (static_cast<double>(outputY) + 0.5) / sy_ - matrix_.y;
            const double x = (matrix_.d * px - matrix_.b * py) / determinant_ * sx_ - 0.5 -
                             static_cast<double>(window_.originX());
            const double y = (-matrix_.c * px + matrix_.a * py) / determinant_ * sy_ - 0.5 -
                             static_cast<double>(window_.originY());
            if (!std::isfinite(x) || !std::isfinite(y) || x <= -1 || y <= -1 || x >= width ||
                y >= height) {
                output[column] = render::Rgba32f::transparent();
                continue;
            }
            const auto ix = static_cast<std::int64_t>(std::floor(x)),
                       iy = static_cast<std::int64_t>(std::floor(y));
            const auto fx = static_cast<float>(x - static_cast<double>(ix)),
                       fy = static_cast<float>(y - static_cast<double>(iy));
            auto p = lerp(lerp(sample(ix, iy), sample(ix + 1, iy), fx),
                          lerp(sample(ix, iy + 1), sample(ix + 1, iy + 1), fx), fy);
            for (auto& channel : p)
                channel *= opacity_;
            const auto pixel = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
            if (!pixel)
                return *pixel.error();
            output[column] = *pixel.value();
        }
        return std::nullopt;
    }

  private:
    LayerMatrix matrix_;
    render::ImageWindow window_;
    double sx_, sy_;
    float opacity_;
    double determinant_;
};
} // namespace bloom::runtime::detail
