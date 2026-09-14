#include <bloom/runtime/value_utility_kernels.hpp>

#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
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

// One kernel call from plain values, which is all a library node's evaluation is: the plan's
// bookkeeping is the Evaluator's, and every operation here is a pure function of its operands.
[[nodiscard]] ValueUtilityOutcome runAt(const document::FrameRate rate, const Kernel kernel,
                                        const std::vector<CompiledValue>& values,
                                        const std::vector<std::int64_t>& selectors = {}) {
    std::vector<const CompiledValue*> operands;
    operands.reserve(values.size());
    for (const auto& value : values) {
        operands.push_back(&value);
    }
    return runtime::evaluateValueUtility(
        {kernel, operands, selectors, core::RationalTime::fromInteger(0), rate});
}

[[nodiscard]] ValueUtilityOutcome run(const Kernel kernel, const std::vector<CompiledValue>& values,
                                      const std::vector<std::int64_t>& selectors = {}) {
    return runAt(document::FrameRate::framesPerSecond24(), kernel, values, selectors);
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

// Every conversion's declared shape, checked against the descriptor table that produced both the
// registered definition and this kernel's operand order. A kernel that read its operands in a
// different order than the node declares them would compute the right answer from the wrong values.
void testDescriptorShapes(Expectations& expectations) {
    for (const auto& descriptor : document::valueUtilityDescriptors()) {
        expectations.expect(!descriptor.typeId.empty() && !descriptor.displayName.empty(),
                            "every descriptor names a type and an artist-facing name");
        expectations.expect(!descriptor.outputs.empty() &&
                                descriptor.outputs.size() <= runtime::kMaximumValueUtilityOutputs,
                            "every descriptor declares between one and four outputs");
        for (const auto& operand : descriptor.operands) {
            expectations.expect(operand.kind != document::SocketValueKind::Image,
                                "a value node never carries pixels");
        }
        expectations.expect(document::findValueUtilityDescriptor(descriptor.kernel) == &descriptor,
                            "a kernel names exactly one descriptor");
        expectations.expect(document::findValueUtilityDescriptor(descriptor.typeId) == &descriptor,
                            "a type id names exactly one descriptor");
    }
    // The safe-parse contract's own shape: a parsing node has a fallback operand and a `valid`
    // output, and neither is optional.
    for (const auto kernel : {Kernel::StringToScalar, Kernel::StringToInteger,
                              Kernel::StringToBoolean, Kernel::StringToColor}) {
        const auto* descriptor = document::findValueUtilityDescriptor(kernel);
        const bool shaped = descriptor != nullptr && descriptor->operands.size() == 2 &&
                            descriptor->operands[0].role == document::kTextPortName &&
                            descriptor->operands[0].kind == document::SocketValueKind::String &&
                            descriptor->operands[1].role == document::kFallbackPortName &&
                            descriptor->outputs.size() == 2 &&
                            descriptor->outputs[0].name == document::kValuePortName &&
                            descriptor->outputs[1].name == document::kValidPortName &&
                            descriptor->outputs[1].kind == document::SocketValueKind::Boolean &&
                            descriptor->outputs[0].kind == descriptor->operands[1].kind;
        expectations.expect(shaped,
                            "every parsing node carries a fallback operand and a valid flag");
    }
}

void testScalarAndIntegerToString(Expectations& expectations) {
    struct ScalarCase final {
        double value;
        std::int64_t decimals;
        bool trim;
        std::int64_t padWidth;
        std::string_view prefix;
        std::string_view suffix;
        std::string_view text;
    };
    static const ScalarCase kCases[]{
        {1.5, 2, false, 0, "", "", "1.50"},
        {1.5, 2, true, 0, "", "", "1.5"},
        {12.0, 0, false, 5, "", "", "00012"},
        {-3.25, 2, false, 0, "x = ", " px", "x = -3.25 px"},
        {0.0, 3, true, 0, "", "", "0"},
        // A negative decimal count and an absurd one are clamped rather than refused: a formatter
        // has no failure the artist could act on.
        {2.5, -4, false, 0, "", "", "2"},
        {2.5, 1000, true, 0, "", "", "2.5"},
        // Non-finite operands have no decimal spelling, so the number part is empty and the
        // decoration the artist authored still appears.
        {std::numeric_limits<double>::infinity(), 2, false, 0, "[", "]", "[]"},
        {std::numeric_limits<double>::quiet_NaN(), 2, false, 0, "", "", ""},
    };
    for (const auto& item : kCases) {
        const auto outcome =
            run(Kernel::ScalarToString, {item.value, item.decimals, item.trim, item.padWidth,
                                         std::string(item.prefix), std::string(item.suffix)});
        expectations.expect(!outcome.failed && holds(outcome, 0, std::string(item.text)),
                            "Scalar To String writes its documented spelling");
    }

    struct IntegerCase final {
        std::int64_t value;
        std::int64_t padWidth;
        std::int64_t radix;
        std::string_view text;
    };
    static const IntegerCase kIntegerCases[]{
        {255, 0, 16, "ff"},
        {255, 4, 16, "00ff"},
        {10, 0, 2, "1010"},
        {511, 0, 8, "777"},
        {-42, 5, 10, "-0042"},
        {0, 0, 10, "0"},
        {std::numeric_limits<std::int64_t>::min(), 0, 10, "-9223372036854775808"},
        // A radix the document could not have stored: the kernel computes base ten rather than
        // trusting a number that names no base.
        {7, 0, 99, "7"},
    };
    for (const auto& item : kIntegerCases) {
        const auto outcome =
            run(Kernel::IntegerToString, {item.value, item.padWidth, std::string{}, std::string{}},
                {item.radix});
        expectations.expect(!outcome.failed && holds(outcome, 0, std::string(item.text)),
                            "Integer To String writes its documented spelling at its radix");
    }
}

void testSafeParsing(Expectations& expectations) {
    struct ScalarCase final {
        std::string_view text;
        bool valid;
        double value;
    };
    static const ScalarCase kScalars[]{
        {"1.5", true, 1.5},        {"  -2  ", true, -2.0}, {"1e3", true, 1000.0},
        {".5", true, 0.5},         {"", false, 99.0},      {"   ", false, 99.0},
        {"abc", false, 99.0},      {"1abc", false, 99.0},  {"1,5", false, 99.0},
        {"inf", false, 99.0},      {"nan", false, 99.0},   {"0x10", false, 99.0},
        {"1e999999", false, 99.0}, {"١٢٣", false, 99.0},
    };
    for (const auto& item : kScalars) {
        const auto outcome = run(Kernel::StringToScalar, {std::string(item.text), 99.0});
        const bool answered =
            holds(outcome, 0, item.value) && holds(outcome, 1, item.valid) && !outcome.failed;
        expectations.expect(answered,
                            "String To Scalar answers the parse or the fallback, and says which");
    }

    struct IntegerCase final {
        std::string_view text;
        std::int64_t radix;
        bool valid;
        std::int64_t value;
    };
    static const IntegerCase kIntegers[]{
        {"42", 10, true, 42},    {"ff", 16, true, 255}, {"1010", 2, true, 10},
        {"  -7 ", 10, true, -7}, {"", 10, false, -1},   {"1.5", 10, false, -1},
        {"2", 2, false, -1},     {"g", 16, false, -1},  {"9223372036854775808", 10, false, -1},
    };
    for (const auto& item : kIntegers) {
        const auto outcome =
            run(Kernel::StringToInteger, {std::string(item.text), std::int64_t{-1}}, {item.radix});
        expectations.expect(holds(outcome, 0, item.value) && holds(outcome, 1, item.valid) &&
                                !outcome.failed,
                            "String To Integer answers the parse or the fallback at its radix");
    }

    struct BooleanCase final {
        std::string_view text;
        bool valid;
        bool value;
    };
    static const BooleanCase kBooleans[]{
        {"true", true, true}, {"TRUE", true, true},   {"yes", true, true}, {"on", true, true},
        {"1", true, true},    {"false", true, false}, {"no", true, false}, {"off", true, false},
        {"0", true, false},   {"", false, true},      {" ", false, true},  {"maybe", false, true},
        {"2", false, true},
    };
    for (const auto& item : kBooleans) {
        const auto outcome = run(Kernel::StringToBoolean, {std::string(item.text), true});
        expectations.expect(holds(outcome, 0, item.value) && holds(outcome, 1, item.valid) &&
                                !outcome.failed,
                            "String To Boolean reads its documented spellings and nothing else");
    }

    const core::Color4d fallback{0.1, 0.2, 0.3, 0.4};
    struct ColorCase final {
        std::string_view text;
        bool valid;
        core::Color4d value;
    };
    static const ColorCase kColors[]{
        {"#ffffff", true, {1.0, 1.0, 1.0, 1.0}},
        {"#000000", true, {0.0, 0.0, 0.0, 1.0}},
        {"  #00FF0080  ", true, {0.0, 1.0, 0.0, 128.0 / 255.0}},
        {"", false, {0.1, 0.2, 0.3, 0.4}},
        {"#fff", false, {0.1, 0.2, 0.3, 0.4}},
        {"ffffff", false, {0.1, 0.2, 0.3, 0.4}},
        {"#gggggg", false, {0.1, 0.2, 0.3, 0.4}},
    };
    for (const auto& item : kColors) {
        const auto outcome = run(Kernel::StringToColor, {std::string(item.text), fallback});
        expectations.expect(holds(outcome, 0, item.value) && holds(outcome, 1, item.valid) &&
                                !outcome.failed,
                            "String To Color reads #RRGGBB and #RRGGBBAA and nothing else");
    }
}

void testNumericConversions(Expectations& expectations) {
    struct RoundingCase final {
        double value;
        document::RoundingMode mode;
        std::int64_t expected;
    };
    static const RoundingCase kCases[]{
        // Ties go to EVEN, the frozen scalar primitive's rule, so this node and a Rounding node
        // never disagree about 2.5.
        {2.5, document::RoundingMode::Round, 2},
        {3.5, document::RoundingMode::Round, 4},
        {-2.5, document::RoundingMode::Round, -2},
        {2.9, document::RoundingMode::Floor, 2},
        {-2.1, document::RoundingMode::Floor, -3},
        {2.1, document::RoundingMode::Ceiling, 3},
        {-2.9, document::RoundingMode::Ceiling, -2},
        {2.9, document::RoundingMode::Truncate, 2},
        {-2.9, document::RoundingMode::Truncate, -2},
        // The edges: NaN names no integer at all, and a magnitude past the signed range saturates
        // rather than wrapping into an unrelated number.
        {std::numeric_limits<double>::quiet_NaN(), document::RoundingMode::Round, 0},
        {std::numeric_limits<double>::infinity(), document::RoundingMode::Round,
         std::numeric_limits<std::int64_t>::max()},
        {-std::numeric_limits<double>::infinity(), document::RoundingMode::Round,
         std::numeric_limits<std::int64_t>::min()},
        {1e30, document::RoundingMode::Floor, std::numeric_limits<std::int64_t>::max()},
    };
    for (const auto& item : kCases) {
        const auto outcome =
            run(Kernel::ScalarToInteger, {item.value}, {document::selectorStoredValue(item.mode)});
        expectations.expect(!outcome.failed && holds(outcome, 0, item.expected),
                            "Scalar To Integer rounds as its mode says and saturates at the edges");
    }

    expectations.expect(holds(run(Kernel::IntegerToScalar, {std::int64_t{-5}}), 0, -5.0),
                        "Integer To Scalar widens exactly");
    expectations.expect(holds(run(Kernel::BooleanToScalar, {true}), 0, 1.0) &&
                            holds(run(Kernel::BooleanToScalar, {false}), 0, 0.0),
                        "Boolean To Scalar is the one mapping every stored boolean has");
    expectations.expect(holds(run(Kernel::BooleanToInteger, {true}), 0, std::int64_t{1}) &&
                            holds(run(Kernel::BooleanToInteger, {false}), 0, std::int64_t{0}),
                        "Boolean To Integer is the same mapping");
    expectations.expect(holds(run(Kernel::IntegerToBoolean, {std::int64_t{0}}), 0, false) &&
                            holds(run(Kernel::IntegerToBoolean, {std::int64_t{-3}}), 0, true),
                        "Integer To Boolean is nonzero");
    // Nonzero is true, and NaN is FALSE: `value != 0` would call a number that is not a number set.
    expectations.expect(
        holds(run(Kernel::ScalarToBoolean, {0.0}), 0, false) &&
            holds(run(Kernel::ScalarToBoolean, {-0.0}), 0, false) &&
            holds(run(Kernel::ScalarToBoolean, {1e-300}), 0, true) &&
            holds(run(Kernel::ScalarToBoolean, {std::numeric_limits<double>::quiet_NaN()}), 0,
                  false) &&
            holds(run(Kernel::ScalarToBoolean, {std::numeric_limits<double>::infinity()}), 0, true),
        "Scalar To Boolean is nonzero, with NaN false and negative zero false");

    const auto labelled =
        run(Kernel::BooleanToString, {true, std::string("ON AIR"), std::string("off air")});
    expectations.expect(holds(labelled, 0, std::string("ON AIR")),
                        "Boolean To String writes the label the artist authored");
    expectations.expect(
        holds(run(Kernel::BooleanToString, {false, std::string("ON AIR"), std::string("off air")}),
              0, std::string("off air")),
        "and its counterpart when false");
}

void testStructuredConversions(Expectations& expectations) {
    const core::Color4d color{0.25, 0.5, 0.75, 0.5};
    expectations.expect(
        holds(run(Kernel::ColorToVector3, {color}), 0, document::Vec3d{0.25, 0.5, 0.75}),
        "Color To Vector 3 carries RGB and drops alpha");
    expectations.expect(
        holds(run(Kernel::Vector3ToColor, {document::Vec3d{0.25, 0.5, 0.75}, 0.5}), 0, color),
        "Vector 3 To Color supplies alpha from its own operand");
    expectations.expect(holds(run(Kernel::Vector2ToVector3, {document::Vec2d{1.0, 2.0}, 3.0}), 0,
                              document::Vec3d{1.0, 2.0, 3.0}),
                        "Vector 2 To Vector 3 supplies Z");
    expectations.expect(holds(run(Kernel::Vector3ToVector2, {document::Vec3d{1.0, 2.0, 3.0}}), 0,
                              document::Vec2d{1.0, 2.0}),
                        "Vector 3 To Vector 2 drops Z rather than projecting through it");

    expectations.expect(
        holds(run(Kernel::ColorToString, {color, false, false}), 0, std::string("#4080bf")),
        "Color To String writes #RRGGBB by default");
    expectations.expect(
        holds(run(Kernel::ColorToString, {color, true, true}), 0, std::string("#4080BF80")),
        "and #RRGGBBAA in upper case on request");

    expectations.expect(holds(run(Kernel::Vector2ToString,
                                  {document::Vec2d{1.0, -2.5}, std::string(", "), std::int64_t{2}}),
                              0, std::string("1.00, -2.50")),
                        "Vector 2 To String joins its components with the authored separator");
    expectations.expect(holds(run(Kernel::Vector3ToString, {document::Vec3d{1.0, 2.0, 3.0},
                                                            std::string(" "), std::int64_t{0}}),
                              0, std::string("1 2 3")),
                        "Vector 3 To String does the same across three");
}

// An operand that arrives as the wrong alternative is a MALFORMED PLAN -- the document's typing
// refuses such a link, and every accepted widening became its own promotion operation. The kernel
// reports it, names the operand, and still writes outputs of the right kinds.
void testMalformedOperands(Expectations& expectations) {
    const auto outcome =
        run(Kernel::ScalarToString, {std::string("not a number"), std::int64_t{2}, false,
                                     std::int64_t{0}, std::string{}, std::string{}});
    expectations.expect(outcome.failed && outcome.failedOperand == 0 && !outcome.summary.empty(),
                        "a mistyped operand is reported and names its own position");
    expectations.expect(outcome.outputCount == 1 &&
                            std::holds_alternative<std::string>(outcome.outputs[0]),
                        "and the outcome still carries a value of the declared kind");

    const auto missing = runtime::evaluateValueUtility({Kernel::StringToScalar,
                                                        {},
                                                        {},
                                                        core::RationalTime::fromInteger(0),
                                                        document::FrameRate::framesPerSecond24()});
    expectations.expect(missing.failed && missing.outputCount == 2 &&
                            std::holds_alternative<double>(missing.outputs[0]) &&
                            std::holds_alternative<bool>(missing.outputs[1]),
                        "an operand the plan never supplied is reported, not read");
}

// The composition frame rate is the ONE rate these read: a Seconds To Frames node and a Time node
// must never disagree about which frame an instant falls in.
void testTimeConversions(Expectations& expectations) {
    const auto rate = document::FrameRate::framesPerSecond24();
    struct FrameCase final {
        double seconds;
        std::int64_t frames;
    };
    static const FrameCase kFrames[]{
        {0.0, 0},
        {1.0, 24},
        {2.5, 60},
        {-1.0, -24},
        // Floored, not rounded: the frame an instant falls INSIDE is the frame being rendered.
        {0.999, 23},
        {-0.5, -12},
        // The edges: NaN names no frame, and an unreachable magnitude saturates.
        {std::numeric_limits<double>::quiet_NaN(), 0},
        {std::numeric_limits<double>::infinity(), std::numeric_limits<std::int64_t>::max()},
    };
    for (const auto& item : kFrames) {
        expectations.expect(
            holds(runAt(rate, Kernel::SecondsToFrames, {item.seconds}), 0, item.frames),
            "Seconds To Frames floors at the composition rate");
    }
    expectations.expect(
        holds(runAt(rate, Kernel::FramesToSeconds, {std::int64_t{24}}), 0, 1.0) &&
            holds(runAt(rate, Kernel::FramesToSeconds, {std::int64_t{12}}), 0, 0.5) &&
            holds(runAt(rate, Kernel::FramesToSeconds, {std::int64_t{-24}}), 0, -1.0),
        "Frames To Seconds is its exact inverse at a frame-aligned time");

    struct TimecodeCase final {
        double seconds;
        std::string_view label;
    };
    static const TimecodeCase kLabels[]{
        {0.0, "00:00:00:00"},
        {1.0, "00:00:01:00"},
        {2.5, "00:00:02:12"},
        {60.0, "00:01:00:00"},
        {3600.0, "01:00:00:00"},
        // Hours are not wrapped at 24: a composition may be longer than a day.
        {90000.0, "25:00:00:00"},
        // A negative time carries one sign on the whole label rather than on a field.
        {-1.0, "-00:00:01:00"},
        {-0.5, "-00:00:00:12"},
    };
    for (const auto& item : kLabels) {
        expectations.expect(holds(runAt(rate, Kernel::SecondsToTimecode, {item.seconds}), 0,
                                  std::string(item.label)),
                            "Seconds To Timecode writes non-drop HH:MM:SS:FF");
    }

    struct ParsedTimecode final {
        std::string_view label;
        bool valid;
        double seconds;
    };
    static const ParsedTimecode kParsed[]{
        {"00:00:02:12", true, 2.5},
        {"02:12", true, 2.5},
        {"01:30:00", true, 90.0},
        {"  00:00:01:00  ", true, 1.0},
        {"-00:00:01:00", true, -1.0},
        {"", false, -7.0},
        {"12", false, -7.0},
        {"00:00:00:24", false, -7.0},
        {"00:00:60:00", false, -7.0},
        {"00:70:00:00", false, -7.0},
        // `;` before the frame field MEANS drop-frame, which Bloom does not support, so reading it
        // as non-drop would name a different frame than the label does.
        {"00:00:02;12", false, -7.0},
        {"00:00:0a:12", false, -7.0},
        {"00:00:-2:12", false, -7.0},
        {"1:2:3:4:5", false, -7.0},
        {"00::02:12", false, -7.0},
    };
    for (const auto& item : kParsed) {
        const auto outcome =
            runAt(rate, Kernel::TimecodeToSeconds, {std::string(item.label), -7.0});
        expectations.expect(holds(outcome, 0, item.seconds) && holds(outcome, 1, item.valid) &&
                                !outcome.failed,
                            "Timecode To Seconds reads its three forms and falls back otherwise");
    }

    // A fractional rate counts the frame field to its NOMINAL whole number, which is what non-drop
    // timecode is: the label drifts from wall clock, and drop-frame -- the correction -- is not
    // supported.
    const auto broadcast = document::FrameRate::create(30000, 1001);
    expectations.expect(broadcast.has_value(), "29.97 is a rate a composition may hold");
    if (broadcast.has_value()) {
        expectations.expect(holds(runAt(*broadcast, Kernel::SecondsToTimecode, {1.0}), 0,
                                  std::string("00:00:00:29")),
                            "one wall-clock second at 29.97 is 29 non-drop frames, not a second");
        const auto refused =
            runAt(*broadcast, Kernel::TimecodeToSeconds, {std::string("00:00:00:30"), -7.0});
        expectations.expect(holds(refused, 1, false),
                            "a frame field at the nominal count names no frame");
    }
}

} // namespace

int main() {
    Expectations expectations;
    testDescriptorShapes(expectations);
    testScalarAndIntegerToString(expectations);
    testSafeParsing(expectations);
    testNumericConversions(expectations);
    testStructuredConversions(expectations);
    testTimeConversions(expectations);
    testMalformedOperands(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
