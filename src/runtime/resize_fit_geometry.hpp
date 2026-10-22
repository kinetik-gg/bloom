#pragma once

#include "layer_parent_transform.hpp"

#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace bloom::runtime::detail {

[[nodiscard]] inline bool validResizeFitKernel(const ResizeFitKernel& kernel) noexcept {
    return kernel.width > 0 && kernel.width <= 32768 && kernel.height > 0 &&
           kernel.height <= 32768 && kernel.pixelAspectNumerator > 0 &&
           kernel.pixelAspectNumerator <= 32768 && kernel.pixelAspectDenominator > 0 &&
           kernel.pixelAspectDenominator <= 32768 && kernel.filter == 0 && kernel.policy >= 0 &&
           kernel.policy <= 2;
}

struct ResizeFitGeometry final {
    render::Rgba32fImageDescriptor descriptor;
    // Device-pixel edge coordinates. The CPU oracle and compact GPU affine map share this.
    LayerMatrix matrix;
};

[[nodiscard]] inline std::optional<ResizeFitGeometry>
resolveResizeFitGeometry(const ResizeFitKernel& kernel,
                         const render::Rgba32fImageDescriptor& source, const double proxyX,
                         const double proxyY) {
    if (!validResizeFitKernel(kernel) || !std::isfinite(proxyX) || !std::isfinite(proxyY) ||
        proxyX <= 0 || proxyY <= 0)
        return std::nullopt;
    // Production proxy scales are ratios of uint32 extents. With authored extents <= 32768,
    // a genuinely fractional product is more than one ULP from an integer. Remove the one-ULP
    // round-up possible when a division followed by multiplication should produce an integer.
    const auto scaledExtent = [](const std::int64_t extent, const double scale) {
        return std::max(1.0, std::ceil(std::nextafter(static_cast<double>(extent) * scale, 0.0)));
    };
    const double width = scaledExtent(kernel.width, proxyX);
    const double height = scaledExtent(kernel.height, proxyY);
    if (width > std::numeric_limits<std::uint32_t>::max() ||
        height > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    const auto outputWidth = static_cast<std::uint32_t>(width);
    const auto outputHeight = static_cast<std::uint32_t>(height);
    const auto window = render::ImageWindow::create(0, 0, outputWidth, outputHeight);
    // Derive the exact proxy PAR from the authored physical output extent, including ceil.
    const auto aspect =
        core::PixelAspectRatio::create(static_cast<std::uint64_t>(kernel.pixelAspectNumerator) *
                                           static_cast<std::uint64_t>(kernel.width) * outputHeight,
                                       static_cast<std::uint64_t>(kernel.pixelAspectDenominator) *
                                           static_cast<std::uint64_t>(kernel.height) * outputWidth);
    if (!window || !aspect)
        return std::nullopt;
    const auto descriptor =
        render::Rgba32fImageDescriptor::create(*window.value(), *window.value(), *aspect);
    if (!descriptor)
        return std::nullopt;
    const auto display = source.displayWindow();
    const double sourceWidth = display.extent().width();
    const double sourceHeight = display.extent().height();
    double xScale = width / sourceWidth;
    double yScale = height / sourceHeight;
    if (kernel.policy != 0) {
        const auto inputAspect = source.pixelAspect();
        const double aspectScale =
            (static_cast<double>(inputAspect.numerator()) / inputAspect.denominator()) /
            (static_cast<double>(aspect->numerator()) / aspect->denominator());
        const double horizontal = xScale / aspectScale;
        yScale = kernel.policy == 1 ? std::min(horizontal, yScale) : std::max(horizontal, yScale);
        xScale = yScale * aspectScale;
    }
    LayerMatrix matrix{
        xScale,
        0,
        0,
        yScale,
        (width - sourceWidth * xScale) * 0.5 - static_cast<double>(display.originX()) * xScale,
        (height - sourceHeight * yScale) * 0.5 - static_cast<double>(display.originY()) * yScale};
    return ResizeFitGeometry{*descriptor.value(), matrix};
}

} // namespace bloom::runtime::detail
