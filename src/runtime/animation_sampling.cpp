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
    } else if constexpr (std::is_same_v<Keyframe, bloom::runtime::CompiledVec3Keyframe>) {
        return std::isfinite(keyframe.value.x) && std::isfinite(keyframe.value.y) &&
               std::isfinite(keyframe.value.z);
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
        // Handle domain is part of curve admission, not of the eased branch: a curve carrying an
        // out-of-range handle is rejected on every request time, so acceptance cannot depend on
        // which segment the request happens to land in.
        if constexpr (requires { keyframe.outgoingHandle; }) {
            if (!bloom::document::isValidKeyframeHandle(keyframe.outgoingHandle) ||
                !bloom::document::isValidKeyframeHandle(keyframe.incomingHandle)) {
                return AnimationSamplingError::InvalidCurve;
            }
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
    } else if constexpr (std::is_same_v<Value, bloom::document::Vec3d>) {
        if (const auto error = mixChannel(left.x, right.x, factor, out.x);
            error != AnimationSamplingError::None) {
            return error;
        }
        if (const auto error = mixChannel(left.y, right.y, factor, out.y);
            error != AnimationSamplingError::None) {
            return error;
        }
        return mixChannel(left.z, right.z, factor, out.z);
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
        // A key whose handles are both bitwise default takes the pre-handle path VERBATIM -- the
        // closed-form Smoothstep factor and the shared Mix -- which is why every document written
        // before ease handles existed still samples bit-for-bit and
        // kAnimationSamplingSemanticsVersion does not move. Only a non-default handle reaches the
        // Bezier segment, and only a scalar or component key can carry one.
        if constexpr (requires { leftKeyframe.outgoingHandle; }) {
            if (!document::isDefaultKeyframeHandle(leftKeyframe.outgoingHandle) ||
                !document::isDefaultKeyframeHandle(right->incomingHandle)) {
                double easedValue = 0.0;
                if (const auto error =
                        sampleBezierSegment(leftKeyframe, *right, shared, easedValue);
                    error != AnimationSamplingError::None) {
                    return {std::nullopt, error, leftKeyframe.id};
                }
                return {easedValue, AnimationSamplingError::None, leftKeyframe.id};
            }
        }
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

AnimationSampleResult<double>
sampleAnimationComponentCurve(const document::AnimationCurveId curveId,
                              const std::span<const CompiledScalarKeyframe> keyframes,
                              const double fallback, const core::RationalTime time) noexcept {
    if (keyframes.empty()) {
        if (!bloom::core::supportsReferenceFloatingPointEnvironment<double>())
            return {std::nullopt, AnimationSamplingError::UnsupportedFloatingPointEnvironment,
                    std::nullopt};
        if (!std::isfinite(fallback))
            return {std::nullopt, AnimationSamplingError::NonFiniteResult, std::nullopt};
        return {fallback, AnimationSamplingError::None, std::nullopt};
    }
    CompiledScalarCurve curve{
        curveId, std::vector<CompiledScalarKeyframe>(keyframes.begin(), keyframes.end())};
    return sampleCurve<double>(curve, time, nullptr);
}

AnimationSampleResult<double>
sampleAnimationComponentCurve(const document::AnimationCurveId curveId,
                              const std::span<const CompiledScalarKeyframe> keyframes,
                              const double fallback, const core::RationalTime time,
                              const CancellationToken& cancellation) noexcept {
    if (keyframes.empty()) {
        if (cancellation.isCancellationRequested())
            return {std::nullopt, AnimationSamplingError::Cancelled, std::nullopt};
        if (!bloom::core::supportsReferenceFloatingPointEnvironment<double>())
            return {std::nullopt, AnimationSamplingError::UnsupportedFloatingPointEnvironment,
                    std::nullopt};
        if (!std::isfinite(fallback))
            return {std::nullopt, AnimationSamplingError::NonFiniteResult, std::nullopt};
        return {fallback, AnimationSamplingError::None, std::nullopt};
    }
    CompiledScalarCurve curve{
        curveId, std::vector<CompiledScalarKeyframe>(keyframes.begin(), keyframes.end())};
    return sampleCurve<double>(curve, time, &cancellation);
}

AnimationSampleResult<document::Vec2d>
sampleAnimationCurve(const CompiledVec2Curve& curve, const core::RationalTime time) noexcept {
    if (std::ranges::any_of(curve.components,
                            [](const auto& component) { return !component.empty(); })) {
        const auto x = sampleAnimationComponentCurve(curve.id, curve.components[0],
                                                     curve.defaultValue.x, time);
        const auto y = sampleAnimationComponentCurve(curve.id, curve.components[1],
                                                     curve.defaultValue.y, time);
        if (!x || !y)
            return {std::nullopt, !x ? x.error : y.error, !x ? x.segmentStart : y.segmentStart};
        return {document::Vec2d{x.value.value_or(0.0), y.value.value_or(0.0)},
                AnimationSamplingError::None,
                x.segmentStart.has_value() ? x.segmentStart : y.segmentStart};
    }
    return sampleCurve<document::Vec2d>(curve, time, nullptr);
}

AnimationSampleResult<document::Vec2d>
sampleAnimationCurve(const CompiledVec2Curve& curve, const core::RationalTime time,
                     const CancellationToken& cancellation) noexcept {
    if (std::ranges::any_of(curve.components,
                            [](const auto& component) { return !component.empty(); })) {
        const auto x = sampleAnimationComponentCurve(curve.id, curve.components[0],
                                                     curve.defaultValue.x, time, cancellation);
        const auto y = sampleAnimationComponentCurve(curve.id, curve.components[1],
                                                     curve.defaultValue.y, time, cancellation);
        if (!x || !y)
            return {std::nullopt, !x ? x.error : y.error, !x ? x.segmentStart : y.segmentStart};
        return {document::Vec2d{x.value.value_or(0.0), y.value.value_or(0.0)},
                AnimationSamplingError::None,
                x.segmentStart.has_value() ? x.segmentStart : y.segmentStart};
    }
    return sampleCurve<document::Vec2d>(curve, time, &cancellation);
}

AnimationSampleResult<document::Vec3d>
sampleAnimationCurve(const CompiledVec3Curve& curve, const core::RationalTime time) noexcept {
    if (std::ranges::any_of(curve.components,
                            [](const auto& component) { return !component.empty(); })) {
        const auto x = sampleAnimationComponentCurve(curve.id, curve.components[0],
                                                     curve.defaultValue.x, time);
        const auto y = sampleAnimationComponentCurve(curve.id, curve.components[1],
                                                     curve.defaultValue.y, time);
        const auto z = sampleAnimationComponentCurve(curve.id, curve.components[2],
                                                     curve.defaultValue.z, time);
        if (!x || !y || !z)
            return {std::nullopt,
                    !x   ? x.error
                    : !y ? y.error
                         : z.error,
                    !x   ? x.segmentStart
                    : !y ? y.segmentStart
                         : z.segmentStart};
        return {
            document::Vec3d{x.value.value_or(0.0), y.value.value_or(0.0), z.value.value_or(0.0)},
            AnimationSamplingError::None,
            x.segmentStart.has_value()   ? x.segmentStart
            : y.segmentStart.has_value() ? y.segmentStart
                                         : z.segmentStart};
    }
    return sampleCurve<document::Vec3d>(curve, time, nullptr);
}

AnimationSampleResult<document::Vec3d>
sampleAnimationCurve(const CompiledVec3Curve& curve, const core::RationalTime time,
                     const CancellationToken& cancellation) noexcept {
    if (std::ranges::any_of(curve.components,
                            [](const auto& component) { return !component.empty(); })) {
        const auto x = sampleAnimationComponentCurve(curve.id, curve.components[0],
                                                     curve.defaultValue.x, time, cancellation);
        const auto y = sampleAnimationComponentCurve(curve.id, curve.components[1],
                                                     curve.defaultValue.y, time, cancellation);
        const auto z = sampleAnimationComponentCurve(curve.id, curve.components[2],
                                                     curve.defaultValue.z, time, cancellation);
        if (!x || !y || !z)
            return {std::nullopt,
                    !x   ? x.error
                    : !y ? y.error
                         : z.error,
                    !x   ? x.segmentStart
                    : !y ? y.segmentStart
                         : z.segmentStart};
        return {
            document::Vec3d{x.value.value_or(0.0), y.value.value_or(0.0), z.value.value_or(0.0)},
            AnimationSamplingError::None,
            x.segmentStart.has_value()   ? x.segmentStart
            : y.segmentStart.has_value() ? y.segmentStart
                                         : z.segmentStart};
    }
    return sampleCurve<document::Vec3d>(curve, time, &cancellation);
}

AnimationSampleResult<core::Color4d> sampleAnimationCurve(const CompiledColor4Curve& curve,
                                                          const core::RationalTime time) noexcept {
    if (std::ranges::any_of(curve.components,
                            [](const auto& component) { return !component.empty(); })) {
        const auto red = sampleAnimationComponentCurve(curve.id, curve.components[0],
                                                       curve.defaultValue.red, time);
        const auto green = sampleAnimationComponentCurve(curve.id, curve.components[1],
                                                         curve.defaultValue.green, time);
        const auto blue = sampleAnimationComponentCurve(curve.id, curve.components[2],
                                                        curve.defaultValue.blue, time);
        const auto alpha = sampleAnimationComponentCurve(curve.id, curve.components[3],
                                                         curve.defaultValue.alpha, time);
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
    return sampleCurve<core::Color4d>(curve, time, nullptr);
}

AnimationSampleResult<core::Color4d>
sampleAnimationCurve(const CompiledColor4Curve& curve, const core::RationalTime time,
                     const CancellationToken& cancellation) noexcept {
    if (std::ranges::any_of(curve.components,
                            [](const auto& component) { return !component.empty(); })) {
        const auto red = sampleAnimationComponentCurve(curve.id, curve.components[0],
                                                       curve.defaultValue.red, time, cancellation);
        const auto green = sampleAnimationComponentCurve(
            curve.id, curve.components[1], curve.defaultValue.green, time, cancellation);
        const auto blue = sampleAnimationComponentCurve(
            curve.id, curve.components[2], curve.defaultValue.blue, time, cancellation);
        const auto alpha = sampleAnimationComponentCurve(
            curve.id, curve.components[3], curve.defaultValue.alpha, time, cancellation);
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
    return sampleCurve<core::Color4d>(curve, time, &cancellation);
}

} // namespace bloom::runtime
