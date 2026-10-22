#pragma once

#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <cstdint>
#include <optional>
#include <span>

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

[[nodiscard]] AnimationSampleResult<double>
sampleAnimationComponentCurve(document::AnimationCurveId curveId,
                              std::span<const CompiledScalarKeyframe> keyframes, double fallback,
                              core::RationalTime time) noexcept;
[[nodiscard]] AnimationSampleResult<double> sampleAnimationComponentCurve(
    document::AnimationCurveId curveId, std::span<const CompiledScalarKeyframe> keyframes,
    double fallback, core::RationalTime time, const CancellationToken& cancellation) noexcept;

[[nodiscard]] AnimationSampleResult<document::Vec2d>
sampleAnimationCurve(const CompiledVec2Curve& curve, core::RationalTime time) noexcept;
[[nodiscard]] AnimationSampleResult<document::Vec2d>
sampleAnimationCurve(const CompiledVec2Curve& curve, core::RationalTime time,
                     const CancellationToken& cancellation) noexcept;

[[nodiscard]] AnimationSampleResult<document::Vec3d>
sampleAnimationCurve(const CompiledVec3Curve& curve, core::RationalTime time) noexcept;
[[nodiscard]] AnimationSampleResult<document::Vec3d>
sampleAnimationCurve(const CompiledVec3Curve& curve, core::RationalTime time,
                     const CancellationToken& cancellation) noexcept;

[[nodiscard]] AnimationSampleResult<core::Color4d>
sampleAnimationCurve(const CompiledColor4Curve& curve, core::RationalTime time) noexcept;
[[nodiscard]] AnimationSampleResult<core::Color4d>
sampleAnimationCurve(const CompiledColor4Curve& curve, core::RationalTime time,
                     const CancellationToken& cancellation) noexcept;

// Borrow immutable document tables for UI readouts; sampling has the same validation and
// interpolation rules as compiled curves and allocates no keyframe storage.
[[nodiscard]] AnimationSampleResult<double>
sampleAnimationCurve(const document::ScalarAnimationCurve& curve, core::RationalTime time) noexcept;
[[nodiscard]] AnimationSampleResult<document::Vec2d>
sampleAnimationCurve(const document::Vec2AnimationCurve& curve, core::RationalTime time,
                     document::Vec2d fallback = {}) noexcept;
[[nodiscard]] AnimationSampleResult<document::Vec3d>
sampleAnimationCurve(const document::Vec3AnimationCurve& curve, core::RationalTime time,
                     document::Vec3d fallback = {}) noexcept;
[[nodiscard]] AnimationSampleResult<core::Color4d>
sampleAnimationCurve(const document::Color4AnimationCurve& curve, core::RationalTime time,
                     core::Color4d fallback = {}) noexcept;

} // namespace bloom::runtime
