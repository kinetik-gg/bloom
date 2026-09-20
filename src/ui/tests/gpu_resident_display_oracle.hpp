#pragma once

// Independent CPU OCIO oracle for the resident RGBA8 display output. It runs the qualified
// Bloom Neutral CPU display processor over the exact same resident RGBA32F image the GPU
// processed, so a dispatch/support defect (missing tail pixels, raced lanes) fails the whole-frame
// per-pixel comparison rather than passing on a tiny uniform fixture.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bloom::render::present_native_support {

using bloom::color::PreparedCpuDisplayProcessorHandle;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba8;

[[nodiscard]] inline std::optional<PreparedCpuDisplayProcessorHandle> buildCpuDisplayProcessor() {
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    if (!resolution.ready()) {
        return std::nullopt;
    }
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    auto built = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    if (!built) {
        return std::nullopt;
    }
    return std::move(built).takeHandle();
}

[[nodiscard]] inline std::optional<std::vector<Rgba8>>
cpuDisplayFrame(const PreparedCpuDisplayProcessorHandle& processor, const Rgba32fImage& image) {
    const auto view = image.view();
    if (!view) {
        return std::nullopt;
    }
    auto displayed = bloom::color::produceBloomNeutralDisplayFrame(
        processor, *view.value(), 65536, std::numeric_limits<std::size_t>::max());
    if (!displayed) {
        return std::nullopt;
    }
    const auto pixels = displayed.value()->pixels();
    return std::vector<Rgba8>(pixels.begin(), pixels.end());
}

// Every pixel: RGB within one 8-bit step and alpha bit-exact, matching the archived CPU OCIO
// display processor. Returns the first failing index (or npos) so callers can report it.
[[nodiscard]] inline std::size_t firstParityMismatch(const std::span<const Rgba8> gpu,
                                                     const std::span<const Rgba8> cpu) {
    if (gpu.size() != cpu.size()) {
        return 0;
    }
    for (std::size_t index = 0; index < gpu.size(); ++index) {
        const int dr =
            std::abs(static_cast<int>(gpu[index].red) - static_cast<int>(cpu[index].red));
        const int dg =
            std::abs(static_cast<int>(gpu[index].green) - static_cast<int>(cpu[index].green));
        const int db =
            std::abs(static_cast<int>(gpu[index].blue) - static_cast<int>(cpu[index].blue));
        if (dr > 1 || dg > 1 || db > 1 || gpu[index].alpha != cpu[index].alpha) {
            return index;
        }
    }
    return std::numeric_limits<std::size_t>::max();
}

} // namespace bloom::render::present_native_support
