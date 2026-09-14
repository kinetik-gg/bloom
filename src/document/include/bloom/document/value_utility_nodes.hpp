#pragma once

#include <bloom/core/safe_parse.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace bloom::document {

// The conversion, string, logic, numeric and readout library (task UTIL-1).
//
// WHY ONE TABLE RATHER THAN SIXTY-TWO DEFINITIONS. The value library's first slice (task S7) gave
// each node shape its own builder, which was right for twenty shapes that genuinely differ -- a
// Math node's five relabelled operands and a Switch's two same-kind branches are not the same
// thing said twice. This slice adds sixty-two nodes whose shapes differ only in WHICH SOCKETS THEY
// HAVE, and sixty-two near-identical builders would be sixty-two places for a socket kind, a
// parameter kind and a default to disagree.
//
// So the shape is DATA. One descriptor per node type names its sockets, its inline selectors and
// its outputs, and four consumers read that one table:
//
//   * value_node_definitions.cpp builds the NodeDefinition,
//   * detail::hasValidValueLoweringShape() validates a registered definition AGAINST it,
//   * the compiler lowers a node's operands in its order,
//   * the evaluator's kernel reads its operands in that same order.
//
// A node type therefore cannot be added half-way: a descriptor with no kernel does not compile, and
// a kernel with no descriptor has nothing to lower.
//
// THE KERNEL ENUMERATION IS NOT PERSISTED. A document stores the TYPE ID, and the type id is what
// names the operation; ValueUtilityKernel exists only to get from a registered definition to a
// compiled kernel inside one process. It may be reordered freely, unlike every enumeration in
// value_operations.hpp whose stored integer is the durable authoring value.

enum class ValueUtilityKernel : std::uint8_t {
    // Conversions (deliverable 1).
    ScalarToString,
    IntegerToString,
    StringToScalar,
    StringToInteger,
    ScalarToInteger,
    IntegerToScalar,
    BooleanToScalar,
    BooleanToInteger,
    ScalarToBoolean,
    IntegerToBoolean,
    BooleanToString,
    StringToBoolean,
    ColorToString,
    StringToColor,
    ColorToVector3,
    Vector3ToColor,
    Vector2ToVector3,
    Vector3ToVector2,
    Vector2ToString,
    Vector3ToString,
    // Time conversions (deliverable 2).
    SecondsToFrames,
    FramesToSeconds,
    SecondsToTimecode,
    TimecodeToSeconds,
    // String utilities (deliverable 3).
    StringConcatenate,
    StringFormat,
    StringLength,
    StringSubstring,
    StringCharacterAt,
    StringSplit,
    StringReplace,
    StringTrim,
    StringCase,
    StringPad,
    StringRepeat,
    StringContains,
    StringStartsWith,
    StringEndsWith,
    StringEquals,
    // Numeric gaps (deliverable 4), in the Math section.
    IntegerMath,
    Rounding,
    Sign,
    Wrap,
    Snap,
    PingPong,
    Smoothstep,
    DegreesToRadians,
    RadiansToDegrees,
    Rotate2d,
    PolarToCartesian,
    CartesianToPolar,
    SeparateHsv,
    CombineHsv,
    HueShift,
    Luminance,
    // Logic (deliverable 4), which stays in Utilities: a predicate is not arithmetic.
    BooleanLogic,
    BooleanNot,
    InRange,
};

// ---------------------------------------------------------------------------------------------
// Type ids
// ---------------------------------------------------------------------------------------------

inline constexpr std::string_view kScalarToStringNodeType = "bloom.scalar-to-string";
inline constexpr std::string_view kIntegerToStringNodeType = "bloom.integer-to-string";
inline constexpr std::string_view kStringToScalarNodeType = "bloom.string-to-scalar";
inline constexpr std::string_view kStringToIntegerNodeType = "bloom.string-to-integer";
inline constexpr std::string_view kScalarToIntegerNodeType = "bloom.scalar-to-integer";
inline constexpr std::string_view kIntegerToScalarNodeType = "bloom.integer-to-scalar";
inline constexpr std::string_view kBooleanToScalarNodeType = "bloom.boolean-to-scalar";
inline constexpr std::string_view kBooleanToIntegerNodeType = "bloom.boolean-to-integer";
inline constexpr std::string_view kScalarToBooleanNodeType = "bloom.scalar-to-boolean";
inline constexpr std::string_view kIntegerToBooleanNodeType = "bloom.integer-to-boolean";
inline constexpr std::string_view kBooleanToStringNodeType = "bloom.boolean-to-string";
inline constexpr std::string_view kStringToBooleanNodeType = "bloom.string-to-boolean";
inline constexpr std::string_view kColorToStringNodeType = "bloom.color-to-string";
inline constexpr std::string_view kStringToColorNodeType = "bloom.string-to-color";
inline constexpr std::string_view kColorToVector3NodeType = "bloom.color-to-vector3";
inline constexpr std::string_view kVector3ToColorNodeType = "bloom.vector3-to-color";
inline constexpr std::string_view kVector2ToVector3NodeType = "bloom.vector2-to-vector3";
inline constexpr std::string_view kVector3ToVector2NodeType = "bloom.vector3-to-vector2";
inline constexpr std::string_view kVector2ToStringNodeType = "bloom.vector2-to-string";
inline constexpr std::string_view kVector3ToStringNodeType = "bloom.vector3-to-string";

inline constexpr std::string_view kSecondsToFramesNodeType = "bloom.seconds-to-frames";
inline constexpr std::string_view kFramesToSecondsNodeType = "bloom.frames-to-seconds";
inline constexpr std::string_view kSecondsToTimecodeNodeType = "bloom.seconds-to-timecode";
inline constexpr std::string_view kTimecodeToSecondsNodeType = "bloom.timecode-to-seconds";

inline constexpr std::string_view kStringConcatenateNodeType = "bloom.string-concatenate";
inline constexpr std::string_view kStringFormatNodeType = "bloom.string-format";
inline constexpr std::string_view kStringLengthNodeType = "bloom.string-length";
inline constexpr std::string_view kStringSubstringNodeType = "bloom.string-substring";
inline constexpr std::string_view kStringCharacterAtNodeType = "bloom.string-character-at";
inline constexpr std::string_view kStringSplitNodeType = "bloom.string-split";
inline constexpr std::string_view kStringReplaceNodeType = "bloom.string-replace";
inline constexpr std::string_view kStringTrimNodeType = "bloom.string-trim";
inline constexpr std::string_view kStringCaseNodeType = "bloom.string-case";
inline constexpr std::string_view kStringPadNodeType = "bloom.string-pad";
inline constexpr std::string_view kStringRepeatNodeType = "bloom.string-repeat";
inline constexpr std::string_view kStringContainsNodeType = "bloom.string-contains";
inline constexpr std::string_view kStringStartsWithNodeType = "bloom.string-starts-with";
inline constexpr std::string_view kStringEndsWithNodeType = "bloom.string-ends-with";
inline constexpr std::string_view kStringEqualsNodeType = "bloom.string-equals";

inline constexpr std::string_view kIntegerMathNodeType = "bloom.integer-math";
inline constexpr std::string_view kRoundingNodeType = "bloom.rounding";
inline constexpr std::string_view kSignNodeType = "bloom.sign";
inline constexpr std::string_view kWrapNodeType = "bloom.wrap";
inline constexpr std::string_view kSnapNodeType = "bloom.snap";
inline constexpr std::string_view kPingPongNodeType = "bloom.ping-pong";
inline constexpr std::string_view kSmoothstepNodeType = "bloom.smoothstep";
inline constexpr std::string_view kDegreesToRadiansNodeType = "bloom.degrees-to-radians";
inline constexpr std::string_view kRadiansToDegreesNodeType = "bloom.radians-to-degrees";
inline constexpr std::string_view kRotate2dNodeType = "bloom.rotate-2d";
inline constexpr std::string_view kPolarToCartesianNodeType = "bloom.polar-to-cartesian";
inline constexpr std::string_view kCartesianToPolarNodeType = "bloom.cartesian-to-polar";
inline constexpr std::string_view kSeparateHsvNodeType = "bloom.separate-hsv";
inline constexpr std::string_view kCombineHsvNodeType = "bloom.combine-hsv";
inline constexpr std::string_view kHueShiftNodeType = "bloom.hue-shift";
inline constexpr std::string_view kLuminanceNodeType = "bloom.luminance";

inline constexpr std::string_view kBooleanLogicNodeType = "bloom.boolean-logic";
inline constexpr std::string_view kBooleanNotNodeType = "bloom.boolean-not";
inline constexpr std::string_view kInRangeNodeType = "bloom.in-range";

// ---------------------------------------------------------------------------------------------
// The selector vocabularies
// ---------------------------------------------------------------------------------------------
//
// Each of these IS persisted -- the stored integer is the durable authoring value, exactly as
// value_operations.hpp's are -- so appending a member is additive and renumbering one silently
// reinterprets every saved document.

// How a Scalar becomes an Integer, and what a Rounding node does to a Scalar. The four members are
// the four ScalarPrimitive rounding operations, named again here because a document stores this
// mapping and must not inherit the primitive vocabulary's own numbering.
enum class RoundingMode : std::uint8_t {
    Round = 0,
    Floor = 1,
    Ceiling = 2,
    Truncate = 3,
};

inline constexpr std::array<RoundingMode, 4> kRoundingModes{
    RoundingMode::Round, RoundingMode::Floor, RoundingMode::Ceiling, RoundingMode::Truncate};

inline constexpr RoundingMode kDefaultRoundingMode = RoundingMode::Round;

// Case mapping is ASCII-only, and the vocabulary says so by having exactly these three members: a
// Unicode case mapping is locale-sensitive, context-sensitive and tied to a table version, so a
// node claiming to do it would answer differently on a different machine.
enum class StringCaseMode : std::uint8_t {
    Upper = 0,
    Lower = 1,
    Title = 2,
};

inline constexpr std::array<StringCaseMode, 3> kStringCaseModes{
    StringCaseMode::Upper, StringCaseMode::Lower, StringCaseMode::Title};

inline constexpr StringCaseMode kDefaultStringCaseMode = StringCaseMode::Upper;

enum class StringPadSide : std::uint8_t {
    Start = 0,
    End = 1,
};

inline constexpr std::array<StringPadSide, 2> kStringPadSides{StringPadSide::Start,
                                                              StringPadSide::End};

inline constexpr StringPadSide kDefaultStringPadSide = StringPadSide::Start;

// Integer arithmetic. Divide and Modulo by zero answer 0 -- the same documented fallback the scalar
// tranche already gives -- and every operation saturates at the signed range rather than wrapping,
// because signed overflow is undefined and a value graph's operands come from anywhere.
enum class IntegerOperation : std::uint8_t {
    Add = 0,
    Subtract = 1,
    Multiply = 2,
    Divide = 3,
    Modulo = 4,
    Minimum = 5,
    Maximum = 6,
};

inline constexpr std::array<IntegerOperation, 7> kIntegerOperations{
    IntegerOperation::Add,    IntegerOperation::Subtract, IntegerOperation::Multiply,
    IntegerOperation::Divide, IntegerOperation::Modulo,   IntegerOperation::Minimum,
    IntegerOperation::Maximum};

inline constexpr IntegerOperation kDefaultIntegerOperation = IntegerOperation::Add;

enum class BooleanOperation : std::uint8_t {
    And = 0,
    Or = 1,
    Xor = 2,
    Nand = 3,
    Nor = 4,
};

inline constexpr std::array<BooleanOperation, 5> kBooleanOperations{
    BooleanOperation::And, BooleanOperation::Or, BooleanOperation::Xor, BooleanOperation::Nand,
    BooleanOperation::Nor};

inline constexpr BooleanOperation kDefaultBooleanOperation = BooleanOperation::And;

// The radices an Integer conversion offers on its card. The stored value is the radix ITSELF, not
// an index into this list, so a document that stores 16 means base sixteen whatever this list says
// -- the list is only what the dropdown offers, and core::parseInteger() accepts any radix in
// [kMinimumRadix, kMaximumRadix].
inline constexpr std::array<std::int64_t, 4> kOfferedRadices{2, 8, 10, 16};

template <typename Enumeration>
[[nodiscard]] constexpr std::int64_t selectorStoredValue(const Enumeration value) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint8_t>(value));
}

template <typename Enumeration, std::size_t Count>
[[nodiscard]] constexpr std::optional<Enumeration>
selectorFromStoredValue(const std::array<Enumeration, Count>& vocabulary,
                        const std::int64_t stored) noexcept {
    for (const auto candidate : vocabulary) {
        if (selectorStoredValue(candidate) == stored) {
            return candidate;
        }
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool isValidStoredRadix(const std::int64_t stored) noexcept {
    return stored >= core::kMinimumRadix && stored <= core::kMaximumRadix;
}

// ---------------------------------------------------------------------------------------------
// The descriptor table
// ---------------------------------------------------------------------------------------------

// One socket-backed operand. `kind` is the SOCKET's kind; the backing parameter's kind follows from
// it, because socketKindForParameterValueKind() is already the one statement of that pairing and a
// second field here could contradict it.
struct ValueUtilityOperand final {
    std::string_view role;
    SocketValueKind kind = SocketValueKind::Scalar;
    // The authored default, read according to `kind`: `number` for a Scalar and as the first
    // component of a vector, `y` and `z` for the rest of one, `integer`, `flag` and `text` for the
    // kinds that carry those. A Color operand always defaults to kDefaultValueColor -- opaque
    // white, the default every other colour schema in Bloom already has.
    double number = 0.0;
    double y = 0.0;
    double z = 0.0;
    std::int64_t integer = 0;
    bool flag = false;
    std::string_view text{};
};

// An inline selector: a closed integer vocabulary with NO socket, exactly as a Math node's
// operation already is. A mode is a choice the author makes about what the node IS, not a quantity
// something upstream computes, which is why it is not linkable.
struct ValueUtilitySelector final {
    std::string_view role;
    std::string_view schemaKey;
    std::int64_t defaultValue = 0;
};

struct ValueUtilityOutput final {
    std::string_view name;
    SocketValueKind kind = SocketValueKind::Scalar;
};

struct ValueUtilityDescriptor final {
    std::string_view typeId;
    // The artist's name for the node, read by the Add surfaces and the card header. Held here so
    // the node's shape and its name are one record rather than a type id repeated in a UI table.
    std::string_view displayName;
    ValueUtilityKernel kernel = ValueUtilityKernel::ScalarToString;
    NodeCategory category = NodeCategory::Utilities;
    std::span<const ValueUtilityOperand> operands;
    std::span<const ValueUtilitySelector> selectors;
    std::span<const ValueUtilityOutput> outputs;
};

// Every node type this library declares, in Add-surface reading order within each category.
[[nodiscard]] std::span<const ValueUtilityDescriptor> valueUtilityDescriptors() noexcept;

[[nodiscard]] const ValueUtilityDescriptor*
findValueUtilityDescriptor(std::string_view typeId) noexcept;

// The same record, found by the kernel it names. The evaluator has a compiled kernel and no type id
// -- a plan carries the operation, not the document's spelling of it -- and it needs the output
// SHAPE to fill a failed operation's fallbacks with values of the right kinds.
[[nodiscard]] const ValueUtilityDescriptor*
findValueUtilityDescriptor(ValueUtilityKernel kernel) noexcept;

// ---------------------------------------------------------------------------------------------
// Shared port names
// ---------------------------------------------------------------------------------------------
//
// Named here rather than spelled at each use for the reason value_nodes.hpp gives: a port name is
// the pairing between a socket and the parameter behind it, and a typo in one of two spellings
// would register a definition whose socket has no value.

inline constexpr std::string_view kTextPortName = "text";
inline constexpr std::string_view kFallbackPortName = "fallback";
inline constexpr std::string_view kValidPortName = "valid";
inline constexpr std::string_view kModeParameterRole = "mode";
inline constexpr std::string_view kRadixParameterRole = "radix";
inline constexpr std::string_view kSecondsPortName = "seconds";
inline constexpr std::string_view kFramesPortName = "frames";
inline constexpr std::string_view kSeparatorPortName = "separator";
inline constexpr std::string_view kIndexPortName = "index";
inline constexpr std::string_view kSearchPortName = "search";
inline constexpr std::string_view kCaseSensitivePortName = "caseSensitive";
inline constexpr std::string_view kMinimumPortName = "min";
inline constexpr std::string_view kMaximumPortName = "max";
inline constexpr std::string_view kDegreesPortName = "degrees";
inline constexpr std::string_view kRadiusPortName = "radius";
inline constexpr std::string_view kAnglePortName = "angle";
inline constexpr std::string_view kHuePortName = "hue";
inline constexpr std::string_view kSaturationPortName = "saturation";
inline constexpr std::string_view kAlphaPortName = "alpha";

// DROP-FRAME TIMECODE IS NOT SUPPORTED, and is documented as unsupported rather than approximated.
// A drop-frame count is a different mapping from frame numbers to wall clock -- it skips two labels
// a minute to keep a 29.97 count near real time -- and a node that printed `HH:MM:SS:FF` while
// meaning drop-frame would name a different frame than the one it showed. Bloom's timecode is
// NON-DROP: the frame field counts at the rate's NOMINAL integer frame count, and the label drifts
// from wall clock at a fractional rate exactly as non-drop timecode is supposed to.
inline constexpr char kTimecodeFieldSeparator = ':';

} // namespace bloom::document
