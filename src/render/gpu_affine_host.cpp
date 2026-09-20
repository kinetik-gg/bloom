#include <bloom/render/gpu_affine.hpp>

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

// Always-compiled host half of AffineBilinearV1: the exact Float64 inverse-map preparation and the
// input factories. It has no Vulkan dependency, so it is shared byte-for-byte by the Vulkan and
// CPU-unavailable stub translation units and cannot drift between them. The kernel only consumes
// this coordinate metadata; no RGBA is generated or resampled here.

namespace bloom::render {
namespace {

// The exact CPU reduction of one inverse-mapped sample (layerTransformBilinearRow and the parented
// row share it): transparent when the sample is not strictly inside (-1, extent) on either axis,
// else floor and a single Float32 rounding of the fraction. The non-finite guard is redundant for
// a finite LayerTransform but keeps a hostile composed matrix from reaching an out-of-range
// int64->int32 conversion.
[[nodiscard]] GpuAffineSample reduceAffineSample(const double x, const double y,
                                                 const std::uint32_t sourceWidth,
                                                 const std::uint32_t sourceHeight) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || x <= -1.0 || y <= -1.0 ||
        x >= static_cast<double>(sourceWidth) || y >= static_cast<double>(sourceHeight)) {
        return GpuAffineSample{kGpuAffineTransparentBase, 0, 0.0F, 0.0F};
    }
    const auto baseX = static_cast<std::int64_t>(std::floor(x));
    const auto baseY = static_cast<std::int64_t>(std::floor(y));
    return GpuAffineSample{static_cast<std::int32_t>(baseX), static_cast<std::int32_t>(baseY),
                           static_cast<float>(x - static_cast<double>(baseX)),
                           static_cast<float>(y - static_cast<double>(baseY))};
}

} // namespace

std::vector<GpuAffineSample> prepareAffineSamples(const LayerTransform& transform,
                                                  const ImageWindow outputWindow) {
    const auto sourceWindow = transform.sourceWindow();
    const std::uint32_t sourceWidth = sourceWindow.extent().width();
    const std::uint32_t sourceHeight = sourceWindow.extent().height();
    const std::uint32_t outputWidth = outputWindow.extent().width();
    const std::uint32_t outputHeight = outputWindow.extent().height();
    std::vector<GpuAffineSample> samples(static_cast<std::size_t>(outputWidth) * outputHeight);
    for (std::uint32_t row = 0; row < outputHeight; ++row) {
        const double absoluteY = static_cast<double>(outputWindow.originY() + row);
        for (std::uint32_t column = 0; column < outputWidth; ++column) {
            const double absoluteX = static_cast<double>(outputWindow.originX() + column);
            const auto sample = transform.inverseMap(absoluteX, absoluteY);
            samples[static_cast<std::size_t>(row) * outputWidth + column] =
                reduceAffineSample(sample.x, sample.y, sourceWidth, sourceHeight);
        }
    }
    return samples;
}

std::vector<GpuAffineSample> prepareAffineMatrixSamples(const GpuAffineMatrix& matrix,
                                                        const ImageWindow sourceWindow,
                                                        const ImageWindow outputWindow) {
    const std::uint32_t sourceWidth = sourceWindow.extent().width();
    const std::uint32_t sourceHeight = sourceWindow.extent().height();
    const std::uint32_t outputWidth = outputWindow.extent().width();
    const std::uint32_t outputHeight = outputWindow.extent().height();
    std::vector<GpuAffineSample> samples(static_cast<std::size_t>(outputWidth) * outputHeight);
    const double determinant = matrix.a * matrix.d - matrix.b * matrix.c;
    if (!std::isfinite(determinant) || determinant == 0.0) {
        // No inverse: the placement collapses the layer onto a line or point. The CPU treats a
        // collapsed layer as empty (transparent), so every output pixel is the transparent
        // sentinel and no device work is needed.
        for (auto& sample : samples) {
            sample = GpuAffineSample{kGpuAffineTransparentBase, 0, 0.0F, 0.0F};
        }
        return samples;
    }
    for (std::uint32_t row = 0; row < outputHeight; ++row) {
        const double absoluteY = static_cast<double>(outputWindow.originY() + row);
        for (std::uint32_t column = 0; column < outputWidth; ++column) {
            const double absoluteX = static_cast<double>(outputWindow.originX() + column);
            const double relativeX = absoluteX - matrix.tx;
            const double relativeY = absoluteY - matrix.ty;
            const double localX = (matrix.d * relativeX - matrix.b * relativeY) / determinant;
            const double localY = (-matrix.c * relativeX + matrix.a * relativeY) / determinant;
            samples[static_cast<std::size_t>(row) * outputWidth + column] =
                reduceAffineSample(localX, localY, sourceWidth, sourceHeight);
        }
    }
    return samples;
}

} // namespace bloom::render
