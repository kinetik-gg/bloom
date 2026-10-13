#pragma once

#include <bloom/render/gpu_composite.hpp>
#include <bloom/render/processing_host_storage.hpp>

#include <cmath>
#include <span>

namespace bloom::render::composite_detail {
// Exact CPU arithmetic shared by the public pure-host helper and rooted native preparation.
inline void fillTranslationAxis(const std::span<GpuAxisSample> axis,
                                const std::uint32_t sourceExtent,
                                const double translation) noexcept {
    for (std::size_t local = 0; local < axis.size(); ++local) {
        const auto sample = static_cast<double>(local) - translation;
        if (sample <= -1.0 || sample >= static_cast<double>(sourceExtent)) {
            axis[local] = GpuAxisSample{kGpuAxisOutOfRange, 0.0F};
            continue;
        }
        const auto base = static_cast<std::int64_t>(std::floor(sample));
        axis[local] = GpuAxisSample{static_cast<std::int32_t>(base),
                                    static_cast<float>(sample - static_cast<double>(base))};
    }
}
using TranslationAxisStorage = std::vector<GpuAxisSample, ProcessingHostAllocator<GpuAxisSample>>;
[[nodiscard]] inline TranslationAxisStorage
prepareProcessingTranslationAxis(const std::uint32_t outputExtent, const std::uint32_t sourceExtent,
                                 const double translation,
                                 const std::shared_ptr<GpuProcessingPhaseLease>& phase,
                                 std::optional<ProcessingHostStorageRefusal>* refusal = nullptr) {
    TranslationAxisStorage axis(outputExtent,
                                ProcessingHostAllocator<GpuAxisSample>{phase, refusal});
    fillTranslationAxis(axis, sourceExtent, translation);
    return axis;
}
} // namespace bloom::render::composite_detail
