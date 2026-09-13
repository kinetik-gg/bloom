#include <bloom/runtime/value_graph_evaluation.hpp>

#include <bloom/core/value_primitives.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace bloom;
using runtime::CompiledValue;
using runtime::CompiledValueOperand;
using runtime::CompiledValueOperation;
using runtime::ValueOutputIndex;

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

// A tiny builder so each case reads as the graph it is rather than as index bookkeeping: every
// operation claims the next run of outputs, which is exactly the contract the compiler emits and
// the evaluator checks.
class GraphBuilder final {
  public:
    [[nodiscard]] ValueOutputIndex add(runtime::CompiledValueKernel kernel,
                                       const std::uint8_t outputCount = 1) {
        const auto first = ValueOutputIndex::fromRaw(outputs_);
        operations_.push_back({document::NodeId::fromRaw(operations_.size() + 1), first,
                               outputCount, std::move(kernel)});
        outputs_ += outputCount;
        return first;
    }

    [[nodiscard]] runtime::ValueGraphEvaluation
    run(const core::RationalTime time = core::RationalTime::fromInteger(0),
        const document::FrameRate rate = document::FrameRate::framesPerSecond24()) const {
        return runtime::evaluateValueGraph(operations_, outputs_, time, rate);
    }

  private:
    std::vector<CompiledValueOperation> operations_;
    std::size_t outputs_ = 0;
};

[[nodiscard]] CompiledValueOperand constant(CompiledValue value) {
    return {document::ParameterId::fromRaw(1), std::move(value)};
}

[[nodiscard]] CompiledValueOperand reference(const ValueOutputIndex index) {
    return {document::ParameterId::fromRaw(1), index};
}

template <typename Value>
[[nodiscard]] bool holds(const runtime::ValueGraphEvaluation& evaluation, const std::size_t index,
                         const Value& expected) {
    if (index >= evaluation.outputs.size()) {
        return false;
    }
    const auto* value = std::get_if<Value>(&evaluation.outputs[index]);
    return value != nullptr && *value == expected;
}

void testLiteralsAndPassthrough(Expectations& expectations) {
    GraphBuilder builder;
    const auto scalar = builder.add(runtime::CompiledValuePassthrough{constant(2.5)});
    const auto boolean = builder.add(runtime::CompiledValuePassthrough{constant(true)});
    const auto text =
        builder.add(runtime::CompiledValuePassthrough{constant(std::string("caption"))});
    const auto reroute = builder.add(runtime::CompiledValuePassthrough{reference(scalar)});
    const auto evaluation = builder.run();
    expectations.expect(evaluation.diagnostics.empty(), "a graph of literals reports nothing");
    expectations.expect(holds(evaluation, scalar.value(), 2.5),
                        "a Scalar literal is its own value");
    expectations.expect(holds(evaluation, boolean.value(), true),
                        "a Boolean literal is its own value");
    expectations.expect(holds(evaluation, text.value(), std::string("caption")),
                        "a String literal is its own value");
    expectations.expect(holds(evaluation, reroute.value(), 2.5),
                        "a Reroute passes its operand through unchanged");
}

void testTime(Expectations& expectations) {
    GraphBuilder builder;
    const auto time = builder.add(runtime::CompiledValueTime{}, 2);
    const auto rate = document::FrameRate::framesPerSecond24();
    // Frame 12 at 24fps is exactly half a second in, and both outputs must say so in their own
    // unit.
    const auto halfSecond = core::RationalTime::create(12, 24);
    if (!halfSecond.has_value()) {
        expectations.expect(false, "the half-second fixture time is constructible");
        return;
    }
    const auto evaluation = builder.run(*halfSecond, rate);
    expectations.expect(holds(evaluation, time.value(), 0.5),
                        "the seconds output is the request time in seconds");
    expectations.expect(holds(evaluation, time.value() + 1, std::int64_t{12}),
                        "the frame output is the same instant as a frame number");

    expectations.expect(runtime::valueGraphFrameIndex(core::RationalTime::fromInteger(0), rate) ==
                            std::int64_t{0},
                        "time zero is frame zero");
    const auto beforeZero = core::RationalTime::create(-1, 24);
    expectations.expect(beforeZero.has_value() &&
                            runtime::valueGraphFrameIndex(*beforeZero, rate) == std::int64_t{-1},
                        "a time before zero floors rather than truncating toward zero");
    // Half a frame in: a frame spans the half-open interval from its own start to the next one's,
    // so anything inside frame zero's span is still frame zero.
    const auto halfFrame = core::RationalTime::create(1, 48);
    expectations.expect(halfFrame.has_value() &&
                            runtime::valueGraphFrameIndex(*halfFrame, rate) == std::int64_t{0},
                        "a time inside frame zero's span is still frame zero");
    const auto dropFrame = document::FrameRate::create(30000, 1001);
    const auto oneSecond = core::RationalTime::fromInteger(1);
    expectations.expect(dropFrame.has_value() && runtime::valueGraphFrameIndex(
                                                     oneSecond, *dropFrame) == std::int64_t{29},
                        "one second at 30000/1001 floors to frame 29, exactly and without floats");
}

void testScalarMath(Expectations& expectations) {
    using Scalar = core::primitives::ScalarPrimitive;
    {
        GraphBuilder builder;
        const auto sum = builder.add(
            runtime::CompiledValueScalarMath{Scalar::Add, false, {constant(1.5), constant(2.25)}});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, sum.value(), 3.75), "Add is exact");
        expectations.expect(evaluation.diagnostics.empty(), "a domain-valid Add reports nothing");
    }
    {
        // The documented fallback, and the one an artist will actually hit: dividing by zero is
        // zero, with a scoped diagnostic rather than a NaN spreading through the graph.
        GraphBuilder builder;
        const auto quotient = builder.add(runtime::CompiledValueScalarMath{
            Scalar::Divide, false, {constant(1.0), constant(0.0)}});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, quotient.value(), 0.0),
                            "dividing by zero substitutes the documented fallback of zero");
        expectations.expect(evaluation.diagnostics.size() == 1 &&
                                evaluation.diagnostics.front().output == quotient,
                            "and says so, scoped to the operation that failed");
    }
    {
        GraphBuilder builder;
        const auto clamped = builder.add(
            runtime::CompiledValueScalarMath{Scalar::Add, true, {constant(0.9), constant(0.9)}});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, clamped.value(), 1.0),
                            "the clamp toggle confines the result to the unit interval");
    }
    {
        // Five operands, the widest arity in the vocabulary: the node declares five sockets so one
        // definition serves every operation, and Remap is the operation that reads all of them.
        GraphBuilder builder;
        const auto remapped = builder.add(runtime::CompiledValueScalarMath{
            Scalar::Remap,
            false,
            {constant(5.0), constant(0.0), constant(10.0), constant(100.0), constant(200.0)}});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, remapped.value(), 150.0),
                            "Remap reads all five operands");
    }
    {
        GraphBuilder builder;
        const auto wrongKind = builder.add(
            runtime::CompiledValueScalarMath{Scalar::Absolute, false, {constant(true)}});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, wrongKind.value(), 0.0) &&
                                evaluation.diagnostics.size() == 1,
                            "a Boolean is never silently a number: the operand is reported");
    }
}

void testVectorMathAndReductions(Expectations& expectations) {
    {
        GraphBuilder builder;
        const auto sum = builder.add(runtime::CompiledValueVectorMath{
            document::VectorOperation::Add, 2, constant(document::Vec2d{1.0, 2.0}),
            constant(document::Vec2d{3.0, 4.0}), constant(1.0)});
        const auto scaled = builder.add(
            runtime::CompiledValueVectorMath{document::VectorOperation::Scale, 2, reference(sum),
                                             constant(document::Vec2d{}), constant(2.0)});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, sum.value(), document::Vec2d{4.0, 6.0}),
                            "componentwise Add routes through the scalar kernel per component");
        expectations.expect(holds(evaluation, scaled.value(), document::Vec2d{8.0, 12.0}),
                            "Scale multiplies every component by the node's scalar operand");
    }
    {
        GraphBuilder builder;
        const auto cross = builder.add(runtime::CompiledValueVectorMath{
            document::VectorOperation::CrossProduct, 3, constant(document::Vec3d{1.0, 0.0, 0.0}),
            constant(document::Vec3d{0.0, 1.0, 0.0}), constant(1.0)});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, cross.value(), document::Vec3d{0.0, 0.0, 1.0}),
                            "x cross y is z at three components");
    }
    {
        // Normalizing a zero-length vector has no answer, so the documented fallback is the zero
        // vector itself -- not an arbitrary axis that would look like a real direction.
        GraphBuilder builder;
        const auto degenerate = builder.add(runtime::CompiledValueVectorMath{
            document::VectorOperation::Normalize, 2, constant(document::Vec2d{}),
            constant(document::Vec2d{}), constant(1.0)});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, degenerate.value(), document::Vec2d{}),
                            "normalizing a zero-length vector falls back to the zero vector");
        expectations.expect(evaluation.diagnostics.size() == 1, "and reports the domain failure");
    }
    {
        GraphBuilder builder;
        const auto length = builder.add(runtime::CompiledValueVectorReduce{
            document::VectorReduction::Length, 2, constant(document::Vec2d{3.0, 4.0}),
            constant(document::Vec2d{})});
        const auto dot = builder.add(runtime::CompiledValueVectorReduce{
            document::VectorReduction::DotProduct, 2, constant(document::Vec2d{1.0, 2.0}),
            constant(document::Vec2d{3.0, 4.0})});
        const auto distance = builder.add(runtime::CompiledValueVectorReduce{
            document::VectorReduction::Distance, 2, constant(document::Vec2d{}),
            constant(document::Vec2d{3.0, 4.0})});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, length.value(), 5.0), "Length is exact");
        expectations.expect(holds(evaluation, dot.value(), 11.0), "Dot Product is exact");
        expectations.expect(holds(evaluation, distance.value(), 5.0), "Distance is exact");
    }
}

void testMapRangeClampAndMix(Expectations& expectations) {
    {
        GraphBuilder builder;
        const auto linear = builder.add(runtime::CompiledValueMapRange{
            document::RangeInterpolation::Linear, false, constant(0.25), constant(0.0),
            constant(1.0), constant(10.0), constant(20.0)});
        const auto smooth = builder.add(runtime::CompiledValueMapRange{
            document::RangeInterpolation::Smoothstep, false, constant(0.5), constant(0.0),
            constant(1.0), constant(0.0), constant(1.0)});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, linear.value(), 12.5),
                            "a linear Map Range is an exact remap");
        expectations.expect(holds(evaluation, smooth.value(), 0.5),
                            "smoothstep is symmetric about the midpoint");
    }
    {
        // A degenerate source range has no factor, so the documented fallback is the destination
        // minimum -- the value everything in that range would map to if it had any width.
        GraphBuilder builder;
        const auto degenerate = builder.add(runtime::CompiledValueMapRange{
            document::RangeInterpolation::Linear, false, constant(5.0), constant(2.0),
            constant(2.0), constant(-7.0), constant(9.0)});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, degenerate.value(), -7.0),
                            "a degenerate source range falls back to the destination minimum");
        expectations.expect(evaluation.diagnostics.size() == 1, "and reports it");
    }
    {
        GraphBuilder builder;
        const auto inside =
            builder.add(runtime::CompiledValueClamp{constant(0.5), constant(0.0), constant(1.0)});
        // Reversed bounds name no interval, so the value passes through UNCLAMPED: visibly wrong in
        // the picture, where silently swapping the bounds would look correct and hide the mistake.
        const auto reversed =
            builder.add(runtime::CompiledValueClamp{constant(5.0), constant(1.0), constant(0.0)});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, inside.value(), 0.5),
                            "Clamp passes a value in range");
        expectations.expect(holds(evaluation, reversed.value(), 5.0),
                            "reversed Clamp bounds pass the value through unclamped");
        expectations.expect(evaluation.diagnostics.size() == 1,
                            "and the inverted bound pair is reported");
    }
    {
        GraphBuilder builder;
        const auto scalar = builder.add(
            runtime::CompiledValueMix{false, constant(0.25), constant(0.0), constant(8.0)});
        const auto color = builder.add(runtime::CompiledValueMix{
            true, constant(0.5), constant(core::Color4d{0.0, 0.0, 0.0, 0.0}),
            constant(core::Color4d{1.0, 0.5, 0.25, 1.0})});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, scalar.value(), 2.0), "a scalar Mix interpolates");
        expectations.expect(
            holds(evaluation, color.value(), core::Color4d{0.5, 0.25, 0.125, 0.5}),
            "a colour Mix interpolates all four channels independently, alpha included");
    }
}

void testCompareSwitchAndConvert(Expectations& expectations) {
    {
        GraphBuilder builder;
        const auto greater = builder.add(runtime::CompiledValueCompare{
            document::CompareOperation::Greater, constant(2.0), constant(1.0), constant(0.0001)});
        // Equality is the one predicate that spends the epsilon: these differ by less than it.
        const auto nearlyEqual = builder.add(runtime::CompiledValueCompare{
            document::CompareOperation::Equal, constant(1.0), constant(1.00001), constant(0.0001)});
        const auto exactlyUnequal = builder.add(runtime::CompiledValueCompare{
            document::CompareOperation::Equal, constant(1.0), constant(1.01), constant(0.0001)});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, greater.value(), true), "Greater compares exactly");
        expectations.expect(holds(evaluation, nearlyEqual.value(), true),
                            "Equal admits a difference within epsilon");
        expectations.expect(holds(evaluation, exactlyUnequal.value(), false),
                            "and refuses one outside it");
    }
    {
        GraphBuilder builder;
        const auto condition = builder.add(runtime::CompiledValueCompare{
            document::CompareOperation::Less, constant(1.0), constant(2.0), constant(0.0)});
        const auto chosen = builder.add(runtime::CompiledValueSwitch{
            reference(condition), constant(std::string("false branch")),
            constant(std::string("true branch"))});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, chosen.value(), std::string("true branch")),
                            "Switch passes exactly the selected branch through");
        expectations.expect(evaluation.diagnostics.empty(),
                            "and the unselected branch costs no diagnostic");
    }
    {
        GraphBuilder builder;
        const auto separated = builder.add(
            runtime::CompiledValueSeparate{constant(document::Vec3d{1.0, 2.0, 3.0}), 3, false}, 3);
        const auto channels = builder.add(
            runtime::CompiledValueSeparate{constant(core::Color4d{0.1, 0.2, 0.3, 0.4}), 4, true},
            4);
        const auto combined = builder.add(runtime::CompiledValueCombine{
            {reference(ValueOutputIndex::fromRaw(separated.value() + 2)), reference(separated),
             reference(ValueOutputIndex::fromRaw(separated.value() + 1))},
            false});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, separated.value(), 1.0) &&
                                holds(evaluation, separated.value() + 1, 2.0) &&
                                holds(evaluation, separated.value() + 2, 3.0),
                            "Separate XYZ writes one output per component, in declaration order");
        expectations.expect(holds(evaluation, channels.value(), 0.1) &&
                                holds(evaluation, channels.value() + 3, 0.4),
                            "Separate RGBA writes red first and alpha last");
        expectations.expect(holds(evaluation, combined.value(), document::Vec3d{3.0, 1.0, 2.0}),
                            "Combine XYZ reassembles its operands in its own port order");
    }
}

void testRandomAndPromotions(Expectations& expectations) {
    {
        GraphBuilder builder;
        const auto first = builder.add(
            runtime::CompiledValueRandom{constant(std::int64_t{7}), constant(0.0), constant(1.0)});
        const auto again = builder.add(
            runtime::CompiledValueRandom{constant(std::int64_t{7}), constant(0.0), constant(1.0)});
        const auto other = builder.add(
            runtime::CompiledValueRandom{constant(std::int64_t{8}), constant(0.0), constant(1.0)});
        const auto scaled = builder.add(runtime::CompiledValueRandom{
            constant(std::int64_t{7}), constant(10.0), constant(20.0)});
        const auto evaluation = builder.run();
        const auto* value = std::get_if<double>(&evaluation.outputs[first.value()]);
        const auto* repeat = std::get_if<double>(&evaluation.outputs[again.value()]);
        const auto* different = std::get_if<double>(&evaluation.outputs[other.value()]);
        const auto* ranged = std::get_if<double>(&evaluation.outputs[scaled.value()]);
        expectations.expect(value != nullptr && repeat != nullptr && *value == *repeat,
                            "the same seed is the same value within one frame");
        expectations.expect(different != nullptr && *different != *value,
                            "a different seed is a different value");
        expectations.expect(value != nullptr && *value >= 0.0 && *value < 1.0,
                            "the default bounds are the half-open unit span");
        expectations.expect(ranged != nullptr && std::abs(*ranged - (10.0 + 10.0 * *value)) < 1e-12,
                            "bounds rescale the same hashed position rather than rehashing");
        // Evaluated again at a different time: nothing about a seeded value may depend on the
        // frame, because a cached frame has to agree with the frame that produced it.
        const auto later = builder.run(core::RationalTime::fromInteger(3));
        expectations.expect(holds(later, first.value(), *value),
                            "a seeded value does not change with the request time");
    }
    {
        GraphBuilder builder;
        const auto widened = builder.add(runtime::CompiledValuePromotion{
            runtime::ValuePromotion::IntegerToScalar, constant(std::int64_t{-3})});
        const auto fromBoolean = builder.add(runtime::CompiledValuePromotion{
            runtime::ValuePromotion::BooleanToInteger, constant(true)});
        const auto splat2 = builder.add(runtime::CompiledValuePromotion{
            runtime::ValuePromotion::ScalarToVector2, constant(1.5)});
        const auto splat3 = builder.add(runtime::CompiledValuePromotion{
            runtime::ValuePromotion::ScalarToVector3, reference(widened)});
        const auto evaluation = builder.run();
        expectations.expect(holds(evaluation, widened.value(), -3.0),
                            "Integer widens to Scalar exactly");
        expectations.expect(holds(evaluation, fromBoolean.value(), std::int64_t{1}),
                            "true is one, the same mapping every stored boolean has");
        expectations.expect(holds(evaluation, splat2.value(), document::Vec2d{1.5, 1.5}),
                            "a Scalar splats into every component of a Vector2");
        expectations.expect(holds(evaluation, splat3.value(), document::Vec3d{-3.0, -3.0, -3.0}),
                            "and of a Vector3");
        expectations.expect(evaluation.diagnostics.empty(), "every whitelisted promotion is total");
    }
}

void testMalformedPlan(Expectations& expectations) {
    // An operand naming an output the graph never writes. A well-formed plan cannot contain one --
    // the compiler emits operations in topological order -- so this is the malformed-plan path, and
    // it is reported rather than read out of uninitialized storage.
    const std::vector<CompiledValueOperation> operations{
        {document::NodeId::fromRaw(1), ValueOutputIndex::fromRaw(0), 1,
         runtime::CompiledValuePassthrough{reference(ValueOutputIndex::fromRaw(9))}}};
    const auto evaluation =
        runtime::evaluateValueGraph(operations, 1, core::RationalTime::fromInteger(0),
                                    document::FrameRate::framesPerSecond24());
    expectations.expect(evaluation.outputs.size() == 1,
                        "the output table is always the size the plan declares");
    expectations.expect(evaluation.diagnostics.size() == 1,
                        "an operand naming an unwritten output is reported");

    // An operation claiming a range that does not continue the partition stops the sweep rather
    // than writing where a later operation's operands would read from.
    const std::vector<CompiledValueOperation> overlapping{
        {document::NodeId::fromRaw(1), ValueOutputIndex::fromRaw(0), 1,
         runtime::CompiledValuePassthrough{constant(1.0)}},
        {document::NodeId::fromRaw(2), ValueOutputIndex::fromRaw(0), 1,
         runtime::CompiledValuePassthrough{constant(2.0)}}};
    const auto refused =
        runtime::evaluateValueGraph(overlapping, 2, core::RationalTime::fromInteger(0),
                                    document::FrameRate::framesPerSecond24());
    expectations.expect(
        holds(refused, 0, 1.0) && refused.diagnostics.size() == 1,
        "an overlapping output range is refused without corrupting the first write");

    // A kernel whose own output count exceeds the run the operation claims. The compiler cannot
    // emit one -- both numbers come from the same definition -- so this is the boundary where a
    // malformed plan would otherwise write past a neighbour's slot.
    const std::vector<CompiledValueOperation> overreaching{
        {document::NodeId::fromRaw(1), ValueOutputIndex::fromRaw(0), 1,
         runtime::CompiledValueSeparate{constant(core::Color4d{0.25, 0.5, 0.75, 1.0}), 4, true}},
        {document::NodeId::fromRaw(2), ValueOutputIndex::fromRaw(1), 1,
         runtime::CompiledValuePassthrough{constant(99.0)}}};
    const auto clamped =
        runtime::evaluateValueGraph(overreaching, 2, core::RationalTime::fromInteger(0),
                                    document::FrameRate::framesPerSecond24());
    expectations.expect(holds(clamped, 0, 0.25) && holds(clamped, 1, 99.0),
                        "a kernel that would overrun its own run writes only the slots it owns");
}

} // namespace

int main() {
    Expectations expectations;
    testLiteralsAndPassthrough(expectations);
    testTime(expectations);
    testScalarMath(expectations);
    testVectorMathAndReductions(expectations);
    testMapRangeClampAndMix(expectations);
    testCompareSwitchAndConvert(expectations);
    testRandomAndPromotions(expectations);
    testMalformedPlan(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
