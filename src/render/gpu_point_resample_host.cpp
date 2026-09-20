#include <bloom/render/gpu_point_resample.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
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
