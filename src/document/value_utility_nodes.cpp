#include <bloom/document/value_utility_nodes.hpp>

#include <bloom/document/parameter.hpp>
#include <bloom/document/value_nodes.hpp>

#include <algorithm>
#include <array>

namespace {

using bloom::document::NodeCategory;
using bloom::document::SocketValueKind;
using bloom::document::ValueUtilityDescriptor;
using bloom::document::ValueUtilityKernel;
using bloom::document::ValueUtilityOperand;
using bloom::document::ValueUtilityOutput;
using bloom::document::ValueUtilitySelector;

// The shared shapes. A node's outputs are usually one value called `result`; a node that PARSES has
// two, because the safe-parse contract says so and no parsing node may omit either.
constexpr std::array<ValueUtilityOutput, 1> kScalarResult{
    ValueUtilityOutput{bloom::document::kResultPortName, SocketValueKind::Scalar}};
constexpr std::array<ValueUtilityOutput, 1> kIntegerResult{
    ValueUtilityOutput{bloom::document::kResultPortName, SocketValueKind::Integer}};
constexpr std::array<ValueUtilityOutput, 1> kBooleanResult{
    ValueUtilityOutput{bloom::document::kResultPortName, SocketValueKind::Boolean}};
constexpr std::array<ValueUtilityOutput, 1> kStringResult{
    ValueUtilityOutput{bloom::document::kResultPortName, SocketValueKind::String}};
constexpr std::array<ValueUtilityOutput, 1> kColorResult{
    ValueUtilityOutput{bloom::document::kResultPortName, SocketValueKind::Color}};
constexpr std::array<ValueUtilityOutput, 1> kVector2Result{
    ValueUtilityOutput{bloom::document::kResultPortName, SocketValueKind::Vector2}};
constexpr std::array<ValueUtilityOutput, 1> kVector3Result{
    ValueUtilityOutput{bloom::document::kResultPortName, SocketValueKind::Vector3}};

// The safe-parse output pair: the parsed value (or the fallback) and the Boolean that says which of
// the two it is. Both are always written, so a graph downstream never has to ask whether the node
// ran.
constexpr std::array<ValueUtilityOutput, 2> kParsedScalar{
    ValueUtilityOutput{bloom::document::kValuePortName, SocketValueKind::Scalar},
    ValueUtilityOutput{bloom::document::kValidPortName, SocketValueKind::Boolean}};
constexpr std::array<ValueUtilityOutput, 2> kParsedInteger{
    ValueUtilityOutput{bloom::document::kValuePortName, SocketValueKind::Integer},
    ValueUtilityOutput{bloom::document::kValidPortName, SocketValueKind::Boolean}};
constexpr std::array<ValueUtilityOutput, 2> kParsedBoolean{
    ValueUtilityOutput{bloom::document::kValuePortName, SocketValueKind::Boolean},
    ValueUtilityOutput{bloom::document::kValidPortName, SocketValueKind::Boolean}};
constexpr std::array<ValueUtilityOutput, 2> kParsedColor{
    ValueUtilityOutput{bloom::document::kValuePortName, SocketValueKind::Color},
    ValueUtilityOutput{bloom::document::kValidPortName, SocketValueKind::Boolean}};

constexpr std::array<ValueUtilitySelector, 0> kNoSelectors{};

// The radix selector, shared by both Integer conversions so the two cannot offer different bases.
constexpr std::array<ValueUtilitySelector, 1> kRadixSelector{ValueUtilitySelector{
    bloom::document::kRadixParameterRole, bloom::document::kNumberRadixParameterSchemaKey,
    bloom::core::kDefaultRadix}};

constexpr std::array<ValueUtilitySelector, 1> kRoundingSelector{ValueUtilitySelector{
    bloom::document::kModeParameterRole, bloom::document::kRoundingModeParameterSchemaKey,
    bloom::document::selectorStoredValue(bloom::document::kDefaultRoundingMode)}};

// ---------------------------------------------------------------------------------------------
// Deliverable 1: conversions
// ---------------------------------------------------------------------------------------------

constexpr std::array<ValueUtilityOperand, 6> kScalarToStringOperands{
    ValueUtilityOperand{.role = bloom::document::kValuePortName, .kind = SocketValueKind::Scalar},
    ValueUtilityOperand{.role = "decimals", .kind = SocketValueKind::Integer, .integer = 2},
    ValueUtilityOperand{.role = "trimZeros", .kind = SocketValueKind::Boolean, .flag = false},
    ValueUtilityOperand{.role = "padWidth", .kind = SocketValueKind::Integer, .integer = 0},
    ValueUtilityOperand{.role = "prefix", .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = "suffix", .kind = SocketValueKind::String}};

constexpr std::array<ValueUtilityOperand, 4> kIntegerToStringOperands{
    ValueUtilityOperand{.role = bloom::document::kValuePortName, .kind = SocketValueKind::Integer},
    ValueUtilityOperand{.role = "padWidth", .kind = SocketValueKind::Integer, .integer = 0},
    ValueUtilityOperand{.role = "prefix", .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = "suffix", .kind = SocketValueKind::String}};

constexpr std::array<ValueUtilityOperand, 2> kStringToScalarOperands{
    ValueUtilityOperand{.role = bloom::document::kTextPortName, .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = bloom::document::kFallbackPortName,
                        .kind = SocketValueKind::Scalar}};

constexpr std::array<ValueUtilityOperand, 2> kStringToIntegerOperands{
    ValueUtilityOperand{.role = bloom::document::kTextPortName, .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = bloom::document::kFallbackPortName,
                        .kind = SocketValueKind::Integer}};

constexpr std::array<ValueUtilityOperand, 2> kStringToBooleanOperands{
    ValueUtilityOperand{.role = bloom::document::kTextPortName, .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = bloom::document::kFallbackPortName,
                        .kind = SocketValueKind::Boolean}};

constexpr std::array<ValueUtilityOperand, 2> kStringToColorOperands{
    ValueUtilityOperand{.role = bloom::document::kTextPortName, .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = bloom::document::kFallbackPortName,
                        .kind = SocketValueKind::Color}};

constexpr std::array<ValueUtilityOperand, 1> kScalarOperand{
    ValueUtilityOperand{.role = bloom::document::kValuePortName, .kind = SocketValueKind::Scalar}};
constexpr std::array<ValueUtilityOperand, 1> kIntegerOperand{
    ValueUtilityOperand{.role = bloom::document::kValuePortName, .kind = SocketValueKind::Integer}};
constexpr std::array<ValueUtilityOperand, 1> kBooleanOperand{
    ValueUtilityOperand{.role = bloom::document::kValuePortName, .kind = SocketValueKind::Boolean}};
constexpr std::array<ValueUtilityOperand, 1> kColorOperand{
    ValueUtilityOperand{.role = bloom::document::kColorPortName, .kind = SocketValueKind::Color}};
constexpr std::array<ValueUtilityOperand, 1> kVector3Operand{ValueUtilityOperand{
    .role = bloom::document::kVectorPortName, .kind = SocketValueKind::Vector3}};

constexpr std::array<ValueUtilityOperand, 3> kBooleanToStringOperands{
    ValueUtilityOperand{.role = bloom::document::kValuePortName, .kind = SocketValueKind::Boolean},
    ValueUtilityOperand{.role = "trueText", .kind = SocketValueKind::String, .text = "true"},
    ValueUtilityOperand{.role = "falseText", .kind = SocketValueKind::String, .text = "false"}};

constexpr std::array<ValueUtilityOperand, 3> kColorToStringOperands{
    ValueUtilityOperand{.role = bloom::document::kColorPortName, .kind = SocketValueKind::Color},
    ValueUtilityOperand{.role = "includeAlpha", .kind = SocketValueKind::Boolean, .flag = false},
    ValueUtilityOperand{.role = "uppercase", .kind = SocketValueKind::Boolean, .flag = false}};

constexpr std::array<ValueUtilityOperand, 2> kVector3ToColorOperands{
    ValueUtilityOperand{.role = bloom::document::kVectorPortName, .kind = SocketValueKind::Vector3},
    ValueUtilityOperand{.role = "alpha", .kind = SocketValueKind::Scalar, .number = 1.0}};

constexpr std::array<ValueUtilityOperand, 2> kVector2ToVector3Operands{
    ValueUtilityOperand{.role = bloom::document::kVectorPortName, .kind = SocketValueKind::Vector2},
    ValueUtilityOperand{.role = "z", .kind = SocketValueKind::Scalar}};

constexpr std::array<ValueUtilityOperand, 3> kVector2ToStringOperands{
    ValueUtilityOperand{.role = bloom::document::kVectorPortName, .kind = SocketValueKind::Vector2},
    ValueUtilityOperand{.role = "separator", .kind = SocketValueKind::String, .text = ", "},
    ValueUtilityOperand{.role = "decimals", .kind = SocketValueKind::Integer, .integer = 2}};

constexpr std::array<ValueUtilityOperand, 3> kVector3ToStringOperands{
    ValueUtilityOperand{.role = bloom::document::kVectorPortName, .kind = SocketValueKind::Vector3},
    ValueUtilityOperand{.role = "separator", .kind = SocketValueKind::String, .text = ", "},
    ValueUtilityOperand{.role = "decimals", .kind = SocketValueKind::Integer, .integer = 2}};

// The table. Ordered as the conversions read: to text, out of text, between the numeric kinds, then
// between the structured kinds.
// ---------------------------------------------------------------------------------------------
// Deliverable 2: time conversions
// ---------------------------------------------------------------------------------------------

constexpr std::array<ValueUtilityOperand, 1> kSecondsOperand{ValueUtilityOperand{
    .role = bloom::document::kSecondsPortName, .kind = SocketValueKind::Scalar}};

constexpr std::array<ValueUtilityOperand, 1> kFramesOperand{ValueUtilityOperand{
    .role = bloom::document::kFramesPortName, .kind = SocketValueKind::Integer}};

constexpr std::array<ValueUtilityOperand, 2> kTimecodeToSecondsOperands{
    ValueUtilityOperand{.role = bloom::document::kTextPortName, .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = bloom::document::kFallbackPortName,
                        .kind = SocketValueKind::Scalar}};

// ---------------------------------------------------------------------------------------------
// Deliverable 3: string utilities
// ---------------------------------------------------------------------------------------------

// The same value/valid pair a parsing node writes. Character At and Split are not parsers, but they
// can be ASKED FOR SOMETHING THAT IS NOT THERE, and answering that with the same shape the safe
// parse contract already defines means a graph has one rule to learn rather than two.
constexpr std::array<ValueUtilityOutput, 2> kParsedString{
    ValueUtilityOutput{bloom::document::kValuePortName, SocketValueKind::String},
    ValueUtilityOutput{bloom::document::kValidPortName, SocketValueKind::Boolean}};

constexpr std::array<ValueUtilitySelector, 1> kStringCaseSelector{ValueUtilitySelector{
    bloom::document::kModeParameterRole, bloom::document::kStringCaseParameterSchemaKey,
    bloom::document::selectorStoredValue(bloom::document::kDefaultStringCaseMode)}};

constexpr std::array<ValueUtilitySelector, 1> kStringPadSelector{ValueUtilitySelector{
    "side", bloom::document::kStringPadSideParameterSchemaKey,
    bloom::document::selectorStoredValue(bloom::document::kDefaultStringPadSide)}};

constexpr std::array<ValueUtilityOperand, 5> kConcatenateOperands{
    ValueUtilityOperand{.role = "a", .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = "b", .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = "c", .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = "d", .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = bloom::document::kSeparatorPortName,
                        .kind = SocketValueKind::String}};

constexpr std::array<ValueUtilityOperand, 5> kFormatOperands{
    ValueUtilityOperand{.role = "pattern", .kind = SocketValueKind::String, .text = "{0} {1}"},
    ValueUtilityOperand{.role = "a", .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = "b", .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = "c", .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = "d", .kind = SocketValueKind::String}};

constexpr std::array<ValueUtilityOperand, 1> kTextOperand{
    ValueUtilityOperand{.role = bloom::document::kTextPortName, .kind = SocketValueKind::String}};

constexpr std::array<ValueUtilityOperand, 3> kSubstringOperands{
    ValueUtilityOperand{.role = bloom::document::kTextPortName, .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = "start", .kind = SocketValueKind::Integer, .integer = 0},
    // Negative means "to the end", which is what makes a freshly added node answer the whole
    // string rather than nothing at all.
    ValueUtilityOperand{.role = "length", .kind = SocketValueKind::Integer, .integer = -1}};

constexpr std::array<ValueUtilityOperand, 3> kCharacterAtOperands{
    ValueUtilityOperand{.role = bloom::document::kTextPortName, .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = bloom::document::kIndexPortName, .kind = SocketValueKind::Integer},
    ValueUtilityOperand{.role = bloom::document::kFallbackPortName,
                        .kind = SocketValueKind::String}};

constexpr std::array<ValueUtilityOperand, 4> kSplitOperands{
    ValueUtilityOperand{.role = bloom::document::kTextPortName, .kind = SocketValueKind::String},
    ValueUtilityOperand{
        .role = bloom::document::kSeparatorPortName, .kind = SocketValueKind::String, .text = ","},
    ValueUtilityOperand{.role = bloom::document::kIndexPortName, .kind = SocketValueKind::Integer},
    ValueUtilityOperand{.role = bloom::document::kFallbackPortName,
                        .kind = SocketValueKind::String}};

constexpr std::array<ValueUtilityOperand, 3> kReplaceOperands{
    ValueUtilityOperand{.role = bloom::document::kTextPortName, .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = bloom::document::kSearchPortName, .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = "replacement", .kind = SocketValueKind::String}};

constexpr std::array<ValueUtilityOperand, 3> kPadOperands{
    ValueUtilityOperand{.role = bloom::document::kTextPortName, .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = "width", .kind = SocketValueKind::Integer, .integer = 0},
    ValueUtilityOperand{.role = "fill", .kind = SocketValueKind::String, .text = " "}};

constexpr std::array<ValueUtilityOperand, 2> kRepeatOperands{
    ValueUtilityOperand{.role = bloom::document::kTextPortName, .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = "count", .kind = SocketValueKind::Integer, .integer = 1}};

constexpr std::array<ValueUtilityOperand, 3> kSearchOperands{
    ValueUtilityOperand{.role = bloom::document::kTextPortName, .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = bloom::document::kSearchPortName, .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = bloom::document::kCaseSensitivePortName,
                        .kind = SocketValueKind::Boolean,
                        .flag = true}};

constexpr std::array<ValueUtilityOperand, 3> kStringEqualsOperands{
    ValueUtilityOperand{.role = "a", .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = "b", .kind = SocketValueKind::String},
    ValueUtilityOperand{.role = bloom::document::kCaseSensitivePortName,
                        .kind = SocketValueKind::Boolean,
                        .flag = true}};

constexpr std::array<ValueUtilityDescriptor, 39> kDescriptors{
    ValueUtilityDescriptor{bloom::document::kScalarToStringNodeType, "Scalar To String",
                           ValueUtilityKernel::ScalarToString, NodeCategory::Utilities,
                           kScalarToStringOperands, kNoSelectors, kStringResult},
    ValueUtilityDescriptor{bloom::document::kIntegerToStringNodeType, "Integer To String",
                           ValueUtilityKernel::IntegerToString, NodeCategory::Utilities,
                           kIntegerToStringOperands, kRadixSelector, kStringResult},
    ValueUtilityDescriptor{bloom::document::kBooleanToStringNodeType, "Boolean To String",
                           ValueUtilityKernel::BooleanToString, NodeCategory::Utilities,
                           kBooleanToStringOperands, kNoSelectors, kStringResult},
    ValueUtilityDescriptor{bloom::document::kColorToStringNodeType, "Color To String",
                           ValueUtilityKernel::ColorToString, NodeCategory::Utilities,
                           kColorToStringOperands, kNoSelectors, kStringResult},
    ValueUtilityDescriptor{bloom::document::kVector2ToStringNodeType, "Vector 2 To String",
                           ValueUtilityKernel::Vector2ToString, NodeCategory::Utilities,
                           kVector2ToStringOperands, kNoSelectors, kStringResult},
    ValueUtilityDescriptor{bloom::document::kVector3ToStringNodeType, "Vector 3 To String",
                           ValueUtilityKernel::Vector3ToString, NodeCategory::Utilities,
                           kVector3ToStringOperands, kNoSelectors, kStringResult},

    ValueUtilityDescriptor{bloom::document::kStringToScalarNodeType, "String To Scalar",
                           ValueUtilityKernel::StringToScalar, NodeCategory::Utilities,
                           kStringToScalarOperands, kNoSelectors, kParsedScalar},
    ValueUtilityDescriptor{bloom::document::kStringToIntegerNodeType, "String To Integer",
                           ValueUtilityKernel::StringToInteger, NodeCategory::Utilities,
                           kStringToIntegerOperands, kRadixSelector, kParsedInteger},
    ValueUtilityDescriptor{bloom::document::kStringToBooleanNodeType, "String To Boolean",
                           ValueUtilityKernel::StringToBoolean, NodeCategory::Utilities,
                           kStringToBooleanOperands, kNoSelectors, kParsedBoolean},
    ValueUtilityDescriptor{bloom::document::kStringToColorNodeType, "String To Color",
                           ValueUtilityKernel::StringToColor, NodeCategory::Utilities,
                           kStringToColorOperands, kNoSelectors, kParsedColor},

    ValueUtilityDescriptor{bloom::document::kScalarToIntegerNodeType, "Scalar To Integer",
                           ValueUtilityKernel::ScalarToInteger, NodeCategory::Utilities,
                           kScalarOperand, kRoundingSelector, kIntegerResult},
    ValueUtilityDescriptor{bloom::document::kIntegerToScalarNodeType, "Integer To Scalar",
                           ValueUtilityKernel::IntegerToScalar, NodeCategory::Utilities,
                           kIntegerOperand, kNoSelectors, kScalarResult},
    ValueUtilityDescriptor{bloom::document::kBooleanToScalarNodeType, "Boolean To Scalar",
                           ValueUtilityKernel::BooleanToScalar, NodeCategory::Utilities,
                           kBooleanOperand, kNoSelectors, kScalarResult},
    ValueUtilityDescriptor{bloom::document::kBooleanToIntegerNodeType, "Boolean To Integer",
                           ValueUtilityKernel::BooleanToInteger, NodeCategory::Utilities,
                           kBooleanOperand, kNoSelectors, kIntegerResult},
    ValueUtilityDescriptor{bloom::document::kScalarToBooleanNodeType, "Scalar To Boolean",
                           ValueUtilityKernel::ScalarToBoolean, NodeCategory::Utilities,
                           kScalarOperand, kNoSelectors, kBooleanResult},
    ValueUtilityDescriptor{bloom::document::kIntegerToBooleanNodeType, "Integer To Boolean",
                           ValueUtilityKernel::IntegerToBoolean, NodeCategory::Utilities,
                           kIntegerOperand, kNoSelectors, kBooleanResult},

    ValueUtilityDescriptor{bloom::document::kColorToVector3NodeType, "Color To Vector 3",
                           ValueUtilityKernel::ColorToVector3, NodeCategory::Utilities,
                           kColorOperand, kNoSelectors, kVector3Result},
    ValueUtilityDescriptor{bloom::document::kVector3ToColorNodeType, "Vector 3 To Color",
                           ValueUtilityKernel::Vector3ToColor, NodeCategory::Utilities,
                           kVector3ToColorOperands, kNoSelectors, kColorResult},
    ValueUtilityDescriptor{bloom::document::kVector2ToVector3NodeType, "Vector 2 To Vector 3",
                           ValueUtilityKernel::Vector2ToVector3, NodeCategory::Utilities,
                           kVector2ToVector3Operands, kNoSelectors, kVector3Result},
    ValueUtilityDescriptor{bloom::document::kVector3ToVector2NodeType, "Vector 3 To Vector 2",
                           ValueUtilityKernel::Vector3ToVector2, NodeCategory::Utilities,
                           kVector3Operand, kNoSelectors, kVector2Result},

    ValueUtilityDescriptor{bloom::document::kSecondsToFramesNodeType, "Seconds To Frames",
                           ValueUtilityKernel::SecondsToFrames, NodeCategory::Utilities,
                           kSecondsOperand, kNoSelectors, kIntegerResult},
    ValueUtilityDescriptor{bloom::document::kFramesToSecondsNodeType, "Frames To Seconds",
                           ValueUtilityKernel::FramesToSeconds, NodeCategory::Utilities,
                           kFramesOperand, kNoSelectors, kScalarResult},
    ValueUtilityDescriptor{bloom::document::kSecondsToTimecodeNodeType, "Seconds To Timecode",
                           ValueUtilityKernel::SecondsToTimecode, NodeCategory::Utilities,
                           kSecondsOperand, kNoSelectors, kStringResult},
    ValueUtilityDescriptor{bloom::document::kTimecodeToSecondsNodeType, "Timecode To Seconds",
                           ValueUtilityKernel::TimecodeToSeconds, NodeCategory::Utilities,
                           kTimecodeToSecondsOperands, kNoSelectors, kParsedScalar},

    ValueUtilityDescriptor{bloom::document::kStringConcatenateNodeType, "Concatenate",
                           ValueUtilityKernel::StringConcatenate, NodeCategory::Utilities,
                           kConcatenateOperands, kNoSelectors, kStringResult},
    ValueUtilityDescriptor{bloom::document::kStringFormatNodeType, "Format",
                           ValueUtilityKernel::StringFormat, NodeCategory::Utilities,
                           kFormatOperands, kNoSelectors, kStringResult},
    ValueUtilityDescriptor{bloom::document::kStringLengthNodeType, "Length",
                           ValueUtilityKernel::StringLength, NodeCategory::Utilities, kTextOperand,
                           kNoSelectors, kIntegerResult},
    ValueUtilityDescriptor{bloom::document::kStringSubstringNodeType, "Substring",
                           ValueUtilityKernel::StringSubstring, NodeCategory::Utilities,
                           kSubstringOperands, kNoSelectors, kStringResult},
    ValueUtilityDescriptor{bloom::document::kStringCharacterAtNodeType, "Character At",
                           ValueUtilityKernel::StringCharacterAt, NodeCategory::Utilities,
                           kCharacterAtOperands, kNoSelectors, kParsedString},
    ValueUtilityDescriptor{bloom::document::kStringSplitNodeType, "Split",
                           ValueUtilityKernel::StringSplit, NodeCategory::Utilities, kSplitOperands,
                           kNoSelectors, kParsedString},
    ValueUtilityDescriptor{bloom::document::kStringReplaceNodeType, "Replace",
                           ValueUtilityKernel::StringReplace, NodeCategory::Utilities,
                           kReplaceOperands, kNoSelectors, kStringResult},
    ValueUtilityDescriptor{bloom::document::kStringTrimNodeType, "Trim",
                           ValueUtilityKernel::StringTrim, NodeCategory::Utilities, kTextOperand,
                           kNoSelectors, kStringResult},
    ValueUtilityDescriptor{bloom::document::kStringCaseNodeType, "Case",
                           ValueUtilityKernel::StringCase, NodeCategory::Utilities, kTextOperand,
                           kStringCaseSelector, kStringResult},
    ValueUtilityDescriptor{bloom::document::kStringPadNodeType, "Pad",
                           ValueUtilityKernel::StringPad, NodeCategory::Utilities, kPadOperands,
                           kStringPadSelector, kStringResult},
    ValueUtilityDescriptor{bloom::document::kStringRepeatNodeType, "Repeat",
                           ValueUtilityKernel::StringRepeat, NodeCategory::Utilities,
                           kRepeatOperands, kNoSelectors, kStringResult},
    ValueUtilityDescriptor{bloom::document::kStringContainsNodeType, "Contains",
                           ValueUtilityKernel::StringContains, NodeCategory::Utilities,
                           kSearchOperands, kNoSelectors, kBooleanResult},
    ValueUtilityDescriptor{bloom::document::kStringStartsWithNodeType, "Starts With",
                           ValueUtilityKernel::StringStartsWith, NodeCategory::Utilities,
                           kSearchOperands, kNoSelectors, kBooleanResult},
    ValueUtilityDescriptor{bloom::document::kStringEndsWithNodeType, "Ends With",
                           ValueUtilityKernel::StringEndsWith, NodeCategory::Utilities,
                           kSearchOperands, kNoSelectors, kBooleanResult},
    ValueUtilityDescriptor{bloom::document::kStringEqualsNodeType, "String Equals",
                           ValueUtilityKernel::StringEquals, NodeCategory::Utilities,
                           kStringEqualsOperands, kNoSelectors, kBooleanResult},
};

} // namespace

namespace bloom::document {

std::span<const ValueUtilityDescriptor> valueUtilityDescriptors() noexcept { return kDescriptors; }

const ValueUtilityDescriptor* findValueUtilityDescriptor(const ValueUtilityKernel kernel) noexcept {
    const auto* const match =
        std::ranges::find(kDescriptors, kernel, &ValueUtilityDescriptor::kernel);
    return match == kDescriptors.end() ? nullptr : match;
}

const ValueUtilityDescriptor* findValueUtilityDescriptor(const std::string_view typeId) noexcept {
    const auto* const match =
        std::ranges::find(kDescriptors, typeId, &ValueUtilityDescriptor::typeId);
    return match == kDescriptors.end() ? nullptr : match;
}

} // namespace bloom::document
