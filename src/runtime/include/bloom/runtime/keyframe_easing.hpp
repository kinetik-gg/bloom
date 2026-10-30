#pragma once

// Blender's keyframe easing equations (Robert Penner's, in the forms Blender's BLI_easing
// implements, including its exact-endpoint Expo and its blended Elastic), evaluated in double with
// a fixed operation order and no libm transcendental call. The animation sampler is the only
// production caller; every route that resolves a parameter -- viewer, RAM preview, still, range
// and video export, headless render -- samples curves through it on the host, so one function
// defines every route's value. See docs/architecture/animation-and-time.md, "Easing equations".

#include <bloom/document/animation.hpp>
#include <bloom/runtime/compiled_curves.hpp>

namespace bloom::runtime {

// Resolves Auto the way Blender's fcurve evaluation does: Out for Back, Bounce and Elastic, In for
// every other equation. An explicit mode is returned unchanged.
[[nodiscard]] document::KeyframeEasing
resolveKeyframeEasing(CompiledKeyframeInterpolation mode, document::KeyframeEasing easing) noexcept;

[[nodiscard]] constexpr bool
isEasingInterpolation(const CompiledKeyframeInterpolation mode) noexcept {
    return mode >= CompiledKeyframeInterpolation::Sine &&
           mode <= CompiledKeyframeInterpolation::Elastic;
}

// The value of one eased segment. `factor` is the segment's exact interval factor in [0, 1],
// `begin` and `end` are the two keys' values, and `durationSeconds` (> 0) is the segment length,
// which only Elastic reads (its period and blend window are absolute times, as in Blender). A
// non-easing `mode` returns `begin`.
[[nodiscard]] double sampleEasingEquation(CompiledKeyframeInterpolation mode,
                                          const document::KeyframeEasingParameters& parameters,
                                          double begin, double end, double durationSeconds,
                                          double factor) noexcept;

namespace detail {
// The deterministic transcendental kernels the equations use, exposed for their accuracy tests.
// Each is written out of + - * /, floor, ldexp and sqrt (all exactly rounded by IEEE 754) in a
// fixed order, so the result is a pure function of the operand on every platform.
[[nodiscard]] double sinTurns(double turns) noexcept; // sin(2 pi turns)
[[nodiscard]] double cosTurns(double turns) noexcept; // cos(2 pi turns)
[[nodiscard]] double exp2(double exponent) noexcept;
[[nodiscard]] double asinTurns(double value) noexcept; // asin(value) / (2 pi), value in [-1, 1]
} // namespace detail

} // namespace bloom::runtime
