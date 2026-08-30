// Direct unit tests for bloom::runtime::compileAnimationCurve() (issue #86, task E1): the public
// document-curve -> compiled-curve conversion extracted, behavior-preserving, from
// snapshot_compiler.cpp's own per-curve lowering (compileReachableCurves() in
// snapshot_compiler_lowering.ipp). snapshot_compiler_tests.cpp's
// testParameterSourcesAndDiagnosticIds already pins the SAME conversion end-to-end through the
// compiler (a full CompiledScalarCurve operator== against hand-written expected keyframes) and that
// test suite passes unchanged after the extraction, so equivalence through the compiler is not
// re-proven here. This file instead exercises the extracted function pair directly: Hold and Linear
// source interpolation, an exact subframe source time, and the "final key's outgoing interpolation
// is always normalized to Linear" fact (docs/architecture/animation-and-time.md) -- proving this is
// a PURE mechanical mapping that preserves whatever interpolation value the document curve already
// carries, rather than a second enforcement point for that document-layer invariant.

#include <bloom/runtime/curve_compilation.hpp>

#include <cstdlib>
#include <iostream>
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

[[nodiscard]] core::RationalTime time(const std::int64_t numerator,
                                      const std::int64_t denominator = 1) {
    const auto value = core::RationalTime::create(numerator, denominator);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

void testScalarCurveMapsEveryFieldIncludingHoldAndLinear(Expectations& expectations) {
    // Three keys: a Hold-outgoing key at an exact subframe time (1/3), a Linear-outgoing interior
    // key, and a canonical Linear-outgoing final key (K1's normalization fact -- the final key is
    // always Linear on any published document curve).
    const document::ScalarAnimationCurve curve{
        document::AnimationCurveId::fromRaw(7),
        {{document::KeyframeId::fromRaw(70), time(1, 3), -0.0,
          document::KeyframeInterpolation::Hold},
         {document::KeyframeId::fromRaw(71), time(2), 0.5, document::KeyframeInterpolation::Linear},
         {document::KeyframeId::fromRaw(72), time(5), 1.0,
          document::KeyframeInterpolation::Linear}}};

    const auto compiled = runtime::compileAnimationCurve(curve);
    const runtime::CompiledScalarCurve expected{
        document::AnimationCurveId::fromRaw(7),
        {{document::KeyframeId::fromRaw(70), time(1, 3), -0.0,
          runtime::CompiledKeyframeInterpolation::Hold},
         {document::KeyframeId::fromRaw(71), time(2), 0.5,
          runtime::CompiledKeyframeInterpolation::Linear},
         {document::KeyframeId::fromRaw(72), time(5), 1.0,
          runtime::CompiledKeyframeInterpolation::Linear}}};

    expectations.expect(
        compiled == expected,
        "scalar conversion preserves curve id, exact key id/time/value, and maps "
        "Hold/Linear interpolation 1:1, including an exact subframe source time and "
        "a signed -0.0 value");
    expectations.expect(compiled.keyframes.back().outgoingInterpolation ==
                            runtime::CompiledKeyframeInterpolation::Linear,
                        "the canonical Linear final-key fact (docs/architecture/animation-and-time."
                        "md) survives the conversion unchanged");
}

void testVec2CurveMapsEveryFieldIncludingHoldAndLinear(Expectations& expectations) {
    const document::Vec2AnimationCurve curve{
        document::AnimationCurveId::fromRaw(8),
        {{document::KeyframeId::fromRaw(80), time(0), document::Vec2d{1.0, 2.0},
          document::KeyframeInterpolation::Linear},
         {document::KeyframeId::fromRaw(81), time(7, 4), document::Vec2d{3.0, -4.0},
          document::KeyframeInterpolation::Hold},
         {document::KeyframeId::fromRaw(82), time(9), document::Vec2d{5.0, 6.0},
          document::KeyframeInterpolation::Linear}}};

    const auto compiled = runtime::compileAnimationCurve(curve);
    const runtime::CompiledVec2Curve expected{
        document::AnimationCurveId::fromRaw(8),
        {{document::KeyframeId::fromRaw(80), time(0), document::Vec2d{1.0, 2.0},
          runtime::CompiledKeyframeInterpolation::Linear},
         {document::KeyframeId::fromRaw(81), time(7, 4), document::Vec2d{3.0, -4.0},
          runtime::CompiledKeyframeInterpolation::Hold},
         {document::KeyframeId::fromRaw(82), time(9), document::Vec2d{5.0, 6.0},
          runtime::CompiledKeyframeInterpolation::Linear}}};

    expectations.expect(compiled == expected,
                        "Vec2 conversion preserves curve id, exact key id/time/value, and maps "
                        "Hold/Linear interpolation 1:1, including an exact subframe source time");
}

void testConversionIsMechanicalAndDoesNotEnforceTheFinalKeyInvariant(Expectations& expectations) {
    // compileAnimationCurve() is a PURE mapping with no validation pass (validation/normalization
    // is owned by src/commands, and sampling-time validation by animation_sampling.cpp). Feeding it
    // a deliberately non-canonical curve (a Hold final key, which src/commands never produces)
    // proves it carries the source value through verbatim rather than silently re-normalizing it --
    // the extraction must not become a second enforcement point for a document-layer invariant.
    const document::ScalarAnimationCurve nonCanonical{
        document::AnimationCurveId::fromRaw(9),
        {{document::KeyframeId::fromRaw(90), time(0), 0.0, document::KeyframeInterpolation::Hold}}};
    const auto compiled = runtime::compileAnimationCurve(nonCanonical);
    expectations.expect(compiled.keyframes.size() == 1 &&
                            compiled.keyframes.front().outgoingInterpolation ==
                                runtime::CompiledKeyframeInterpolation::Hold,
                        "a non-canonical Hold final key is passed through unchanged: this "
                        "conversion does not itself enforce the final-key Linear invariant");
}

} // namespace

int main() {
    Expectations expectations;
    testScalarCurveMapsEveryFieldIncludingHoldAndLinear(expectations);
    testVec2CurveMapsEveryFieldIncludingHoldAndLinear(expectations);
    testConversionIsMechanicalAndDoesNotEnforceTheFinalKeyInvariant(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
