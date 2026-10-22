#pragma once

#include <bloom/render/ocio_gpu_program.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>

namespace bloom::runtime::detail {
// A proxy-upsampled kernel has at most 8193 coefficients (32,772 bytes) per axis. This is
// an explicit shader-source/loop-work bound; every larger request returns Unsupported/failure.
inline constexpr std::size_t kMaximumSeparableBlurSupport = 4096;
struct PreparedSeparableBlur final {
    std::vector<float> horizontal;
    std::vector<float> vertical;
    BlurEdgeMode edge = BlurEdgeMode::Transparent;
    [[nodiscard]] bool identity() const noexcept {
        return horizontal.size() == 1 && vertical.size() == 1;
    }
};
struct ImageEffectResult;
[[nodiscard]] std::optional<PreparedSeparableBlur>
prepareSeparableBlur(const SeparableBlurKernel&, std::optional<double> radius,
                     double horizontalScale, double verticalScale);
[[nodiscard]] std::optional<render::ImageWindow> blurOutputWindow(const PreparedSeparableBlur&,
                                                                  render::ImageWindow);
[[nodiscard]] ImageEffectResult applySeparableBlur(const PreparedSeparableBlur&,
                                                   const render::Rgba32fImage&, std::size_t budget,
                                                   const CancellationToken&, OperationIndex,
                                                   const EvaluationProgressCallback&);
// Host preparation emits only normalized coefficient GLSL. Pixels stay resident on the device.
[[nodiscard]] render::OcioGpuProgramResult
buildSeparableBlurProgram(std::span<const float> weights, bool vertical, BlurEdgeMode edge);
} // namespace bloom::runtime::detail
