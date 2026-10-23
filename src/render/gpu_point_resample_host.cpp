#include <bloom/render/gpu_point_resample.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

// Always-compiled host half of PointResampleV1: the exact binary64 axis-map preparation. It has no
// Vulkan dependency, so it is shared byte-for-byte by the Vulkan and CPU-unavailable stub
// translation units and cannot drift between them. The kernel only consumes these indices; no RGBA
// is generated or resampled here, and the cost is O(outputWidth + outputHeight).

namespace bloom::render {
namespace {

// The exact CPU reduction of one output axis: min(extent - 1, (uint32)(double(index) / scale)).
// The binary64 division and the clamp are the media-image-proxy oracle's own arithmetic. The
// double clamp before the conversion is equivalent for the non-negative in-range oracle inputs and
// keeps a hostile argument from reaching an out-of-range float->uint32 conversion.
[[nodiscard]] std::int32_t pointResampleAxisIndex(const std::uint32_t index,
                                                  const std::uint32_t extent,
                                                  const double scale) noexcept {
    const auto mapped = static_cast<double>(index) / scale;
    const auto maximum = static_cast<double>(extent - 1U);
    const auto bounded = mapped > maximum ? maximum : mapped;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(bounded));
}

} // namespace

std::uint32_t pointResampleSourceIndex(const std::int64_t output, const std::int64_t sourceOrigin,
                                       const std::uint32_t sourceExtent,
                                       const double scale) noexcept {
    const auto mapped =
        std::floor(static_cast<double>(output) / scale) - static_cast<double>(sourceOrigin);
    const auto maximum = static_cast<double>(sourceExtent - 1U);
    const auto bounded = mapped < 0.0 ? 0.0 : (mapped > maximum ? maximum : mapped);
    return static_cast<std::uint32_t>(bounded);
}

std::optional<ImageWindow> pointResampleProxyWindow(const ImageWindow source,
                                                    const double horizontalScale,
                                                    const double verticalScale) noexcept {
    if (!std::isfinite(horizontalScale) || !std::isfinite(verticalScale) ||
        horizontalScale <= 0.0 || verticalScale <= 0.0)
        return std::nullopt;
    const auto axis = [](const std::int64_t origin, const std::uint32_t extent,
                         const double scale) -> std::optional<std::pair<double, double>> {
        const auto minimum = std::floor(static_cast<double>(origin) * scale);
        const auto maximum =
            std::ceil((static_cast<double>(origin) + static_cast<double>(extent)) * scale);
        const auto size = std::max(1.0, maximum - minimum);
        constexpr auto kExtent = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
        constexpr auto kOrigin = static_cast<double>(std::numeric_limits<std::int32_t>::max());
        if (!std::isfinite(minimum) || !std::isfinite(size) || size > kExtent ||
            std::fabs(minimum) > kOrigin)
            return std::nullopt;
        return std::pair{minimum, size};
    };
    const auto x = axis(source.originX(), source.extent().width(), horizontalScale);
    const auto y = axis(source.originY(), source.extent().height(), verticalScale);
    if (!x || !y)
        return std::nullopt;
    auto window = ImageWindow::create(
        static_cast<std::int64_t>(x->first), static_cast<std::int64_t>(y->first),
        static_cast<std::uint64_t>(x->second), static_cast<std::uint64_t>(y->second));
    if (!window)
        return std::nullopt;
    return *window.value();
}

GpuPointResampleAxisMaps preparePointResampleAxisMaps(const ImageWindow source,
                                                      const ImageWindow output,
                                                      const double horizontalScale,
                                                      const double verticalScale) {
    GpuPointResampleAxisMaps maps;
    maps.sourceX.resize(output.extent().width());
    maps.sourceY.resize(output.extent().height());
    for (std::uint32_t x = 0; x < output.extent().width(); ++x)
        maps.sourceX[x] = static_cast<std::int32_t>(
            pointResampleSourceIndex(output.originX() + static_cast<std::int64_t>(x),
                                     source.originX(), source.extent().width(), horizontalScale));
    for (std::uint32_t y = 0; y < output.extent().height(); ++y)
        maps.sourceY[y] = static_cast<std::int32_t>(
            pointResampleSourceIndex(output.originY() + static_cast<std::int64_t>(y),
                                     source.originY(), source.extent().height(), verticalScale));
    return maps;
}

GpuPointResampleAxisMaps
preparePointResampleAxisMaps(const std::uint32_t sourceWidth, const std::uint32_t sourceHeight,
                             const std::uint32_t outputWidth, const std::uint32_t outputHeight,
                             const double horizontalScale, const double verticalScale) {
    GpuPointResampleAxisMaps maps;
    maps.sourceX.resize(outputWidth);
    maps.sourceY.resize(outputHeight);
    for (std::uint32_t x = 0; x < outputWidth; ++x) {
        maps.sourceX[x] = pointResampleAxisIndex(x, sourceWidth, horizontalScale);
    }
    for (std::uint32_t y = 0; y < outputHeight; ++y) {
        maps.sourceY[y] = pointResampleAxisIndex(y, sourceHeight, verticalScale);
    }
    return maps;
}

} // namespace bloom::render
