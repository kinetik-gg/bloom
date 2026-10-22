#pragma once

#include <bloom/runtime/compiled_plan.hpp>

#include <cmath>
#include <limits>
#include <optional>

namespace bloom::runtime::detail {

// One source of arithmetic for the CPU oracle and GPU OCIO MatrixTransform. The transform is
// applied to straight, scene-linear RGB; alpha is never transformed. Negative and HDR values are
// deliberately not clamped. Float coefficients match the resident RGBA32F storage precision.
struct PointwiseAffine final {
    float scale = 1.0F;
    float offset = 0.0F;

    [[nodiscard]] bool identity() const noexcept { return scale == 1.0F && offset == 0.0F; }
};

[[nodiscard]] inline std::optional<PointwiseAffine>
pointwiseAffine(const PointwiseImageKernel& kernel, const std::optional<double> amount) noexcept {
    if (kernel.effect == PointwiseImageEffect::Invert)
        return kernel.amount || amount ? std::nullopt : std::optional{PointwiseAffine{-1.0F, 1.0F}};
    if (!kernel.amount || !amount || !std::isfinite(*amount))
        return std::nullopt;
    double scale = 1.0;
    double offset = 0.0;
    switch (kernel.effect) {
    case PointwiseImageEffect::Exposure:
        scale = std::exp2(*amount);
        break;
    case PointwiseImageEffect::Offset:
        offset = *amount;
        break;
    case PointwiseImageEffect::Multiply:
        scale = *amount;
        break;
    case PointwiseImageEffect::Invert:
        return std::nullopt;
    default:
        return std::nullopt;
    }
    constexpr double kMaxFloat = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(scale) || std::abs(scale) > kMaxFloat || std::abs(offset) > kMaxFloat)
        return std::nullopt;
    return PointwiseAffine{static_cast<float>(scale), static_cast<float>(offset)};
}
} // namespace bloom::runtime::detail
