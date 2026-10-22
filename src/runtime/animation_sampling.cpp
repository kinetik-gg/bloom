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

[[nodiscard]] bool cancelled(const bloom::runtime::CancellationToken* cancellation) noexcept {
    return cancellation != nullptr && cancellation->isCancellationRequested();
}

[[nodiscard]] CompiledKeyframeInterpolation
interpolationMode(CompiledKeyframeInterpolation mode) noexcept {
    return mode;
}
[[nodiscard]] CompiledKeyframeInterpolation
interpolationMode(bloom::document::KeyframeInterpolation mode) noexcept {
    switch (mode) {
    case bloom::document::KeyframeInterpolation::Hold:
        return CompiledKeyframeInterpolation::Hold;
    case bloom::document::KeyframeInterpolation::Linear:
        return CompiledKeyframeInterpolation::Linear;
    case bloom::document::KeyframeInterpolation::EaseInOut:
        return CompiledKeyframeInterpolation::EaseInOut;
    }
    return static_cast<CompiledKeyframeInterpolation>(255);
}

template <typename Keyframes> struct ScalarCurveView {
    bloom::document::AnimationCurveId id;
    const Keyframes& keyframes;
};

[[nodiscard]] bool supportedInterpolation(const CompiledKeyframeInterpolation mode) noexcept {
    return mode == CompiledKeyframeInterpolation::Hold ||
           mode == CompiledKeyframeInterpolation::Linear ||
           mode == CompiledKeyframeInterpolation::EaseInOut;
}

// Every compiled curve a sampler validates is a SCALAR table: a vector or colour parameter is
// sampled component by component, and a component is a scalar curve.
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
        if (!keyframe.id.isValid() || !std::isfinite(keyframe.value) ||
            (index > 0 && !(curve.keyframes[index - 1].time < keyframe.time))) {
            return AnimationSamplingError::InvalidCurve;
        }
        // Handle domain is part of curve admission, not of the eased branch: a curve carrying an
        // out-of-range handle is rejected on every request time, so acceptance cannot depend on
        // which segment the request happens to land in.
        if (!bloom::document::isValidKeyframeHandle(keyframe.outgoingHandle) ||
            !bloom::document::isValidKeyframeHandle(keyframe.incomingHandle)) {
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
        if (!supportedInterpolation(interpolationMode(keyframe.outgoingInterpolation))) {
            return AnimationSamplingError::UnsupportedInterpolation;
        }
    }
    return interpolationMode(curve.keyframes.back().outgoingInterpolation) ==
                   CompiledKeyframeInterpolation::Linear
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

// The EaseInOut factor transform for a segment whose handles are both bitwise DEFAULT, at (1/3, 0)
// and (2/3, 1). With those control points the interpolation is a cubic Bezier whose x control
// points are 0, 1/3, 2/3, 1 has x(s) == s identically, so the exact rational interval factor IS the
// curve parameter and the eased factor is the closed-form polynomial 3t^2 - 2t^3 -- which is
// exactly ScalarPrimitive::Smoothstep over the unit range. Reusing that already-validated core
// primitive rather than hand-multiplying keeps the sampler's "no libm, no long double, no
// compiler-specific extended integer" property and gives the transform its own semantics version
// (kScalarPrimitiveSemanticsVersion) for free. Endpoints stay exact: Smoothstep maps 0 to exactly 0
// and 1 to exactly 1.
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

// One axis of a cubic Bezier at parameter `s`, written out of + - * / alone in a fixed operation
// order so two calls with the same operands cannot differ by a bit. No libm, no long double, no
// fused multiply-add: the sampler's arithmetic contract is the same one the exact rational factor
// already keeps.
[[nodiscard]] double bezierAxis(const double parameter, const double p0, const double p1,
                                const double p2, const double p3) noexcept {
    const double inverse = 1.0 - parameter;
    const double inverseSquared = inverse * inverse;
    const double parameterSquared = parameter * parameter;
    const double weight0 = inverseSquared * inverse;
    const double weight1 = 3.0 * (inverseSquared * parameter);
    const double weight2 = 3.0 * (inverse * parameterSquared);
    const double weight3 = parameterSquared * parameter;
    return ((weight0 * p0) + (weight1 * p1)) + ((weight2 * p2) + (weight3 * p3));
}

// The curve parameter whose x equals the segment's interval factor. x(s) is the cubic Bezier with
// x control points (0, x1, x2, 1); handle times in [0, 1] keep it monotone non-decreasing, which is
// what makes a bisection an inversion rather than a guess. The loop always runs its full 64 steps
// -- there is no early exit and no tolerance -- so the result is a pure function of the operands:
// two calls are bitwise equal, and a larger factor can only ever choose the same or a later half,
// so s is monotone in the factor.
[[nodiscard]] double bezierParameterForFactor(const double factor, const double x1,
                                              const double x2) noexcept {
    double low = 0.0;
    double high = 1.0;
    for (int step = 0; step < 64; ++step) {
        const double middle = (low + high) * 0.5;
        if (bezierAxis(middle, 0.0, x1, x2, 1.0) < factor) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return (low + high) * 0.5;
}

// The eased segment of a scalar curve whose ease handles are NOT both default. P0 and P3 are the
// two keys; P1 is the left key offset by its outgoing handle and P2 the right key offset by its
// incoming one, with handle times measured as fractions of the segment. When both handle TIMES are
// bitwise default the x cubic is the identity (control points 0, 1/3, 2/3, 1) and the interval
// factor is already the curve parameter, so the inversion is skipped entirely.
template <typename Keyframe>
[[nodiscard]] AnimationSamplingError sampleBezierSegment(const Keyframe& left,
                                                         const Keyframe& right, const double factor,
                                                         double& out) noexcept {
    const double outgoingTime = left.outgoingHandle.time;
    const double incomingTime = right.incomingHandle.time;
    const double parameter =
        (bloom::document::isDefaultKeyframeHandleTime(outgoingTime) &&
         bloom::document::isDefaultKeyframeHandleTime(incomingTime))
            ? factor
            : bezierParameterForFactor(factor, outgoingTime, 1.0 - incomingTime);
    out = bezierAxis(parameter, left.value, left.value + left.outgoingHandle.value,
                     right.value + right.incomingHandle.value, right.value);
    if (!std::isfinite(out)) {
        return AnimationSamplingError::NonFiniteResult;
    }
    return AnimationSamplingError::None;
}

template <typename Curve>
[[nodiscard]] auto interval(const Curve& curve, const RationalTime time) noexcept {
    return std::upper_bound(curve.keyframes.begin(), curve.keyframes.end(), time,
                            [](const auto requestedTime, const auto& keyframe) {
                                return requestedTime < keyframe.time;
                            });
}

[[nodiscard]] bloom::runtime::AnimationSampleResult<double>
endpoint(const double value, const bloom::document::KeyframeId keyframeId) noexcept {
    return {value, AnimationSamplingError::None, keyframeId};
}

} // namespace

namespace bloom::runtime {

namespace {

// The ONE interval-selection and interpolation body. Every sampled curve reaches it: a scalar
// parameter's own, and each component of a vector or colour parameter, which is what composes the
// typed value the caller asked for. The rules -- extrapolation, Hold, Linear, the eased factor and
// the Bezier segment -- are unchanged and remain sampling semantics version 2.
template <typename Curve>
AnimationSampleResult<double> sampleCurve(const Curve& curve, const core::RationalTime time,
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
    if (time == leftKeyframe.time || interpolationMode(leftKeyframe.outgoingInterpolation) ==
                                         CompiledKeyframeInterpolation::Hold) {
        return endpoint(leftKeyframe.value, leftKeyframe.id);
    }
    const bool eased = interpolationMode(leftKeyframe.outgoingInterpolation) ==
                       CompiledKeyframeInterpolation::EaseInOut;
    if (!eased && interpolationMode(leftKeyframe.outgoingInterpolation) !=
                      CompiledKeyframeInterpolation::Linear) {
        return {std::nullopt, AnimationSamplingError::UnsupportedInterpolation, leftKeyframe.id};
    }

    const auto factor = core::rationalIntervalFactor(time, leftKeyframe.time, right->time);
    if (!factor || factor.value() == nullptr) {
        return {std::nullopt, AnimationSamplingError::InvalidInterval, leftKeyframe.id};
    }
    double shared = *factor.value();
    if (eased) {
        // A key whose handles are both bitwise default takes the pre-handle path VERBATIM -- the
        // closed-form Smoothstep factor and the shared Mix -- which is why every document written
        // before ease handles existed still samples bit-for-bit and
        // kAnimationSamplingSemanticsVersion does not move. Only a non-default handle reaches the
        // Bezier segment.
        if (!document::isDefaultKeyframeHandle(leftKeyframe.outgoingHandle) ||
            !document::isDefaultKeyframeHandle(right->incomingHandle)) {
            double easedValue = 0.0;
            if (const auto error = sampleBezierSegment(leftKeyframe, *right, shared, easedValue);
                error != AnimationSamplingError::None) {
                return {std::nullopt, error, leftKeyframe.id};
            }
            return {easedValue, AnimationSamplingError::None, leftKeyframe.id};
        }
        if (const auto error = easedFactor(shared, shared); error != AnimationSamplingError::None) {
            return {std::nullopt, error, leftKeyframe.id};
        }
    }
    double mixed = 0.0;
    if (const auto error = mixChannel(leftKeyframe.value, right->value, shared, mixed);
        error != AnimationSamplingError::None) {
        return {std::nullopt, error, leftKeyframe.id};
    }
    return {mixed, AnimationSamplingError::None, leftKeyframe.id};
}

template <typename Keyframes>
AnimationSampleResult<double>
sampleComponent(document::AnimationCurveId curveId, const Keyframes& keyframes, double fallback,
                core::RationalTime time, const CancellationToken* cancellation) noexcept {
    if (keyframes.empty()) {
        if (cancelled(cancellation))
            return {std::nullopt, AnimationSamplingError::Cancelled, std::nullopt};
        if (!core::supportsReferenceFloatingPointEnvironment<double>())
            return {std::nullopt, AnimationSamplingError::UnsupportedFloatingPointEnvironment,
                    std::nullopt};
        if (!std::isfinite(fallback))
            return {std::nullopt, AnimationSamplingError::NonFiniteResult, std::nullopt};
        return {fallback, AnimationSamplingError::None, std::nullopt};
    }
    return sampleCurve(ScalarCurveView{curveId, keyframes}, time, cancellation);
}

template <typename Value, std::size_t Size> struct ComponentCurveView {
    document::AnimationCurveId id;
    std::array<std::span<const document::ScalarKeyframe>, Size> components;
    Value defaultValue;
};
template <typename Curve, typename Value>
auto componentView(const Curve& curve, Value fallback) noexcept {
    ComponentCurveView<Value, std::tuple_size_v<decltype(curve.components)>> view{
        curve.id, {}, fallback};
    for (std::size_t i = 0; i < view.components.size(); ++i)
        view.components[i] = curve.components[i].keyframes;
    return view;
}

template <typename Curve>
AnimationSampleResult<document::Vec2d> sampleVec2(const Curve& curve, core::RationalTime time,
                                                  const CancellationToken* cancellation) noexcept {
    const auto x =
        sampleComponent(curve.id, curve.components[0], curve.defaultValue.x, time, cancellation);
    const auto y =
        sampleComponent(curve.id, curve.components[1], curve.defaultValue.y, time, cancellation);
    if (!x || !y)
        return {std::nullopt, !x ? x.error : y.error, !x ? x.segmentStart : y.segmentStart};
    return {document::Vec2d{x.value.value_or(0.0), y.value.value_or(0.0)},
            AnimationSamplingError::None,
            x.segmentStart.has_value() ? x.segmentStart : y.segmentStart};
}

template <typename Curve>
AnimationSampleResult<document::Vec3d> sampleVec3(const Curve& curve, core::RationalTime time,
                                                  const CancellationToken* cancellation) noexcept {
    const auto x =
        sampleComponent(curve.id, curve.components[0], curve.defaultValue.x, time, cancellation);
    const auto y =
        sampleComponent(curve.id, curve.components[1], curve.defaultValue.y, time, cancellation);
    const auto z =
        sampleComponent(curve.id, curve.components[2], curve.defaultValue.z, time, cancellation);
    if (!x || !y || !z)
        return {std::nullopt,
                !x   ? x.error
                : !y ? y.error
                     : z.error,
                !x   ? x.segmentStart
                : !y ? y.segmentStart
                     : z.segmentStart};
    return {document::Vec3d{x.value.value_or(0.0), y.value.value_or(0.0), z.value.value_or(0.0)},
            AnimationSamplingError::None,
            x.segmentStart.has_value()   ? x.segmentStart
            : y.segmentStart.has_value() ? y.segmentStart
                                         : z.segmentStart};
}

template <typename Curve>
AnimationSampleResult<core::Color4d> sampleColor4(const Curve& curve, core::RationalTime time,
                                                  const CancellationToken* cancellation) noexcept {
    const auto red =
        sampleComponent(curve.id, curve.components[0], curve.defaultValue.red, time, cancellation);
    const auto green = sampleComponent(curve.id, curve.components[1], curve.defaultValue.green,
                                       time, cancellation);
    const auto blue =
        sampleComponent(curve.id, curve.components[2], curve.defaultValue.blue, time, cancellation);
    const auto alpha = sampleComponent(curve.id, curve.components[3], curve.defaultValue.alpha,
                                       time, cancellation);
    if (!red || !green || !blue || !alpha)
        return {std::nullopt,
                !red     ? red.error
                : !green ? green.error
                : !blue  ? blue.error
                         : alpha.error,
                !red     ? red.segmentStart
                : !green ? green.segmentStart
                : !blue  ? blue.segmentStart
                         : alpha.segmentStart};
    return {core::Color4d{red.value.value_or(0.0), green.value.value_or(0.0),
                          blue.value.value_or(0.0), alpha.value.value_or(0.0)},
            AnimationSamplingError::None, red.segmentStart};
}

} // namespace

AnimationSampleResult<double> sampleAnimationCurve(const CompiledScalarCurve& curve,
                                                   core::RationalTime time) noexcept {
    return sampleCurve(curve, time, nullptr);
}
AnimationSampleResult<double> sampleAnimationCurve(const CompiledScalarCurve& curve,
                                                   core::RationalTime time,
                                                   const CancellationToken& cancellation) noexcept {
    return sampleCurve(curve, time, &cancellation);
}
AnimationSampleResult<double> sampleAnimationCurve(const document::ScalarAnimationCurve& curve,
                                                   core::RationalTime time) noexcept {
    return sampleCurve(curve, time, nullptr);
}
AnimationSampleResult<double>
sampleAnimationComponentCurve(document::AnimationCurveId curveId,
                              std::span<const CompiledScalarKeyframe> keyframes, double fallback,
                              core::RationalTime time) noexcept {
    return sampleComponent(curveId, keyframes, fallback, time, nullptr);
}
AnimationSampleResult<double> sampleAnimationComponentCurve(
    document::AnimationCurveId curveId, std::span<const CompiledScalarKeyframe> keyframes,
    double fallback, core::RationalTime time, const CancellationToken& cancellation) noexcept {
    return sampleComponent(curveId, keyframes, fallback, time, &cancellation);
}

AnimationSampleResult<document::Vec2d> sampleAnimationCurve(const CompiledVec2Curve& curve,
                                                            core::RationalTime time) noexcept {
    return sampleVec2(curve, time, nullptr);
}
AnimationSampleResult<document::Vec2d>
sampleAnimationCurve(const CompiledVec2Curve& curve, core::RationalTime time,
                     const CancellationToken& cancellation) noexcept {
    return sampleVec2(curve, time, &cancellation);
}
AnimationSampleResult<document::Vec2d>
sampleAnimationCurve(const document::Vec2AnimationCurve& curve, core::RationalTime time,
                     document::Vec2d fallback) noexcept {
    return sampleVec2(componentView(curve, fallback), time, nullptr);
}

AnimationSampleResult<document::Vec3d> sampleAnimationCurve(const CompiledVec3Curve& curve,
                                                            core::RationalTime time) noexcept {
    return sampleVec3(curve, time, nullptr);
}
AnimationSampleResult<document::Vec3d>
sampleAnimationCurve(const CompiledVec3Curve& curve, core::RationalTime time,
                     const CancellationToken& cancellation) noexcept {
    return sampleVec3(curve, time, &cancellation);
}
AnimationSampleResult<document::Vec3d>
sampleAnimationCurve(const document::Vec3AnimationCurve& curve, core::RationalTime time,
                     document::Vec3d fallback) noexcept {
    return sampleVec3(componentView(curve, fallback), time, nullptr);
}

AnimationSampleResult<core::Color4d> sampleAnimationCurve(const CompiledColor4Curve& curve,
                                                          core::RationalTime time) noexcept {
    return sampleColor4(curve, time, nullptr);
}
AnimationSampleResult<core::Color4d>
sampleAnimationCurve(const CompiledColor4Curve& curve, core::RationalTime time,
                     const CancellationToken& cancellation) noexcept {
    return sampleColor4(curve, time, &cancellation);
}
AnimationSampleResult<core::Color4d>
sampleAnimationCurve(const document::Color4AnimationCurve& curve, core::RationalTime time,
                     core::Color4d fallback) noexcept {
    return sampleColor4(componentView(curve, fallback), time, nullptr);
}

} // namespace bloom::runtime
