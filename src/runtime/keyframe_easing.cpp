#include <bloom/runtime/keyframe_easing.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

// Blender's formulas are float; these are the same expressions in double, in the same operation
// order, with `time / duration` already reduced to the exact interval factor `u`. Only Elastic
// needs the absolute segment duration back, exactly where Blender multiplies `time * duration`.

namespace bloom::runtime {
namespace detail {
namespace {

inline constexpr double kHalfPi = 1.5707963267948966;
inline constexpr double kLn2 = 0.6931471805599453;

// Taylor coefficients, each a compile-time correctly rounded quotient.
inline constexpr double kSin3 = -1.0 / 6.0;
inline constexpr double kSin5 = 1.0 / 120.0;
inline constexpr double kSin7 = -1.0 / 5040.0;
inline constexpr double kSin9 = 1.0 / 362880.0;
inline constexpr double kSin11 = -1.0 / 39916800.0;
inline constexpr double kSin13 = 1.0 / 6227020800.0;
inline constexpr double kSin15 = -1.0 / 1307674368000.0;
inline constexpr double kCos2 = -1.0 / 2.0;
inline constexpr double kCos4 = 1.0 / 24.0;
inline constexpr double kCos6 = -1.0 / 720.0;
inline constexpr double kCos8 = 1.0 / 40320.0;
inline constexpr double kCos10 = -1.0 / 3628800.0;
inline constexpr double kCos12 = 1.0 / 479001600.0;
inline constexpr double kCos14 = -1.0 / 87178291200.0;
inline constexpr double kCos16 = 1.0 / 20922789888000.0;

// |angle| <= pi / 4: the truncated series errs by under 5e-17.
[[nodiscard]] double sinKernel(const double angle) noexcept {
    const double square = angle * angle;
    const double tail =
        kSin3 +
        square *
            (kSin5 +
             square * (kSin7 +
                       square * (kSin9 + square * (kSin11 + square * (kSin13 + square * kSin15)))));
    return angle + (angle * square) * tail;
}

[[nodiscard]] double cosKernel(const double angle) noexcept {
    const double square = angle * angle;
    const double tail =
        kCos2 +
        square *
            (kCos4 +
             square *
                 (kCos6 +
                  square * (kCos8 +
                            square * (kCos10 +
                                      square * (kCos12 + square * (kCos14 + square * kCos16))))));
    return 1.0 + square * tail;
}

struct Reduced {
    double angle; // in [-pi/4, pi/4]
    int quadrant; // 0..3
};

// Quarter turns are reduced exactly: scaling by 4 is exact, and subtracting the nearest integer
// from a double is exact too, so the only rounding is the final scale to radians.
[[nodiscard]] Reduced reduceTurns(const double turns) noexcept {
    const double quarters = turns * 4.0;
    const double nearest = std::floor(quarters + 0.5);
    const double fraction = quarters - nearest;
    const double quadrant = nearest - 4.0 * std::floor(nearest * 0.25);
    return {fraction * kHalfPi, static_cast<int>(quadrant)};
}

} // namespace

double sinTurns(const double turns) noexcept {
    if (!std::isfinite(turns))
        return turns - turns; // NaN
    const auto reduced = reduceTurns(turns);
    switch (reduced.quadrant) {
    case 0:
        return sinKernel(reduced.angle);
    case 1:
        return cosKernel(reduced.angle);
    case 2:
        return -sinKernel(reduced.angle);
    default:
        return -cosKernel(reduced.angle);
    }
}

double cosTurns(const double turns) noexcept {
    if (!std::isfinite(turns))
        return turns - turns;
    const auto reduced = reduceTurns(turns);
    switch (reduced.quadrant) {
    case 0:
        return cosKernel(reduced.angle);
    case 1:
        return -sinKernel(reduced.angle);
    case 2:
        return -cosKernel(reduced.angle);
    default:
        return sinKernel(reduced.angle);
    }
}

double exp2(const double exponent) noexcept {
    if (std::isnan(exponent))
        return exponent;
    if (exponent < -1100.0)
        return 0.0;
    if (exponent > 1100.0)
        return HUGE_VAL;
    const double whole = std::floor(exponent);
    const double y = (exponent - whole) * kLn2; // in [0, ln 2)
    // e^y by Horner over 1/k!, k <= 20: the truncation is below 1e-22.
    double sum = 1.0;
    for (int k = 20; k >= 1; --k)
        sum = 1.0 + (y / static_cast<double>(k)) * sum;
    return std::ldexp(sum, static_cast<int>(whole));
}

double asinTurns(const double value) noexcept {
    if (!(value >= -1.0 && value <= 1.0))
        return std::numeric_limits<double>::quiet_NaN();
    const double magnitude = value < 0.0 ? -value : value;
    // asin(a) = pi/2 - 2 asin(sqrt((1 - a) / 2)) folds a > 1/2 into the series' fast range;
    // 1 - a is exact there.
    const bool folded = magnitude > 0.5;
    const double y = folded ? std::sqrt((1.0 - magnitude) * 0.5) : magnitude;
    const double square = y * y;
    double power = y;
    double coefficient = 1.0;
    double series = y;
    for (int k = 1; k <= 30; ++k) {
        coefficient = coefficient * (static_cast<double>(2 * k - 1) / static_cast<double>(2 * k));
        power = power * square;
        series = series + (coefficient * power) / static_cast<double>(2 * k + 1);
    }
    // In turns: asin / (2 pi). pi/2 is a quarter turn.
    constexpr double kInverseTwoPi = 0.15915494309189535;
    const double turns = folded ? 0.25 - (2.0 * series) * kInverseTwoPi : series * kInverseTwoPi;
    return value < 0.0 ? -turns : turns;
}

} // namespace detail

namespace {

using document::KeyframeEasing;

// Blender's pow_min / pow_scale: the Expo curve is shifted and rescaled so it starts and ends
// exactly on its keys.
inline constexpr double kExpoMinimum = 0.0009765625; // 2^-10
inline constexpr double kExpoScale = 1.0 / (1.0 - 0.0009765625);

[[nodiscard]] double expoIn(const double u, const double begin, const double change) noexcept {
    if (u == 0.0)
        return begin;
    return change * (detail::exp2(10.0 * (u - 1.0)) - kExpoMinimum) * kExpoScale + begin;
}
[[nodiscard]] double expoOut(const double u, const double begin, const double change) noexcept {
    if (u == 0.0)
        return begin;
    return change * (1.0 - (detail::exp2(-10.0 * u) - kExpoMinimum) * kExpoScale) + begin;
}

[[nodiscard]] double bounceOut(double u, const double begin, const double change) noexcept {
    if (u < (1.0 / 2.75))
        return change * (7.5625 * u * u) + begin;
    if (u < (2.0 / 2.75)) {
        u -= (1.5 / 2.75);
        return change * ((7.5625 * u) * u + 0.75) + begin;
    }
    if (u < (2.5 / 2.75)) {
        u -= (2.25 / 2.75);
        return change * ((7.5625 * u) * u + 0.9375) + begin;
    }
    u -= (2.625 / 2.75);
    return change * ((7.5625 * u) * u + 0.984375) + begin;
}
[[nodiscard]] double bounceIn(const double u, const double begin, const double change) noexcept {
    return change - bounceOut(1.0 - u, 0.0, change) + begin;
}

// Blender's elastic_blend(): below |change| the amplitude scales the swing, faded in over the
// first quarter period so the curve does not jump where it leaves the key.
[[nodiscard]] double elasticBlend(const double time, const double change, const double duration,
                                  const double amplitude, const double s, double f) noexcept {
    if (change != 0.0) {
        const double window = std::fabs(s);
        if (amplitude != 0.0)
            f *= amplitude / std::fabs(change);
        else
            f = 0.0;
        if (std::fabs(time * duration) < window) {
            const double l = std::fabs(time * duration) / window;
            f = (f * l) + (1.0 - l);
        }
    }
    return f;
}

// Shared setup of Blender's three elastic forms: the period, phase offset `s`, blend factor `f`
// and effective amplitude.
struct ElasticShape {
    double period;
    double s;
    double f;
    double amplitude;
};
[[nodiscard]] ElasticShape elasticShape(const double time, const double change,
                                        const double duration, const double amplitude,
                                        const double period,
                                        const double automaticPeriod) noexcept {
    ElasticShape shape{period, 0.0, 1.0, amplitude};
    if (shape.period == 0.0)
        shape.period = duration * automaticPeriod;
    if (amplitude == 0.0 || amplitude < std::fabs(change)) {
        shape.s = shape.period / 4.0;
        shape.f = elasticBlend(time, change, duration, amplitude, shape.s, shape.f);
        shape.amplitude = change;
    } else {
        // period / (2 pi) * asin(change / amplitude), with asin already in turns.
        shape.s = shape.period * detail::asinTurns(change / amplitude);
    }
    return shape;
}
// amplitude * 2^(10 time) * sin((time * duration - s) * 2 pi / period)
[[nodiscard]] double elasticSwing(const ElasticShape& shape, const double time,
                                  const double duration) noexcept {
    return shape.amplitude * detail::exp2(10.0 * time) *
           detail::sinTurns((time * duration - shape.s) / shape.period);
}

[[nodiscard]] double elastic(const KeyframeEasing easing, const double u, const double begin,
                             const double change, const double duration,
                             const document::KeyframeEasingParameters& parameters) noexcept {
    if (u == 0.0)
        return begin;
    if (easing == KeyframeEasing::InOut) {
        double time = u * 2.0;
        if (time == 2.0)
            return begin + change;
        time -= 1.0;
        const auto shape = elasticShape(time, change, duration, parameters.amplitude,
                                        parameters.period, 0.3 * 1.5);
        if (time < 0.0)
            return (shape.f * -0.5) * elasticSwing(shape, time, duration) + begin;
        time = -time;
        return (shape.f * 0.5) * elasticSwing(shape, time, duration) + change + begin;
    }
    if (u == 1.0)
        return begin + change;
    if (easing == KeyframeEasing::In) {
        const double time = u - 1.0;
        const auto shape =
            elasticShape(time, change, duration, parameters.amplitude, parameters.period, 0.3);
        return (-shape.f * elasticSwing(shape, time, duration)) + begin;
    }
    const double time = -u;
    const auto shape =
        elasticShape(time, change, duration, parameters.amplitude, parameters.period, 0.3);
    return (shape.f * elasticSwing(shape, time, duration)) + change + begin;
}

} // namespace

KeyframeEasing resolveKeyframeEasing(const CompiledKeyframeInterpolation mode,
                                     const KeyframeEasing easing) noexcept {
    if (easing != KeyframeEasing::Auto)
        return easing;
    return mode == CompiledKeyframeInterpolation::Back ||
                   mode == CompiledKeyframeInterpolation::Bounce ||
                   mode == CompiledKeyframeInterpolation::Elastic
               ? KeyframeEasing::Out
               : KeyframeEasing::In;
}

double sampleEasingEquation(const CompiledKeyframeInterpolation mode,
                            const document::KeyframeEasingParameters& parameters,
                            const double begin, const double end, const double durationSeconds,
                            const double u) noexcept {
    const double c = end - begin;
    const double b = begin;
    const auto easing = resolveKeyframeEasing(mode, parameters.easing);
    const bool in = easing == KeyframeEasing::In;
    const bool out = easing == KeyframeEasing::Out;
    switch (mode) {
    case CompiledKeyframeInterpolation::Sine:
        if (in)
            return -c * detail::cosTurns(u * 0.25) + c + b;
        if (out)
            return c * detail::sinTurns(u * 0.25) + b;
        return -c / 2.0 * (detail::cosTurns(u * 0.5) - 1.0) + b;
    case CompiledKeyframeInterpolation::Quad: {
        if (in)
            return c * u * u + b;
        if (out)
            return -c * u * (u - 2.0) + b;
        double t = u * 2.0;
        if (t < 1.0)
            return c / 2.0 * t * t + b;
        t -= 1.0;
        return -c / 2.0 * (t * (t - 2.0) - 1.0) + b;
    }
    case CompiledKeyframeInterpolation::Cubic: {
        if (in)
            return c * u * u * u + b;
        if (out) {
            const double t = u - 1.0;
            return c * (t * t * t + 1.0) + b;
        }
        double t = u * 2.0;
        if (t < 1.0)
            return c / 2.0 * t * t * t + b;
        t -= 2.0;
        return c / 2.0 * (t * t * t + 2.0) + b;
    }
    case CompiledKeyframeInterpolation::Quart: {
        if (in)
            return c * u * u * u * u + b;
        if (out) {
            const double t = u - 1.0;
            return -c * (t * t * t * t - 1.0) + b;
        }
        double t = u * 2.0;
        if (t < 1.0)
            return c / 2.0 * t * t * t * t + b;
        t -= 2.0;
        return -c / 2.0 * (t * t * t * t - 2.0) + b;
    }
    case CompiledKeyframeInterpolation::Quint: {
        if (in)
            return c * u * u * u * u * u + b;
        if (out) {
            const double t = u - 1.0;
            return c * (t * t * t * t * t + 1.0) + b;
        }
        double t = u * 2.0;
        if (t < 1.0)
            return c / 2.0 * t * t * t * t * t + b;
        t -= 2.0;
        return c / 2.0 * (t * t * t * t * t + 2.0) + b;
    }
    case CompiledKeyframeInterpolation::Expo:
        if (in)
            return expoIn(u, b, c);
        if (out)
            return expoOut(u, b, c);
        if (u <= 0.5)
            return expoIn(u * 2.0, b, c / 2.0);
        return expoOut(u * 2.0 - 1.0, b + c / 2.0, c / 2.0);
    case CompiledKeyframeInterpolation::Circ: {
        if (in)
            return -c * (std::sqrt(1.0 - u * u) - 1.0) + b;
        if (out) {
            const double t = u - 1.0;
            return c * std::sqrt(1.0 - t * t) + b;
        }
        double t = u * 2.0;
        if (t < 1.0)
            return -c / 2.0 * (std::sqrt(1.0 - t * t) - 1.0) + b;
        t -= 2.0;
        return c / 2.0 * (std::sqrt(1.0 - t * t) + 1.0) + b;
    }
    case CompiledKeyframeInterpolation::Back: {
        double overshoot = parameters.back;
        if (in)
            return c * u * u * ((overshoot + 1.0) * u - overshoot) + b;
        if (out) {
            const double t = u - 1.0;
            return c * (t * t * ((overshoot + 1.0) * t + overshoot) + 1.0) + b;
        }
        overshoot *= 1.525;
        double t = u * 2.0;
        if (t < 1.0)
            return c / 2.0 * (t * t * ((overshoot + 1.0) * t - overshoot)) + b;
        t -= 2.0;
        return c / 2.0 * (t * t * ((overshoot + 1.0) * t + overshoot) + 2.0) + b;
    }
    case CompiledKeyframeInterpolation::Bounce:
        if (in)
            return bounceIn(u, b, c);
        if (out)
            return bounceOut(u, b, c);
        if (u < 0.5)
            return bounceIn(u * 2.0, 0.0, c) * 0.5 + b;
        return bounceOut(u * 2.0 - 1.0, 0.0, c) * 0.5 + c * 0.5 + b;
    case CompiledKeyframeInterpolation::Elastic:
        return elastic(easing, u, b, c, durationSeconds, parameters);
    case CompiledKeyframeInterpolation::Hold:
    case CompiledKeyframeInterpolation::Linear:
    case CompiledKeyframeInterpolation::EaseInOut:
        break;
    }
    return begin;
}

} // namespace bloom::runtime
