#pragma once

#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <cstdint>
#include <optional>

namespace bloom::runtime {

enum class AnimationSamplingError : std::uint8_t {
    None,
    InvalidCurve,
    UnsupportedFloatingPointEnvironment,
    UnsupportedInterpolation,
    InvalidInterval,
    NonFiniteResult,
    Cancelled,
};

template <typename Value> struct AnimationSampleResult final {
    std::optional<Value> value;
    AnimationSamplingError error = AnimationSamplingError::None;
    std::optional<document::KeyframeId> segmentStart;

    [[nodiscard]] explicit operator bool() const noexcept {
        return value.has_value() && error == AnimationSamplingError::None;
    }
};

[[nodiscard]] AnimationSampleResult<double> sampleAnimationCurve(const CompiledScalarCurve& curve,
                                                                 core::RationalTime time) noexcept;
[[nodiscard]] AnimationSampleResult<double>
sampleAnimationCurve(const CompiledScalarCurve& curve, core::RationalTime time,
                     const CancellationToken& cancellation) noexcept;

[[nodiscard]] AnimationSampleResult<document::Vec2d>
sampleAnimationCurve(const CompiledVec2Curve& curve, core::RationalTime time) noexcept;
[[nodiscard]] AnimationSampleResult<document::Vec2d>
sampleAnimationCurve(const CompiledVec2Curve& curve, core::RationalTime time,
                     const CancellationToken& cancellation) noexcept;

} // namespace bloom::runtime
