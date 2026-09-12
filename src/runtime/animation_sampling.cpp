#include <bloom/runtime/animation_sampling.hpp>

#include <bloom/core/floating_point.hpp>
#include <bloom/core/rational_interval.hpp>
#include <bloom/core/scalar_primitives.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace {

using bloom::core::RationalTime;
using bloom::core::primitives::ScalarEvaluationError;
using bloom::core::primitives::ScalarPrimitive;
using bloom::runtime::AnimationSamplingError;
using bloom::runtime::CompiledKeyframeInterpolation;

template <typename Keyframe> [[nodiscard]] bool finiteValue(const Keyframe& keyframe) noexcept {
    if constexpr (std::is_same_v<Keyframe, bloom::runtime::CompiledScalarKeyframe>) {
        return std::isfinite(keyframe.value);
    } else if constexpr (std::is_same_v<Keyframe, bloom::runtime::CompiledColor4Keyframe>) {
        // core::Color4d::isValid() is the authoring-colour contract (finite RGB, alpha in [0, 1]),
        // the same one a constant colour already satisfies -- not a fourth hand-written test.
        return keyframe.value.isValid();
    } else {
        return std::isfinite(keyframe.value.x) && std::isfinite(keyframe.value.y);
    }
}

[[nodiscard]] bool cancelled(const bloom::runtime::CancellationToken* cancellation) noexcept {
    return cancellation != nullptr && cancellation->isCancellationRequested();
}

[[nodiscard]] bool supportedInterpolation(const CompiledKeyframeInterpolation mode) noexcept {
    return mode == CompiledKeyframeInterpolation::Hold ||
           mode == CompiledKeyframeInterpolation::Linear ||
           mode == CompiledKeyframeInterpolation::EaseInOut;
}

template <typename Curve>
[[nodiscard]] AnimationSamplingError
validateForSampling(const Curve& curve,
                    const bloom::runtime::CancellationToken* cancellation) noexcept {
    if (!curve.id.isValid() || curve.keyframes.empty()) {
        return AnimationSamplingError::InvalidCurve;
    }
    for (std::size_t index = 0; index < curve.keyframes.size(); ++index) {
        if (cancelled(cancellation)) {
            return AnimationSamplingError::Cancelled;
        }
        const auto& keyframe = curve.keyframes[index];
        if (!keyframe.id.isValid() || !finiteValue(keyframe) ||
            (index > 0 && !(curve.keyframes[index - 1].time < keyframe.time))) {
            return AnimationSamplingError::InvalidCurve;
        }
    }
    if (!bloom::core::supportsReferenceFloatingPointEnvironment<double>()) {
        return AnimationSamplingError::UnsupportedFloatingPointEnvironment;
    }
    for (const auto& keyframe : curve.keyframes) {
        if (cancelled(cancellation)) {
            return AnimationSamplingError::Cancelled;
        }
        if (!supportedInterpolation(keyframe.outgoingInterpolation)) {
            return AnimationSamplingError::UnsupportedInterpolation;
        }
    }
    return curve.keyframes.back().outgoingInterpolation == CompiledKeyframeInterpolation::Linear
               ? AnimationSamplingError::None
               : AnimationSamplingError::UnsupportedInterpolation;
}

[[nodiscard]] AnimationSamplingError scalarError(const ScalarEvaluationError error) noexcept {
    switch (error) {
    case ScalarEvaluationError::None:
        return AnimationSamplingError::None;
    case ScalarEvaluationError::UnsupportedFloatingPointEnvironment:
        return AnimationSamplingError::UnsupportedFloatingPointEnvironment;
    case ScalarEvaluationError::NonFiniteInput:
    case ScalarEvaluationError::NonFiniteResult:
        return AnimationSamplingError::NonFiniteResult;
    case ScalarEvaluationError::UnknownPrimitive:
    case ScalarEvaluationError::InvalidArity:
    case ScalarEvaluationError::DivideByZero:
    case ScalarEvaluationError::InvalidInterval:
    case ScalarEvaluationError::DegenerateRange:
    case ScalarEvaluationError::OutsideDomain:
        return AnimationSamplingError::InvalidCurve;
    }
    return AnimationSamplingError::InvalidCurve;
}

// One mixed channel, through Float64 scalar Mix version 1.
[[nodiscard]] AnimationSamplingError mixChannel(const double start, const double end,
                                                const double factor, double& out) noexcept {
    const std::array inputs{start, end, factor};
    const auto result = bloom::core::primitives::evaluateScalar(ScalarPrimitive::Mix, inputs);
    if (!result || result.value() == nullptr) {
        return scalarError(result.error());
    }
    out = *result.value();
    return AnimationSamplingError::None;
}

// The EaseInOut factor transform. The interpolation is a cubic Bezier with FIXED symmetric handles
// at (1/3, 0) and (2/3, 1): a cubic Bezier whose x control points are 0, 1/3, 2/3, 1 has x(s) == s
// identically, so the exact rational interval factor IS the curve parameter and the eased factor is
// the closed-form polynomial 3t^2 - 2t^3 -- which is exactly ScalarPrimitive::Smoothstep over the
// unit range. Reusing that already-validated core primitive rather than hand-multiplying keeps the
// sampler's "no libm, no long double, no compiler-specific extended integer" property and gives the
// transform its own semantics version (kScalarPrimitiveSemanticsVersion) for free. Endpoints stay
// exact: Smoothstep maps 0 to exactly 0 and 1 to exactly 1.
[[nodiscard]] AnimationSamplingError easedFactor(const double factor, double& out) noexcept {
    const std::array inputs{0.0, 1.0, factor};
    const auto result =
        bloom::core::primitives::evaluateScalar(ScalarPrimitive::Smoothstep, inputs);
    if (!result || result.value() == nullptr) {
        return scalarError(result.error());
    }
    out = *result.value();
    return AnimationSamplingError::None;
}

template <typename Curve>
[[nodiscard]] auto interval(const Curve& curve, const RationalTime time) noexcept {
    return std::upper_bound(curve.keyframes.begin(), curve.keyframes.end(), time,
                            [](const auto requestedTime, const auto& keyframe) {
                                return requestedTime < keyframe.time;
                            });
}

template <typename Value>
[[nodiscard]] bloom::runtime::AnimationSampleResult<Value>
endpoint(Value value, const bloom::document::KeyframeId keyframeId) noexcept {
    return {std::move(value), AnimationSamplingError::None, keyframeId};
}

// Per-value-kind channel mixing: ONE shared factor applied to the scalar, to each Vec2d component,
// or to each of the four authoring colour channels (docs/architecture/animation-and-time.md,
// "Sampling Semantics Version 1": "compute one shared factor ... and apply Float64 scalar Mix
// version 1 to the scalar or each component").
template <typename Value>
[[nodiscard]] AnimationSamplingError mixValue(const Value& left, const Value& right,
                                              const double factor, Value& out) noexcept {
    if constexpr (std::is_same_v<Value, double>) {
        return mixChannel(left, right, factor, out);
    } else if constexpr (std::is_same_v<Value, bloom::core::Color4d>) {
        if (const auto error = mixChannel(left.red, right.red, factor, out.red);
            error != AnimationSamplingError::None) {
            return error;
        }
        if (const auto error = mixChannel(left.green, right.green, factor, out.green);
            error != AnimationSamplingError::None) {
            return error;
        }
        if (const auto error = mixChannel(left.blue, right.blue, factor, out.blue);
            error != AnimationSamplingError::None) {
            return error;
        }
        return mixChannel(left.alpha, right.alpha, factor, out.alpha);
    } else {
        if (const auto error = mixChannel(left.x, right.x, factor, out.x);
            error != AnimationSamplingError::None) {
            return error;
        }
        return mixChannel(left.y, right.y, factor, out.y);
    }
}

} // namespace

namespace bloom::runtime {

namespace {

// The ONE interval-selection and interpolation body every curve kind shares. Before task S5 the
// scalar and Vec2 paths were two verbatim copies of it; a third copy for colour would have made the
// interval/extrapolation/Hold/eased rules three places that could drift, so the shared body is a
// template over the curve's value type instead. The rules themselves are unchanged.
template <typename Value, typename Curve>
AnimationSampleResult<Value> sampleCurve(const Curve& curve, const core::RationalTime time,
                                         const CancellationToken* cancellation) noexcept {
    if (const auto error = validateForSampling(curve, cancellation);
        error != AnimationSamplingError::None) {
        return {std::nullopt, error, std::nullopt};
    }
    if (time <= curve.keyframes.front().time) {
        const auto& keyframe = curve.keyframes.front();
        return endpoint(keyframe.value, keyframe.id);
    }
    if (time >= curve.keyframes.back().time) {
        const auto& keyframe = curve.keyframes.back();
        return endpoint(keyframe.value, keyframe.id);
    }

    const auto right = interval(curve, time);
    const auto& leftKeyframe = *(right - 1);
    if (time == leftKeyframe.time ||
        leftKeyframe.outgoingInterpolation == CompiledKeyframeInterpolation::Hold) {
        return endpoint(leftKeyframe.value, leftKeyframe.id);
    }
    const bool eased =
        leftKeyframe.outgoingInterpolation == CompiledKeyframeInterpolation::EaseInOut;
    if (!eased && leftKeyframe.outgoingInterpolation != CompiledKeyframeInterpolation::Linear) {
        return {std::nullopt, AnimationSamplingError::UnsupportedInterpolation, leftKeyframe.id};
    }

    const auto factor = core::rationalIntervalFactor(time, leftKeyframe.time, right->time);
    if (!factor || factor.value() == nullptr) {
        return {std::nullopt, AnimationSamplingError::InvalidInterval, leftKeyframe.id};
    }
    double shared = *factor.value();
    if (eased) {
        if (const auto error = easedFactor(shared, shared); error != AnimationSamplingError::None) {
            return {std::nullopt, error, leftKeyframe.id};
        }
    }
    Value mixed{};
    if (const auto error = mixValue(leftKeyframe.value, right->value, shared, mixed);
        error != AnimationSamplingError::None) {
        return {std::nullopt, error, leftKeyframe.id};
    }
    return {mixed, AnimationSamplingError::None, leftKeyframe.id};
}

} // namespace

AnimationSampleResult<double> sampleAnimationCurve(const CompiledScalarCurve& curve,
                                                   const core::RationalTime time) noexcept {
    return sampleCurve<double>(curve, time, nullptr);
}

AnimationSampleResult<double> sampleAnimationCurve(const CompiledScalarCurve& curve,
                                                   const core::RationalTime time,
                                                   const CancellationToken& cancellation) noexcept {
    return sampleCurve<double>(curve, time, &cancellation);
}

AnimationSampleResult<document::Vec2d>
sampleAnimationCurve(const CompiledVec2Curve& curve, const core::RationalTime time) noexcept {
    return sampleCurve<document::Vec2d>(curve, time, nullptr);
}

AnimationSampleResult<document::Vec2d>
sampleAnimationCurve(const CompiledVec2Curve& curve, const core::RationalTime time,
                     const CancellationToken& cancellation) noexcept {
    return sampleCurve<document::Vec2d>(curve, time, &cancellation);
}

AnimationSampleResult<core::Color4d> sampleAnimationCurve(const CompiledColor4Curve& curve,
                                                          const core::RationalTime time) noexcept {
    return sampleCurve<core::Color4d>(curve, time, nullptr);
}

AnimationSampleResult<core::Color4d>
sampleAnimationCurve(const CompiledColor4Curve& curve, const core::RationalTime time,
                     const CancellationToken& cancellation) noexcept {
    return sampleCurve<core::Color4d>(curve, time, &cancellation);
}

} // namespace bloom::runtime
