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
    } else {
        return std::isfinite(keyframe.value.x) && std::isfinite(keyframe.value.y);
    }
}

[[nodiscard]] bool cancelled(const bloom::runtime::CancellationToken* cancellation) noexcept {
    return cancellation != nullptr && cancellation->isCancellationRequested();
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
        if (keyframe.outgoingInterpolation != CompiledKeyframeInterpolation::Hold &&
            keyframe.outgoingInterpolation != CompiledKeyframeInterpolation::Linear) {
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
        return AnimationSamplingError::InvalidCurve;
    }
    return AnimationSamplingError::InvalidCurve;
}

[[nodiscard]] bloom::runtime::AnimationSampleResult<double>
mix(const double start, const double end, const double factor,
    const bloom::document::KeyframeId segmentStart) noexcept {
    const std::array inputs{start, end, factor};
    const auto result = bloom::core::primitives::evaluateScalar(ScalarPrimitive::Mix, inputs);
    if (!result || result.value() == nullptr) {
        return {std::nullopt, scalarError(result.error()), segmentStart};
    }
    return {*result.value(), AnimationSamplingError::None, segmentStart};
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

} // namespace

namespace bloom::runtime {

namespace {

AnimationSampleResult<double> sampleScalarCurve(const CompiledScalarCurve& curve,
                                                const core::RationalTime time,
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
    if (leftKeyframe.outgoingInterpolation != CompiledKeyframeInterpolation::Linear) {
        return {std::nullopt, AnimationSamplingError::UnsupportedInterpolation, leftKeyframe.id};
    }

    const auto factor = core::rationalIntervalFactor(time, leftKeyframe.time, right->time);
    if (!factor || factor.value() == nullptr) {
        return {std::nullopt, AnimationSamplingError::InvalidInterval, leftKeyframe.id};
    }
    return mix(leftKeyframe.value, right->value, *factor.value(), leftKeyframe.id);
}

AnimationSampleResult<document::Vec2d>
sampleVec2Curve(const CompiledVec2Curve& curve, const core::RationalTime time,
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
    if (leftKeyframe.outgoingInterpolation != CompiledKeyframeInterpolation::Linear) {
        return {std::nullopt, AnimationSamplingError::UnsupportedInterpolation, leftKeyframe.id};
    }

    const auto factor = core::rationalIntervalFactor(time, leftKeyframe.time, right->time);
    if (!factor || factor.value() == nullptr) {
        return {std::nullopt, AnimationSamplingError::InvalidInterval, leftKeyframe.id};
    }
    const auto x = mix(leftKeyframe.value.x, right->value.x, *factor.value(), leftKeyframe.id);
    if (!x || !x.value.has_value()) {
        return {std::nullopt, x.error, leftKeyframe.id};
    }
    const auto y = mix(leftKeyframe.value.y, right->value.y, *factor.value(), leftKeyframe.id);
    if (!y || !y.value.has_value()) {
        return {std::nullopt, y.error, leftKeyframe.id};
    }
    return {document::Vec2d{*x.value, *y.value}, AnimationSamplingError::None, leftKeyframe.id};
}

} // namespace

AnimationSampleResult<double> sampleAnimationCurve(const CompiledScalarCurve& curve,
                                                   const core::RationalTime time) noexcept {
    return sampleScalarCurve(curve, time, nullptr);
}

AnimationSampleResult<double> sampleAnimationCurve(const CompiledScalarCurve& curve,
                                                   const core::RationalTime time,
                                                   const CancellationToken& cancellation) noexcept {
    return sampleScalarCurve(curve, time, &cancellation);
}

AnimationSampleResult<document::Vec2d>
sampleAnimationCurve(const CompiledVec2Curve& curve, const core::RationalTime time) noexcept {
    return sampleVec2Curve(curve, time, nullptr);
}

AnimationSampleResult<document::Vec2d>
sampleAnimationCurve(const CompiledVec2Curve& curve, const core::RationalTime time,
                     const CancellationToken& cancellation) noexcept {
    return sampleVec2Curve(curve, time, &cancellation);
}

} // namespace bloom::runtime
