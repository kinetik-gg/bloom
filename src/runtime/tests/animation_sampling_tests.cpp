#include <bloom/runtime/animation_sampling.hpp>

#include <bloom/core/color.hpp>
#include <bloom/core/rational_interval.hpp>
#include <bloom/core/scalar_primitives.hpp>

#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>
#include <string_view>

namespace {

using namespace bloom;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

class RoundingModeGuard final {
  public:
    RoundingModeGuard() noexcept : original_(std::fegetround()) {}
    ~RoundingModeGuard() { static_cast<void>(std::fesetround(original_)); }

    RoundingModeGuard(const RoundingModeGuard&) = delete;
    RoundingModeGuard& operator=(const RoundingModeGuard&) = delete;

  private:
    int original_ = FE_TONEAREST;
};

[[nodiscard]] core::RationalTime time(const std::int64_t numerator,
                                      const std::int64_t denominator = 1) {
    const auto value = core::RationalTime::create(numerator, denominator);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

[[nodiscard]] runtime::CompiledScalarCurve scalarCurve() {
    return {document::AnimationCurveId::fromRaw(3),
            {{document::KeyframeId::fromRaw(10), time(0), -0.0,
              runtime::CompiledKeyframeInterpolation::Linear},
             {document::KeyframeId::fromRaw(11), time(1), 8.0,
              runtime::CompiledKeyframeInterpolation::Hold},
             {document::KeyframeId::fromRaw(12), time(2), 10.0,
              runtime::CompiledKeyframeInterpolation::Linear}}};
}

// --- Task S5, item 2: the EaseInOut segment -----------------------------------------------------
//
// EaseInOut is a cubic Bezier with FIXED symmetric handles at (1/3, 0) and (2/3, 1). Those x
// handles make the Bezier's x component exactly the identity in its own parameter, so the eased
// factor is the closed-form 3t^2 - 2t^3 of the EXACT rational interval factor t -- no root finding,
// no iteration. At the interval's exact thirds that gives values this test pins as rationals,
// derived here by hand rather than read off the implementation:
//
//   t = 1/3 -> 3(1/9) - 2(1/27) =  9/27 - 2/27 =  7/27
//   t = 2/3 -> 3(4/9) - 2(8/27) = 36/27 - 16/27 = 20/27
//
// Over a 0 -> 1 segment those ARE the sampled values, so the test states the contract in the
// smallest terms that can hold it.
[[nodiscard]] runtime::CompiledScalarCurve easedUnitCurve() {
    return {document::AnimationCurveId::fromRaw(7),
            {{document::KeyframeId::fromRaw(70), time(0), 0.0,
              runtime::CompiledKeyframeInterpolation::EaseInOut},
             {document::KeyframeId::fromRaw(71), time(1), 1.0,
              runtime::CompiledKeyframeInterpolation::Linear}}};
}

void testEaseInOutAtExactThirds(Expectations& expectations) {
    const auto curve = easedUnitCurve();
    const auto atStart = runtime::sampleAnimationCurve(curve, time(0));
    const auto atFirstThird = runtime::sampleAnimationCurve(curve, time(1, 3));
    const auto atMidpoint = runtime::sampleAnimationCurve(curve, time(1, 2));
    const auto atSecondThird = runtime::sampleAnimationCurve(curve, time(2, 3));
    const auto atEnd = runtime::sampleAnimationCurve(curve, time(1));

    expectations.expect(atStart && atStart.value == 0.0 && atEnd && atEnd.value == 1.0,
                        "an eased segment still reproduces its endpoints exactly");
    expectations.expect(atFirstThird && atFirstThird.value == 7.0 / 27.0,
                        "at the interval's first exact third an eased segment is exactly 7/27");
    expectations.expect(atSecondThird && atSecondThird.value == 20.0 / 27.0,
                        "at the second exact third it is exactly 20/27");
    expectations.expect(atMidpoint && atMidpoint.value == 0.5,
                        "and the symmetric handles put the exact midpoint at exactly one half");
    // The whole point of an ease: slower than linear near the start, faster in the middle.
    expectations.expect(atFirstThird && atFirstThird.value < 1.0 / 3.0,
                        "an eased segment lags a linear one over its first third");
    expectations.expect(atSecondThird && atSecondThird.value > 2.0 / 3.0,
                        "and leads it over its last third");

    // Hold still wins over Ease: the left key's mode alone decides the segment.
    const runtime::CompiledScalarCurve held{document::AnimationCurveId::fromRaw(8),
                                            {{document::KeyframeId::fromRaw(80), time(0), 0.0,
                                              runtime::CompiledKeyframeInterpolation::Hold},
                                             {document::KeyframeId::fromRaw(81), time(1), 1.0,
                                              runtime::CompiledKeyframeInterpolation::Linear}}};
    const auto heldMid = runtime::sampleAnimationCurve(held, time(1, 2));
    expectations.expect(heldMid && heldMid.value == 0.0,
                        "a Hold segment is unaffected by the eased path");

    // A final key may not carry anything but Linear; an eased one is refused rather than
    // normalized.
    const runtime::CompiledScalarCurve easedFinal{
        document::AnimationCurveId::fromRaw(9),
        {{document::KeyframeId::fromRaw(90), time(0), 0.0,
          runtime::CompiledKeyframeInterpolation::Linear},
         {document::KeyframeId::fromRaw(91), time(1), 1.0,
          runtime::CompiledKeyframeInterpolation::EaseInOut}}};
    const auto refused = runtime::sampleAnimationCurve(easedFinal, time(1, 2));
    expectations.expect(!refused && refused.error ==
                                        runtime::AnimationSamplingError::UnsupportedInterpolation,
                        "a non-canonical final interpolation is refused, eased or not");
}

// --- Task S5, item 1: the Color4 curve ----------------------------------------------------------
void testColor4Sampling(Expectations& expectations) {
    const runtime::CompiledColor4Curve curve{
        document::AnimationCurveId::fromRaw(11),
        {{document::KeyframeId::fromRaw(110), time(0), core::Color4d{0.0, 0.25, 1.0, 0.0},
          runtime::CompiledKeyframeInterpolation::Linear},
         {document::KeyframeId::fromRaw(111), time(1), core::Color4d{1.0, 0.75, -1.0, 1.0},
          runtime::CompiledKeyframeInterpolation::EaseInOut},
         {document::KeyframeId::fromRaw(112), time(2), core::Color4d{2.0, 0.75, -1.0, 1.0},
          runtime::CompiledKeyframeInterpolation::Linear}}};

    const auto atFirst = runtime::sampleAnimationCurve(curve, time(0));
    expectations.expect(atFirst && atFirst.value == core::Color4d{0.0, 0.25, 1.0, 0.0},
                        "a colour curve returns its first key bit-for-bit at and before it");

    // Every channel takes the SAME shared factor, including a negative one and an HDR one.
    const auto halfway = runtime::sampleAnimationCurve(curve, time(1, 2));
    expectations.expect(halfway && halfway.value == core::Color4d{0.5, 0.5, 0.0, 0.5},
                        "one shared factor mixes all four channels, negative and HDR included");

    // The eased segment applies to a colour exactly as it does to a scalar, and a channel that does
    // not change between the two keys stays exactly where it was.
    const auto eased = runtime::sampleAnimationCurve(curve, time(4, 3));
    expectations.expect(eased && eased.value == core::Color4d{1.0 + (7.0 / 27.0), 0.75, -1.0, 1.0},
                        "an eased colour segment uses the same 7/27 factor at its first third");

    // The authoring-colour domain: alpha outside [0, 1] is not a representable authoring colour, so
    // the whole curve is invalid rather than silently clamped.
    const runtime::CompiledColor4Curve invalid{
        document::AnimationCurveId::fromRaw(12),
        {{document::KeyframeId::fromRaw(120), time(0), core::Color4d{0.0, 0.0, 0.0, 2.0},
          runtime::CompiledKeyframeInterpolation::Linear}}};
    const auto refused = runtime::sampleAnimationCurve(invalid, time(0));
    expectations.expect(!refused && refused.error == runtime::AnimationSamplingError::InvalidCurve,
                        "a colour key whose alpha leaves the unit interval invalidates the curve");
}

void testSampling(Expectations& expectations) {
    const auto curve = scalarCurve();
    const auto before = runtime::sampleAnimationCurve(curve, time(-1));
    const auto exact = runtime::sampleAnimationCurve(curve, time(1));
    const auto linear = runtime::sampleAnimationCurve(curve, time(1, 4));
    const auto hold = runtime::sampleAnimationCurve(curve, time(3, 2));
    const auto after = runtime::sampleAnimationCurve(curve, time(3));

    expectations.expect(before && before.value.has_value() && *before.value == 0.0 &&
                            std::signbit(*before.value),
                        "pre-range sampling preserves the exact first endpoint including sign");
    expectations.expect(exact && exact.value == 8.0 &&
                            exact.segmentStart == document::KeyframeId::fromRaw(11),
                        "an exact key returns its stored value and identity");
    expectations.expect(linear && linear.value == 2.0,
                        "linear interpolation uses the exact rational factor");
    expectations.expect(hold && hold.value == 8.0,
                        "Hold interpolation retains the left key on its half-open segment");
    expectations.expect(after && after.value == 10.0,
                        "post-range sampling clamps to the final endpoint");
}

void testVec2AndExtremeTime(Expectations& expectations) {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const runtime::CompiledVec2Curve curve{document::AnimationCurveId::fromRaw(4),
                                           {{document::KeyframeId::fromRaw(20),
                                             time(0),
                                             {0.0, 4.0},
                                             runtime::CompiledKeyframeInterpolation::Linear},
                                            {document::KeyframeId::fromRaw(21),
                                             time(maximum),
                                             {static_cast<double>(maximum), -4.0},
                                             runtime::CompiledKeyframeInterpolation::Linear}}};
    const auto sample = runtime::sampleAnimationCurve(curve, time(1, maximum));
    expectations.expect(sample && sample.value.has_value() && sample.value->x == 0x1p-63 &&
                            sample.value->y == 4.0,
                        "Vec2 components share one correctly rounded extreme time factor");
}

void testIndependentComponentSampling(Expectations& expectations) {
    runtime::CompiledVec2Curve vector;
    vector.id = document::AnimationCurveId::fromRaw(40);
    vector.defaultValue = document::Vec2d{10.0, 20.0};
    vector.components[0] = {
        {document::KeyframeId::fromRaw(400), time(0), 0.0,
         runtime::CompiledKeyframeInterpolation::Linear},
        {document::KeyframeId::fromRaw(401), time(2), 4.0,
         runtime::CompiledKeyframeInterpolation::Linear},
    };
    vector.components[1] = {
        {document::KeyframeId::fromRaw(402), time(1), 8.0,
         runtime::CompiledKeyframeInterpolation::Linear},
    };

    const auto beforeYKey = runtime::sampleAnimationCurve(vector, time(1, 2));
    expectations.expect(beforeYKey && beforeYKey.value == document::Vec2d{1.0, 8.0},
                        "a Vec2 component samples its own pre-range endpoint independently");
    const auto afterYKey = runtime::sampleAnimationCurve(vector, time(3, 2));
    expectations.expect(afterYKey && afterYKey.value == document::Vec2d{3.0, 8.0},
                        "keyed Vec2 components sample their own times and interpolation");

    runtime::CompiledColor4Curve color;
    color.id = document::AnimationCurveId::fromRaw(41);
    color.defaultValue = core::Color4d{0.1, 0.2, 0.3, 1.0};
    color.components[0] = {
        {document::KeyframeId::fromRaw(410), time(0), 0.0,
         runtime::CompiledKeyframeInterpolation::Hold},
        {document::KeyframeId::fromRaw(411), time(1), 1.0,
         runtime::CompiledKeyframeInterpolation::Linear},
    };
    color.components[1] = {
        {document::KeyframeId::fromRaw(412), time(0), 0.5,
         runtime::CompiledKeyframeInterpolation::Linear},
        {document::KeyframeId::fromRaw(413), time(1), 0.75,
         runtime::CompiledKeyframeInterpolation::Linear},
    };
    const auto colorSample = runtime::sampleAnimationCurve(color, time(1, 2));
    expectations.expect(colorSample && colorSample.value == core::Color4d{0.0, 0.625, 0.3, 1.0},
                        "colour components preserve per-component Hold, Linear and default paths");
}

[[nodiscard]] std::uint64_t bits(const double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] runtime::CompiledScalarCurve easedSegment(const double from, const double to) {
    return {document::AnimationCurveId::fromRaw(7),
            {{document::KeyframeId::fromRaw(20), time(0), from,
              runtime::CompiledKeyframeInterpolation::EaseInOut},
             {document::KeyframeId::fromRaw(21), time(1), to,
              runtime::CompiledKeyframeInterpolation::Linear}}};
}

// The pre-handle eased path, reassembled from the two core primitives it was always made of. It is
// the reference the default-handle curve has to match BIT FOR BIT, which is what lets
// kAnimationSamplingSemanticsVersion stay at 2 while keys gain handles.
[[nodiscard]] bool preHandleEasedValue(const core::RationalTime at, const double from,
                                       const double to, double& out) {
    const auto factor = core::rationalIntervalFactor(at, time(0), time(1));
    if (!factor || factor.value() == nullptr) {
        return false;
    }
    const std::array smoothInputs{0.0, 1.0, *factor.value()};
    const auto smooth = core::primitives::evaluateScalar(
        core::primitives::ScalarPrimitive::Smoothstep, smoothInputs);
    if (!smooth || smooth.value() == nullptr) {
        return false;
    }
    const std::array mixInputs{from, to, *smooth.value()};
    const auto mixed =
        core::primitives::evaluateScalar(core::primitives::ScalarPrimitive::Mix, mixInputs);
    if (!mixed || mixed.value() == nullptr) {
        return false;
    }
    out = *mixed.value();
    return true;
}

void testEaseHandles(Expectations& expectations) {
    const auto implicit = easedSegment(-3.5, 11.25);
    auto explicitDefaults = implicit;
    explicitDefaults.keyframes[0].outgoingHandle = document::KeyframeHandle{};
    explicitDefaults.keyframes[1].incomingHandle = document::KeyframeHandle{};
    bool identical = true;
    for (std::int64_t step = 1; step < 97; ++step) {
        const auto at = time(step, 97);
        double reference = 0.0;
        if (!preHandleEasedValue(at, -3.5, 11.25, reference)) {
            identical = false;
            break;
        }
        const auto sampled = runtime::sampleAnimationCurve(implicit, at);
        const auto sampledExplicit = runtime::sampleAnimationCurve(explicitDefaults, at);
        if (!sampled.value.has_value() || !sampledExplicit.value.has_value() ||
            bits(*sampled.value) != bits(reference) ||
            bits(*sampledExplicit.value) != bits(reference)) {
            identical = false;
            break;
        }
    }
    expectations.expect(identical,
                        "a default-handle eased curve samples bit-for-bit as the pre-handle path");

    // A value handle has a closed form the bisection never touches: with both handle times default
    // the x cubic is the identity, so the curve parameter IS the interval factor. Lifting the left
    // key's outgoing offset to +1 over a 0 -> 1 segment makes the midpoint exactly 7/8.
    auto valueHandled = easedSegment(0.0, 1.0);
    valueHandled.keyframes[0].outgoingHandle.value = 1.0;
    const auto midpoint = runtime::sampleAnimationCurve(valueHandled, time(1, 2));
    expectations.expect(midpoint.value.has_value() && bits(*midpoint.value) == bits(0.875),
                        "a value handle follows the cubic Bezier closed form exactly");

    auto timeHandled = easedSegment(0.0, 1.0);
    timeHandled.keyframes[0].outgoingHandle.time = 0.9;
    timeHandled.keyframes[1].incomingHandle.time = 0.1;
    const auto first = runtime::sampleAnimationCurve(timeHandled, time(1, 3));
    const auto second = runtime::sampleAnimationCurve(timeHandled, time(1, 3));
    expectations.expect(first.value.has_value() && second.value.has_value() &&
                            bits(*first.value) == bits(*second.value),
                        "the handle-time inversion is deterministic to the bit");
    bool monotone = true;
    double previous = -1.0;
    for (std::int64_t step = 0; step <= 64; ++step) {
        const auto sampled = runtime::sampleAnimationCurve(timeHandled, time(step, 64));
        if (!sampled.value.has_value() || *sampled.value < previous) {
            monotone = false;
            break;
        }
        previous = *sampled.value;
    }
    expectations.expect(monotone, "a retimed eased segment stays monotone in the request time");

    auto outOfRange = easedSegment(0.0, 1.0);
    outOfRange.keyframes[0].outgoingHandle.time = 1.5;
    expectations.expect(runtime::sampleAnimationCurve(outOfRange, time(1, 2)).error ==
                            runtime::AnimationSamplingError::InvalidCurve,
                        "a handle time outside [0, 1] is rejected as an invalid curve");
    auto nonFiniteHandle = easedSegment(0.0, 1.0);
    nonFiniteHandle.keyframes[1].incomingHandle.value = std::numeric_limits<double>::quiet_NaN();
    expectations.expect(runtime::sampleAnimationCurve(nonFiniteHandle, time(1, 2)).error ==
                            runtime::AnimationSamplingError::InvalidCurve,
                        "a non-finite handle offset is rejected as an invalid curve");

    // Handles do not loosen the canonical final key: its outgoing mode is still Linear, and the
    // handles it carries change nothing about the value returned at or after it.
    auto finalHandled = easedSegment(0.0, 1.0);
    finalHandled.keyframes[1].incomingHandle.value = 4.0;
    finalHandled.keyframes[1].outgoingHandle.value = 4.0;
    const auto atEnd = runtime::sampleAnimationCurve(finalHandled, time(1));
    expectations.expect(atEnd.value.has_value() && bits(*atEnd.value) == bits(1.0),
                        "the final key keeps its own value however its handles are placed");
    finalHandled.keyframes[1].outgoingInterpolation =
        runtime::CompiledKeyframeInterpolation::EaseInOut;
    expectations.expect(runtime::sampleAnimationCurve(finalHandled, time(1, 2)).error ==
                            runtime::AnimationSamplingError::UnsupportedInterpolation,
                        "the final key stays canonical Linear");

    // A handle belongs to a scalar segment only where the LEFT key is eased. Hold and Linear keep
    // their own shapes and the handles simply persist.
    auto linearHandled = easedSegment(0.0, 1.0);
    linearHandled.keyframes[0].outgoingInterpolation =
        runtime::CompiledKeyframeInterpolation::Linear;
    linearHandled.keyframes[0].outgoingHandle.value = 1.0;
    const auto linearMidpoint = runtime::sampleAnimationCurve(linearHandled, time(1, 2));
    expectations.expect(linearMidpoint.value.has_value() &&
                            bits(*linearMidpoint.value) == bits(0.5),
                        "Linear ignores the handles it carries");
    auto holdHandled = linearHandled;
    holdHandled.keyframes[0].outgoingInterpolation = runtime::CompiledKeyframeInterpolation::Hold;
    const auto holdMidpoint = runtime::sampleAnimationCurve(holdHandled, time(1, 2));
    expectations.expect(holdMidpoint.value.has_value() && bits(*holdMidpoint.value) == bits(0.0),
                        "Hold ignores the handles it carries");
}

void testValidationAndEnvironment(Expectations& expectations) {
    auto curve = scalarCurve();
    curve.keyframes.back().outgoingInterpolation = runtime::CompiledKeyframeInterpolation::Hold;
    expectations.expect(runtime::sampleAnimationCurve(curve, time(1)).error ==
                            runtime::AnimationSamplingError::UnsupportedInterpolation,
                        "a noncanonical final interpolation is rejected");

    curve = scalarCurve();
    curve.keyframes[1].time = curve.keyframes[0].time;
    expectations.expect(runtime::sampleAnimationCurve(curve, time(1)).error ==
                            runtime::AnimationSamplingError::InvalidCurve,
                        "duplicate or unordered key times are rejected");

    curve = scalarCurve();
    curve.keyframes[1].value = std::numeric_limits<double>::infinity();
    expectations.expect(runtime::sampleAnimationCurve(curve, time(1)).error ==
                            runtime::AnimationSamplingError::InvalidCurve,
                        "non-finite curve values fail before interval selection");

    RoundingModeGuard guard;
    if (std::fesetround(FE_DOWNWARD) == 0) {
        curve = scalarCurve();
        expectations.expect(
            runtime::sampleAnimationCurve(curve, time(0)).error ==
                runtime::AnimationSamplingError::UnsupportedFloatingPointEnvironment,
            "endpoint and Hold paths enforce the same reference environment as Linear");
    }
}

} // namespace

int main() {
    Expectations expectations;
    // Task S5 moved this 1 -> 2: EaseInOut lets sampling produce a value no version-1 sampler
    // could, and the Color4 curve table added a third sampled value kind.
    expectations.expect(runtime::kAnimationSamplingSemanticsVersion == 2,
                        "animation sampling semantics are explicitly versioned");
    testSampling(expectations);
    testVec2AndExtremeTime(expectations);
    testIndependentComponentSampling(expectations);
    testEaseInOutAtExactThirds(expectations);
    testEaseHandles(expectations);
    testColor4Sampling(expectations);
    testValidationAndEnvironment(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
