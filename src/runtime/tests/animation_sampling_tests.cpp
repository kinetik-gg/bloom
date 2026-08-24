#include <bloom/runtime/animation_sampling.hpp>

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
    expectations.expect(runtime::kAnimationSamplingSemanticsVersion == 1,
                        "animation sampling semantics are explicitly versioned");
    testSampling(expectations);
    testVec2AndExtremeTime(expectations);
    testValidationAndEnvironment(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
