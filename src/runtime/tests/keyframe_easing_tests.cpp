#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/curve_compilation.hpp>
#include <bloom/runtime/keyframe_easing.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>

namespace {

using namespace bloom;
using Mode = runtime::CompiledKeyframeInterpolation;
using Ease = document::KeyframeEasing;

struct ReferenceRow {
    Mode mode;
    Ease ease;
    double parameter;
    std::int64_t numerator;
    std::int64_t denominator;
    double value;
};

struct ElasticRow {
    Ease ease;
    double amplitude;
    double period;
    double begin;
    double end;
    std::int64_t durationNumerator;
    std::int64_t durationDenominator;
    std::int64_t numerator;
    std::int64_t denominator;
    double value;
};

#include "keyframe_easing_reference.inc"

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] core::RationalTime time(const std::int64_t numerator,
                                      const std::int64_t denominator = 1) {
    const auto value = core::RationalTime::create(numerator, denominator);
    if (!value)
        std::abort();
    return *value;
}

[[nodiscard]] std::uint64_t bits(const double value) { return std::bit_cast<std::uint64_t>(value); }

[[nodiscard]] runtime::CompiledScalarCurve segment(const Mode mode,
                                                   const document::KeyframeEasingParameters& easing,
                                                   const double begin, const double end,
                                                   const core::RationalTime duration) {
    runtime::CompiledScalarCurve curve{document::AnimationCurveId::fromRaw(1), {}};
    curve.keyframes.push_back(
        {document::KeyframeId::fromRaw(1), time(0), begin, mode, {}, {}, easing});
    curve.keyframes.push_back(
        {document::KeyframeId::fromRaw(2), duration, end, Mode::Linear, {}, {}, {}});
    return curve;
}

[[nodiscard]] std::string label(const ReferenceRow& row) {
    return "mode " + std::to_string(static_cast<int>(row.mode)) + " ease " +
           std::to_string(static_cast<int>(row.ease)) + " at " + std::to_string(row.numerator) +
           "/" + std::to_string(row.denominator);
}

void testKernels(Expectations& expectations) {
    double worst = 0.0;
    for (int step = -4000; step <= 4000; ++step) {
        const double turns = static_cast<double>(step) / 997.0;
        const double radians = turns * 6.283185307179586;
        worst = std::fmax(worst, std::fabs(runtime::detail::sinTurns(turns) - std::sin(radians)));
        worst = std::fmax(worst, std::fabs(runtime::detail::cosTurns(turns) - std::cos(radians)));
    }
    expectations.expect(worst < 1e-14, "sinTurns/cosTurns agree with libm to 1e-14");
    expectations.expect(runtime::detail::sinTurns(0.25) == 1.0 &&
                            runtime::detail::cosTurns(0.0) == 1.0 &&
                            runtime::detail::sinTurns(0.0) == 0.0,
                        "quarter turns are exact");

    double worstExp = 0.0;
    for (int step = -1000; step <= 200; ++step) {
        const double exponent = static_cast<double>(step) / 97.0;
        const double reference = std::exp2(exponent);
        worstExp =
            std::fmax(worstExp, std::fabs(runtime::detail::exp2(exponent) - reference) / reference);
    }
    expectations.expect(worstExp < 4e-16, "exp2 agrees with libm to a couple of ulps");
    expectations.expect(runtime::detail::exp2(-10.0) == 0.0009765625 &&
                            runtime::detail::exp2(0.0) == 1.0 && runtime::detail::exp2(3.0) == 8.0,
                        "integer powers of two are exact");

    double worstAsin = 0.0;
    for (int step = -1000; step <= 1000; ++step) {
        const double value = static_cast<double>(step) / 1000.0;
        worstAsin =
            std::fmax(worstAsin, std::fabs(runtime::detail::asinTurns(value) * 6.283185307179586 -
                                           std::asin(value)));
    }
    expectations.expect(worstAsin < 1e-14, "asinTurns agrees with libm to 1e-14");
    expectations.expect(std::isnan(runtime::detail::asinTurns(1.5)), "asin outside [-1, 1] is NaN");
}

void testReferenceRows(Expectations& expectations) {
    for (const auto& row : kReferenceRows) {
        document::KeyframeEasingParameters easing;
        easing.easing = row.ease;
        if (row.mode == Mode::Back)
            easing.back = row.parameter;
        const auto curve = segment(row.mode, easing, 2.0, 5.0, time(1));
        const auto sampled =
            runtime::sampleAnimationCurve(curve, time(row.numerator, row.denominator));
        expectations.expect(sampled.value.has_value() &&
                                std::fabs(*sampled.value - row.value) <= 1e-12,
                            "Blender reference value for " + label(row));
    }
}

void testElasticRows(Expectations& expectations) {
    int index = 0;
    for (const auto& row : kElasticRows) {
        document::KeyframeEasingParameters easing;
        easing.easing = row.ease;
        easing.amplitude = row.amplitude;
        easing.period = row.period;
        const auto duration = time(row.durationNumerator, row.durationDenominator);
        const auto curve = segment(Mode::Elastic, easing, row.begin, row.end, duration);
        const auto at =
            time(row.numerator * row.durationNumerator, row.denominator * row.durationDenominator);
        const auto sampled = runtime::sampleAnimationCurve(curve, at);
        expectations.expect(sampled.value.has_value() &&
                                std::fabs(*sampled.value - row.value) <=
                                    1e-12 * std::fmax(1.0, std::fabs(row.value)),
                            "Blender elastic reference row " + std::to_string(index));
        ++index;
    }
}

void testAutoAndEndpoints(Expectations& expectations) {
    expectations.expect(runtime::resolveKeyframeEasing(Mode::Back, Ease::Auto) == Ease::Out &&
                            runtime::resolveKeyframeEasing(Mode::Bounce, Ease::Auto) == Ease::Out &&
                            runtime::resolveKeyframeEasing(Mode::Elastic, Ease::Auto) ==
                                Ease::Out &&
                            runtime::resolveKeyframeEasing(Mode::Sine, Ease::Auto) == Ease::In &&
                            runtime::resolveKeyframeEasing(Mode::Expo, Ease::Auto) == Ease::In &&
                            runtime::resolveKeyframeEasing(Mode::Quad, Ease::InOut) == Ease::InOut,
                        "Auto resolves as Blender's fcurve evaluation does");
    // Auto samples exactly as the mode it resolves to.
    for (const auto mode : {Mode::Sine, Mode::Quad, Mode::Cubic, Mode::Quart, Mode::Quint,
                            Mode::Expo, Mode::Circ, Mode::Back, Mode::Bounce, Mode::Elastic}) {
        document::KeyframeEasingParameters automatic;
        auto explicitMode = automatic;
        explicitMode.easing = runtime::resolveKeyframeEasing(mode, Ease::Auto);
        const auto a = runtime::sampleAnimationCurve(segment(mode, automatic, -1.0, 4.0, time(2)),
                                                     time(7, 10));
        const auto b = runtime::sampleAnimationCurve(
            segment(mode, explicitMode, -1.0, 4.0, time(2)), time(7, 10));
        expectations.expect(a.value && b.value && bits(*a.value) == bits(*b.value),
                            "Auto is the resolved mode bit for bit");
        // Every equation meets its keys: the key value at the key time, and the next key's value
        // once the segment ends.
        for (const auto ease : {Ease::In, Ease::Out, Ease::InOut}) {
            document::KeyframeEasingParameters parameters;
            parameters.easing = ease;
            const auto curve = segment(mode, parameters, -1.0, 4.0, time(2));
            const auto start = runtime::sampleAnimationCurve(curve, time(0));
            const auto end = runtime::sampleAnimationCurve(curve, time(2));
            const auto near = runtime::sampleAnimationCurve(curve, time(1999999, 1000000));
            expectations.expect(start.value && *start.value == -1.0 && end.value &&
                                    *end.value == 4.0 && near.value &&
                                    std::fabs(*near.value - 4.0) < 0.05,
                                "an eased segment is continuous into its keys (mode " +
                                    std::to_string(static_cast<int>(mode)) + ", ease " +
                                    std::to_string(static_cast<int>(ease)) + ")");
        }
    }
}

void testDeterminismAndAdmission(Expectations& expectations) {
    document::KeyframeEasingParameters parameters;
    parameters.easing = Ease::InOut;
    const auto curve = segment(Mode::Elastic, parameters, 0.0, 100.0, time(5, 3));
    const auto first = runtime::sampleAnimationCurve(curve, time(17, 30));
    const auto second = runtime::sampleAnimationCurve(curve, time(17, 30));
    expectations.expect(first.value && second.value && bits(*first.value) == bits(*second.value),
                        "an eased sample is a pure function of its operands");

    auto invalid = parameters;
    invalid.amplitude = -1.0;
    const auto refused =
        runtime::sampleAnimationCurve(segment(Mode::Sine, invalid, 0.0, 1.0, time(1)), time(1, 2));
    expectations.expect(!refused.value &&
                            refused.error == runtime::AnimationSamplingError::InvalidCurve,
                        "a negative amplitude is refused on every request time");
    invalid = parameters;
    invalid.period = std::nan("");
    const auto refusedPeriod = runtime::sampleAnimationCurve(
        segment(Mode::Linear, invalid, 0.0, 1.0, time(1)), time(1, 2));
    expectations.expect(!refusedPeriod.value, "a non-finite period is refused");

    // The document curve reaches the same equation through compilation.
    document::ScalarAnimationCurve document{document::AnimationCurveId::fromRaw(9), {}};
    document.keyframes.push_back({document::KeyframeId::fromRaw(1),
                                  time(0),
                                  3.0,
                                  document::KeyframeInterpolation::Bounce,
                                  {},
                                  {},
                                  parameters});
    document.keyframes.push_back(
        {document::KeyframeId::fromRaw(2), time(1), 9.0, document::KeyframeInterpolation::Linear});
    const auto compiled = runtime::compileAnimationCurve(document);
    const auto direct = runtime::sampleAnimationCurve(document, time(2, 7));
    const auto viaPlan = runtime::sampleAnimationCurve(compiled, time(2, 7));
    expectations.expect(direct.value && viaPlan.value &&
                            bits(*direct.value) == bits(*viaPlan.value),
                        "document and compiled curves sample the same bits");
    expectations.expect(compiled.keyframes[0].easing == parameters &&
                            compiled.keyframes[0].outgoingInterpolation == Mode::Bounce,
                        "compilation carries the mode and its parameters");
}

} // namespace

int main() {
    Expectations expectations;
    expectations.expect(runtime::kAnimationSamplingSemanticsVersion == 3,
                        "easing equations advance the sampling semantics to 3");
    testKernels(expectations);
    testReferenceRows(expectations);
    testElasticRows(expectations);
    testAutoAndEndpoints(expectations);
    testDeterminismAndAdmission(expectations);
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " keyframe easing expectation(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
