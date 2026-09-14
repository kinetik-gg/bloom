#include <bloom/runtime/value_utility_kernels.hpp>

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace bloom;
using runtime::CompiledValue;
using runtime::ValueUtilityOutcome;

using Kernel = document::ValueUtilityKernel;

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

[[nodiscard]] ValueUtilityOutcome run(const Kernel kernel, const std::vector<CompiledValue>& values,
                                      const std::vector<std::int64_t>& selectors = {}) {
    std::vector<const CompiledValue*> operands;
    operands.reserve(values.size());
    for (const auto& value : values) {
        operands.push_back(&value);
    }
    return runtime::evaluateValueUtility({kernel, operands, selectors,
                                          core::RationalTime::fromInteger(0),
                                          document::FrameRate::framesPerSecond24()});
}

template <typename Value>
[[nodiscard]] bool holds(const ValueUtilityOutcome& outcome, const std::size_t slot,
                         const Value& expected) {
    if (slot >= outcome.outputCount) {
        return false;
    }
    const auto* value = std::get_if<Value>(&outcome.outputs[slot]);
    return value != nullptr && *value == expected;
}

[[nodiscard]] bool near(const ValueUtilityOutcome& outcome, const std::size_t slot,
                        const double expected, const double tolerance = 1e-12) {
    if (slot >= outcome.outputCount) {
        return false;
    }
    const auto* value = std::get_if<double>(&outcome.outputs[slot]);
    return value != nullptr && std::fabs(*value - expected) <= tolerance;
}

[[nodiscard]] bool nearVector(const ValueUtilityOutcome& outcome, const document::Vec2d expected,
                              const double tolerance = 1e-12) {
    const auto* value = std::get_if<document::Vec2d>(&outcome.outputs[0]);
    return value != nullptr && std::fabs(value->x - expected.x) <= tolerance &&
           std::fabs(value->y - expected.y) <= tolerance;
}

[[nodiscard]] bool nearColor(const ValueUtilityOutcome& outcome, const core::Color4d expected,
                             const double tolerance = 1e-12) {
    const auto* value = std::get_if<core::Color4d>(&outcome.outputs[0]);
    return value != nullptr && std::fabs(value->red - expected.red) <= tolerance &&
           std::fabs(value->green - expected.green) <= tolerance &&
           std::fabs(value->blue - expected.blue) <= tolerance &&
           std::fabs(value->alpha - expected.alpha) <= tolerance;
}

// The Math section is a CATEGORY, not a lowering: it has to hold exactly the arithmetic and nothing
// else, and the plumbing has to stay where an artist already found it.
void testMathSectionMembership(Expectations& expectations) {
    const auto& registry = document::builtInNodeDefinitions();
    const auto categoryOf = [&registry](const std::string_view typeId) {
        const auto* definition = registry.find(typeId, document::kValueNodeSchemaVersion);
        return definition == nullptr ? document::NodeCategory::Compatibility : definition->category;
    };
    for (const auto typeId :
         {document::kScalarMathNodeType, document::kVector2MathNodeType,
          document::kVector3MathNodeType, document::kVector2ReduceNodeType,
          document::kVector3ReduceNodeType, document::kMapRangeNodeType, document::kClampNodeType,
          document::kMixNodeType, document::kColorMixNodeType, document::kCompareNodeType,
          document::kRandomNodeType}) {
        expectations.expect(categoryOf(typeId) == document::NodeCategory::Math,
                            "the existing arithmetic moved into Math");
    }
    // The plumbing stays: a Switch, a Separate, a Combine and a Reroute are not arithmetic.
    for (const auto typeId : {document::kScalarSwitchNodeType, document::kStringSwitchNodeType,
                              document::kSeparateXyNodeType, document::kCombineRgbaNodeType,
                              document::kRerouteNodeType}) {
        expectations.expect(categoryOf(typeId) == document::NodeCategory::Utilities,
                            "the plumbing stays in Utilities");
    }
    // The literals stay under Values.
    for (const auto typeId : {document::kScalarValueNodeType, document::kTimeValueNodeType}) {
        expectations.expect(categoryOf(typeId) == document::NodeCategory::Values,
                            "the literal sources stay under Values");
    }
    // Logic is a predicate, not arithmetic, so it is Utilities; the conversions are too.
    for (const auto typeId : {document::kBooleanLogicNodeType, document::kBooleanNotNodeType,
                              document::kInRangeNodeType, document::kStringToScalarNodeType,
                              document::kSecondsToFramesNodeType}) {
        expectations.expect(categoryOf(typeId) == document::NodeCategory::Utilities,
                            "logic and conversion stay in Utilities");
    }
    for (const auto typeId : {document::kIntegerMathNodeType, document::kWrapNodeType,
                              document::kLuminanceNodeType, document::kRotate2dNodeType}) {
        expectations.expect(categoryOf(typeId) == document::NodeCategory::Math,
                            "every new numeric node is listed under Math");
    }
}

void testIntegerMath(Expectations& expectations) {
    struct Case final {
        std::int64_t left;
        std::int64_t right;
        document::IntegerOperation operation;
        std::int64_t result;
    };
    static const Case kCases[]{
        {7, 3, document::IntegerOperation::Add, 10},
        {7, 3, document::IntegerOperation::Subtract, 4},
        {7, 3, document::IntegerOperation::Multiply, 21},
        {7, 3, document::IntegerOperation::Divide, 2},
        {-7, 3, document::IntegerOperation::Divide, -2},
        {7, 3, document::IntegerOperation::Modulo, 1},
        // Truncated division's remainder takes the sign of the DIVIDEND, which is C's own rule.
        {-7, 3, document::IntegerOperation::Modulo, -1},
        {7, -3, document::IntegerOperation::Modulo, 1},
        {7, 3, document::IntegerOperation::Minimum, 3},
        {7, 3, document::IntegerOperation::Maximum, 7},
        // Division and modulo by zero answer zero, the documented fallback the scalar tranche
        // gives.
        {7, 0, document::IntegerOperation::Divide, 0},
        {7, 0, document::IntegerOperation::Modulo, 0},
        // Saturation, never wrapping: signed overflow is undefined, and these operands come from a
        // graph.
        {std::numeric_limits<std::int64_t>::max(), 1, document::IntegerOperation::Add,
         std::numeric_limits<std::int64_t>::max()},
        {std::numeric_limits<std::int64_t>::min(), 1, document::IntegerOperation::Subtract,
         std::numeric_limits<std::int64_t>::min()},
        {std::numeric_limits<std::int64_t>::max(), 2, document::IntegerOperation::Multiply,
         std::numeric_limits<std::int64_t>::max()},
        {std::numeric_limits<std::int64_t>::min(), 2, document::IntegerOperation::Multiply,
         std::numeric_limits<std::int64_t>::min()},
        // The one signed division that overflows.
        {std::numeric_limits<std::int64_t>::min(), -1, document::IntegerOperation::Divide,
         std::numeric_limits<std::int64_t>::max()},
        {std::numeric_limits<std::int64_t>::min(), -1, document::IntegerOperation::Modulo, 0},
        {std::numeric_limits<std::int64_t>::min(), -1, document::IntegerOperation::Multiply,
         std::numeric_limits<std::int64_t>::max()},
    };
    for (const auto& item : kCases) {
        const auto outcome = run(Kernel::IntegerMath, {item.left, item.right},
                                 {document::selectorStoredValue(item.operation)});
        expectations.expect(!outcome.failed && holds(outcome, 0, item.result),
                            "Integer Math saturates and falls back rather than trapping");
    }
}

void testShapingOperations(Expectations& expectations) {
    struct RoundingCase final {
        double value;
        document::RoundingMode mode;
        double result;
    };
    static const RoundingCase kRoundings[]{
        // Ties go to EVEN, which is the frozen primitive's own std::nearbyint rule: 2.5 is 2 and
        // 3.5 is 4. Every rounding in the library routes through it, so no two nodes disagree.
        {2.5, document::RoundingMode::Round, 2.0},
        {3.5, document::RoundingMode::Round, 4.0},
        {-2.5, document::RoundingMode::Round, -2.0},
        {2.9, document::RoundingMode::Floor, 2.0},
        {-2.1, document::RoundingMode::Floor, -3.0},
        {2.1, document::RoundingMode::Ceiling, 3.0},
        {-2.9, document::RoundingMode::Ceiling, -2.0},
        {2.9, document::RoundingMode::Truncate, 2.0},
        {-2.9, document::RoundingMode::Truncate, -2.0},
    };
    for (const auto& item : kRoundings) {
        expectations.expect(
            near(run(Kernel::Rounding, {item.value}, {document::selectorStoredValue(item.mode)}), 0,
                 item.result),
            "Rounding routes through the frozen scalar primitive");
    }

    expectations.expect(near(run(Kernel::Sign, {3.5}), 0, 1.0) &&
                            near(run(Kernel::Sign, {-3.5}), 0, -1.0) &&
                            near(run(Kernel::Sign, {0.0}), 0, 0.0),
                        "Sign answers -1, 0 or 1");

    struct WrapCase final {
        double value;
        double minimum;
        double maximum;
        double result;
    };
    static const WrapCase kWraps[]{
        {0.5, 0.0, 1.0, 0.5},
        {1.5, 0.0, 1.0, 0.5},
        {-0.25, 0.0, 1.0, 0.75},
        {1.0, 0.0, 1.0, 0.0},
        {370.0, 0.0, 360.0, 10.0},
        {-10.0, 0.0, 360.0, 350.0},
        {5.0, 2.0, 4.0, 3.0},
        // A degenerate or reversed interval has no period, so the value passes through UNWRAPPED --
        // visibly wrong, where silently swapping the bounds would look correct.
        {7.0, 1.0, 1.0, 7.0},
        {7.0, 5.0, 1.0, 7.0},
    };
    for (const auto& item : kWraps) {
        expectations.expect(
            near(run(Kernel::Wrap, {item.value, item.minimum, item.maximum}), 0, item.result),
            "Wrap folds into the half-open interval and refuses a degenerate one");
    }

    struct SnapCase final {
        double value;
        double step;
        double result;
    };
    static const SnapCase kSnaps[]{
        {7.3, 1.0, 7.0},   {7.6, 1.0, 8.0}, {7.0, 2.5, 7.5},
        {-7.3, 1.0, -7.0}, {7.3, 0.0, 7.3}, {0.0, 5.0, 0.0},
    };
    for (const auto& item : kSnaps) {
        expectations.expect(near(run(Kernel::Snap, {item.value, item.step}), 0, item.result),
                            "Snap rounds to the nearest multiple, and a zero step is no grid");
    }

    struct PingPongCase final {
        double value;
        double length;
        double result;
    };
    static const PingPongCase kPingPongs[]{
        {0.0, 1.0, 0.0}, {0.5, 1.0, 0.5},  {1.0, 1.0, 1.0}, {1.5, 1.0, 0.5},  {2.0, 1.0, 0.0},
        {2.5, 1.0, 0.5}, {-0.5, 1.0, 0.5}, {3.0, 0.0, 0.0}, {3.0, -1.0, 0.0},
    };
    for (const auto& item : kPingPongs) {
        expectations.expect(
            near(run(Kernel::PingPong, {item.value, item.length}), 0, item.result),
            "Ping-pong is a triangle wave, and a non-positive length has no period");
    }

    struct SmoothstepCase final {
        double edge0;
        double edge1;
        double value;
        double result;
    };
    static const SmoothstepCase kSmoothsteps[]{
        {0.0, 1.0, -1.0, 0.0}, {0.0, 1.0, 0.0, 0.0}, {0.0, 1.0, 0.5, 0.5},
        {0.0, 1.0, 1.0, 1.0},  {0.0, 1.0, 2.0, 1.0}, {2.0, 4.0, 3.0, 0.5},
    };
    for (const auto& item : kSmoothsteps) {
        expectations.expect(
            near(run(Kernel::Smoothstep, {item.edge0, item.edge1, item.value}), 0, item.result),
            "Smoothstep reads its edges first, as the frozen primitive does");
    }
    // A degenerate edge pair is outside the primitive's domain, so the documented fallback of zero
    // is what the node writes.
    expectations.expect(near(run(Kernel::Smoothstep, {1.0, 1.0, 5.0}), 0, 0.0),
                        "a degenerate edge pair answers the documented fallback");
}

void testGeometry(Expectations& expectations) {
    expectations.expect(
        near(run(Kernel::DegreesToRadians, {180.0}), 0, std::numbers::pi) &&
            near(run(Kernel::DegreesToRadians, {-90.0}), 0, -std::numbers::pi / 2.0),
        "Degrees To Radians scales by pi over 180");
    expectations.expect(near(run(Kernel::RadiansToDegrees, {std::numbers::pi}), 0, 180.0, 1e-10),
                        "Radians To Degrees is its inverse");

    const document::Vec2d unitX{1.0, 0.0};
    expectations.expect(
        nearVector(run(Kernel::Rotate2d, {unitX, 90.0, document::Vec2d{}}), {0.0, 1.0}, 1e-12),
        "Rotate 2D turns counter-clockwise in a Y-up frame");
    expectations.expect(
        nearVector(run(Kernel::Rotate2d, {unitX, 180.0, document::Vec2d{}}), {-1.0, 0.0}, 1e-12),
        "and half a turn reverses the vector");
    expectations.expect(nearVector(run(Kernel::Rotate2d, {document::Vec2d{2.0, 1.0}, 90.0,
                                                          document::Vec2d{1.0, 1.0}}),
                                   {1.0, 2.0}, 1e-12),
                        "and it turns about the authored pivot");
    expectations.expect(nearVector(run(Kernel::Rotate2d, {unitX, 0.0, document::Vec2d{}}), unitX),
                        "and no rotation moves nothing");

    expectations.expect(
        nearVector(run(Kernel::PolarToCartesian, {2.0, 0.0}), {2.0, 0.0}, 1e-12) &&
            nearVector(run(Kernel::PolarToCartesian, {2.0, 90.0}), {0.0, 2.0}, 1e-12),
        "Polar To Cartesian reads degrees");

    const auto polar = run(Kernel::CartesianToPolar, {document::Vec2d{0.0, 3.0}});
    expectations.expect(near(polar, 0, 3.0) && near(polar, 1, 90.0, 1e-12),
                        "Cartesian To Polar answers a radius and an angle in degrees");
    const auto negative = run(Kernel::CartesianToPolar, {document::Vec2d{-1.0, 0.0}});
    expectations.expect(near(negative, 0, 1.0) && near(negative, 1, 180.0, 1e-12),
                        "and covers the whole circle");
    const auto origin = run(Kernel::CartesianToPolar, {document::Vec2d{}});
    // The origin has no direction, so zero is the answer rather than an axis it does not point
    // along -- the same choice Normalize makes for a zero-length vector.
    expectations.expect(near(origin, 0, 0.0) && near(origin, 1, 0.0),
                        "and the origin has no direction to report");
}

void testColorOperations(Expectations& expectations) {
    const core::Color4d red{1.0, 0.0, 0.0, 1.0};
    const auto separated = run(Kernel::SeparateHsv, {red});
    expectations.expect(separated.outputCount == 4 && near(separated, 0, 0.0) &&
                            near(separated, 1, 1.0) && near(separated, 2, 1.0) &&
                            near(separated, 3, 1.0),
                        "Separate HSV answers hue, saturation, value and alpha");
    const auto grey = run(Kernel::SeparateHsv, {core::Color4d{0.5, 0.5, 0.5, 0.25}});
    expectations.expect(near(grey, 0, 0.0) && near(grey, 1, 0.0) && near(grey, 2, 0.5) &&
                            near(grey, 3, 0.25),
                        "and a grey has no hue and no saturation");

    expectations.expect(nearColor(run(Kernel::CombineHsv, {0.0, 1.0, 1.0, 1.0}), red),
                        "Combine HSV is its inverse");
    expectations.expect(
        nearColor(run(Kernel::CombineHsv, {120.0, 1.0, 1.0, 0.5}), {0.0, 1.0, 0.0, 0.5}),
        "and a third of the circle round is green");

    expectations.expect(nearColor(run(Kernel::HueShift, {red, 120.0}), {0.0, 1.0, 0.0, 1.0}, 1e-12),
                        "Hue Shift turns red into green a third of the circle round");
    // Hue is periodic, so a shift past the circle wraps rather than clamping.
    expectations.expect(
        nearColor(run(Kernel::HueShift, {red, 360.0}), red, 1e-12) &&
            nearColor(run(Kernel::HueShift, {red, -240.0}), {0.0, 1.0, 0.0, 1.0}, 1e-12),
        "and a shift past the circle wraps");
    expectations.expect(nearColor(run(Kernel::HueShift, {core::Color4d{0.5, 0.5, 0.5, 1.0}, 90.0}),
                                  {0.5, 0.5, 0.5, 1.0}, 1e-12),
                        "and a grey has no hue to shift");

    expectations.expect(near(run(Kernel::Luminance, {red}), 0, core::kRec709RedLuminanceWeight) &&
                            near(run(Kernel::Luminance, {core::Color4d{0.0, 1.0, 0.0, 1.0}}), 0,
                                 core::kRec709GreenLuminanceWeight) &&
                            near(run(Kernel::Luminance, {core::Color4d{0.0, 0.0, 1.0, 1.0}}), 0,
                                 core::kRec709BlueLuminanceWeight),
                        "Luminance uses the colour module's own Rec.709 weights");
    // Alpha is not a colour and takes no part.
    expectations.expect(near(run(Kernel::Luminance, {core::Color4d{1.0, 1.0, 1.0, 0.0}}), 0,
                             core::rec709Luminance({1.0, 1.0, 1.0, 1.0})),
                        "and alpha takes no part in it");
}

void testLogic(Expectations& expectations) {
    struct LogicCase final {
        bool left;
        bool right;
        document::BooleanOperation operation;
        bool result;
    };
    static const LogicCase kCases[]{
        {true, true, document::BooleanOperation::And, true},
        {true, false, document::BooleanOperation::And, false},
        {true, false, document::BooleanOperation::Or, true},
        {false, false, document::BooleanOperation::Or, false},
        {true, false, document::BooleanOperation::Xor, true},
        {true, true, document::BooleanOperation::Xor, false},
        {true, true, document::BooleanOperation::Nand, false},
        {true, false, document::BooleanOperation::Nand, true},
        {false, false, document::BooleanOperation::Nor, true},
        {true, false, document::BooleanOperation::Nor, false},
    };
    for (const auto& item : kCases) {
        const auto outcome = run(Kernel::BooleanLogic, {item.left, item.right},
                                 {document::selectorStoredValue(item.operation)});
        expectations.expect(!outcome.failed && holds(outcome, 0, item.result),
                            "Boolean Logic answers its truth table");
    }
    expectations.expect(holds(run(Kernel::BooleanNot, {true}), 0, false) &&
                            holds(run(Kernel::BooleanNot, {false}), 0, true),
                        "Boolean Not inverts");

    struct RangeCase final {
        double value;
        double minimum;
        double maximum;
        bool result;
    };
    static const RangeCase kRanges[]{
        {0.5, 0.0, 1.0, true},
        // INCLUSIVE at both ends.
        {0.0, 0.0, 1.0, true},
        {1.0, 0.0, 1.0, true},
        {-0.1, 0.0, 1.0, false},
        {1.1, 0.0, 1.0, false},
        // A reversed pair names no interval, so nothing is inside it.
        {0.5, 1.0, 0.0, false},
        {std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0, false},
    };
    for (const auto& item : kRanges) {
        const auto outcome = run(Kernel::InRange, {item.value, item.minimum, item.maximum});
        expectations.expect(!outcome.failed && holds(outcome, 0, item.result),
                            "In Range is inclusive and refuses a reversed pair");
    }
}

void testMalformedOperands(Expectations& expectations) {
    const auto outcome = run(Kernel::Luminance, {2.0});
    expectations.expect(outcome.failed && outcome.failedOperand == 0,
                        "a mistyped operand is reported and names its own position");
    expectations.expect(outcome.outputCount == 1 &&
                            std::holds_alternative<double>(outcome.outputs[0]),
                        "and the outcome still carries a value of the declared kind");
    const auto separated = run(Kernel::SeparateHsv, {std::string("not a color")});
    expectations.expect(separated.failed && separated.outputCount == 4,
                        "a four-output node still writes all four");
}

} // namespace

int main() {
    Expectations expectations;
    testMathSectionMembership(expectations);
    testIntegerMath(expectations);
    testShapingOperations(expectations);
    testGeometry(expectations);
    testColorOperations(expectations);
    testLogic(expectations);
    testMalformedOperands(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
