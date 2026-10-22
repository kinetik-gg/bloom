#pragma once

#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace bloom::runtime::detail {

// Insets are authored in full-resolution pixels. At a proxy resolution each side is rounded
// independently to the nearest device pixel. The same margins change data and display windows;
// pixel aspect stays unchanged. Pad supplies transparent pixels outside the input data window.
struct CanvasMargins final {
    CanvasImageEffect effect = CanvasImageEffect::Crop;
    std::array<double, 4> sides{}; // left, top, right, bottom
    [[nodiscard]] bool identity() const noexcept {
        return sides == std::array<double, 4>{};
    }
};

[[nodiscard]] inline std::optional<render::ImageWindow>
canvasOutputWindow(const render::ImageWindow source, const CanvasMargins& margins,
                   const double horizontalScale, const double verticalScale) noexcept {
    if (static_cast<std::uint8_t>(margins.effect) >
        static_cast<std::uint8_t>(CanvasImageEffect::Pad))
        return std::nullopt;
    if (!(horizontalScale > 0.0) || !(verticalScale > 0.0) ||
        !std::isfinite(horizontalScale) || !std::isfinite(verticalScale))
        return std::nullopt;
    std::array<std::int64_t, 4> pixels{};
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const double amount = margins.sides[i];
        const double scaled = amount * (i == 0 || i == 2 ? horizontalScale : verticalScale);
        if (!std::isfinite(amount) || amount < 0.0 || !std::isfinite(scaled) ||
            scaled > 16777216.0)
            return std::nullopt;
        pixels[i] = static_cast<std::int64_t>(std::round(scaled));
    }
    const auto left = pixels[0], top = pixels[1], right = pixels[2], bottom = pixels[3];
    const auto width = static_cast<std::int64_t>(source.extent().width());
    const auto height = static_cast<std::int64_t>(source.extent().height());
    if (margins.effect == CanvasImageEffect::Crop) {
        if (left + right >= width || top + bottom >= height)
            return std::nullopt;
        const auto output = render::ImageWindow::create(
            source.originX() + left, source.originY() + top,
            static_cast<std::uint64_t>(width - left - right),
            static_cast<std::uint64_t>(height - top - bottom));
        return output ? std::optional{*output.value()} : std::nullopt;
    }
    if (source.originX() < std::numeric_limits<std::int64_t>::min() + left ||
        source.originY() < std::numeric_limits<std::int64_t>::min() + top)
        return std::nullopt;
    const auto output = render::ImageWindow::create(
        source.originX() - left, source.originY() - top,
        static_cast<std::uint64_t>(width + left + right),
        static_cast<std::uint64_t>(height + top + bottom));
    return output ? std::optional{*output.value()} : std::nullopt;
}

} // namespace bloom::runtime::detail
