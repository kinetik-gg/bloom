#include <bloom/core/blend_mode.hpp>
#include <bloom/core/scalar_primitives.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/parameter_enums.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_operations.hpp>
#include <bloom/document/value_utility_nodes.hpp>

#include <array>
#include <cctype>
#include <cstddef>

namespace bloom::document {
namespace {

using Options = std::vector<ParameterEnumOption>;

std::string_view blendModeLabel(const core::BlendMode mode) noexcept {
    switch (mode) {
    case core::BlendMode::Normal:
        return "Normal";
    case core::BlendMode::Add:
        return "Add";
    case core::BlendMode::Multiply:
        return "Multiply";
    case core::BlendMode::Screen:
        return "Screen";
    case core::BlendMode::Overlay:
        return "Overlay";
    case core::BlendMode::Darken:
        return "Darken";
    case core::BlendMode::Lighten:
        return "Lighten";
    case core::BlendMode::Difference:
        return "Difference";
    }
    return "Normal";
}

// "multiply-add" -> "Multiply Add": the frozen primitive id is the name, in title case.
std::string titleCase(const std::string_view id) {
    std::string label;
    bool start = true;
    for (const char character : id) {
        if (character == '-' || character == '_') {
            label.push_back(' ');
            start = true;
            continue;
        }
        label.push_back(start
                            ? static_cast<char>(std::toupper(static_cast<unsigned char>(character)))
                            : character);
        start = false;
    }
    return label;
}

template <typename Array, typename Stored>
Options named(const Array& members, const std::initializer_list<std::string_view> labels,
              const Stored stored) {
    Options options;
    std::size_t index = 0;
    for (const auto label : labels) {
        if (index >= members.size())
            break;
        options.push_back({std::string(label), stored(members[index++])});
    }
    return options;
}

Options sequential(const std::initializer_list<std::string_view> labels) {
    Options options;
    std::int64_t value = 0;
    for (const auto label : labels)
        options.push_back({std::string(label), value++});
    return options;
}

std::string normalized(const std::string_view text) {
    std::string result;
    for (const char character : text) {
        if (character == ' ' || character == '-' || character == '_')
            continue;
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

} // namespace

std::vector<ParameterEnumOption> parameterEnumOptions(const std::string_view schemaKey) {
    if (schemaKey == kGradientRepeatParameterSchemaKey)
        return sequential({"Clamp", "Repeat", "Mirror"});
    if (schemaKey == "bloom.blur.edge")
        return sequential({"Transparent", "Clamp"});
    if (schemaKey == "bloom.set-alpha.channel" || schemaKey == "bloom.multiply-alpha.channel")
        return sequential({"Alpha", "Red", "Green", "Blue", "Luma", "Alpha Inverted",
                           "Red Inverted", "Green Inverted", "Blue Inverted", "Luma Inverted"});
    if (schemaKey == "bloom.resize-fit.filter")
        return sequential({"Bilinear"});
    if (schemaKey == "bloom.resize-fit.policy")
        return sequential({"Fit", "Fill"});
    if (schemaKey == "bloom.ocio-file.interpolation")
        return sequential({"Linear", "Tetrahedral", "Best"});
    if (schemaKey == "bloom.ocio-file.direction")
        return sequential({"Forward", "Inverse"});
    if (schemaKey == "bloom.shape.kind") {
        Options options;
        for (std::int64_t index = 0; index <= 6; ++index)
            options.push_back({std::string(shapeKindName(static_cast<ShapeKind>(index))), index});
        return options;
    }
    if (schemaKey == "bloom.shape.stroke-align")
        return sequential({"Center", "Inside", "Outside"});
    if (schemaKey == "bloom.shape.stroke-join")
        return sequential({"Miter", "Round", "Bevel"});
    if (schemaKey == "bloom.shape.stroke-cap")
        return sequential({"Butt", "Round", "Square"});
    if (schemaKey == "bloom.shape.fill-rule")
        return sequential({"Nonzero", "Even-odd"});
    if (schemaKey == "bloom.image.loop-mode" || schemaKey == "bloom.video.loop-mode" ||
        schemaKey == "bloom.composition-source.loop-mode")
        return sequential({"Hold", "Loop", "Ping-pong"});
    if (schemaKey == "bloom.image.color-space" || schemaKey == "bloom.video.color-space")
        return sequential({"Auto", "sRGB", "Linear", "Raw"});
    if (schemaKey == kBlendModeParameterSchemaKey) {
        Options options;
        for (const auto mode : core::kBlendModes)
            options.push_back(
                {std::string(blendModeLabel(mode)), core::blendModeStoredValue(mode)});
        return options;
    }
    if (schemaKey == kTextAlignmentParameterSchemaKey)
        return sequential({"Left", "Center", "Right"});
    if (schemaKey == kTextVerticalAlignmentParameterSchemaKey)
        return sequential({"Top", "Middle", "Bottom"});
    if (schemaKey == kTextAnchorModeParameterSchemaKey)
        return sequential({"Left", "Center", "Right"});
    if (schemaKey == kTextOverflowParameterSchemaKey)
        return sequential({"Clip", "Grow"});
    if (schemaKey == "bloom.channel-shuffle.mode")
        return sequential({"Color", "Raw"});
    if (schemaKey == kTextFontParameterSchemaKey)
        return {{"DejaVu Sans", kTextFontDejaVuSans},
                {"Inter Regular", kTextFontInterRegular},
                {"Inter Medium", kTextFontInterMedium},
                {"Inter SemiBold", kTextFontInterSemiBold}};
    if (schemaKey == kScalarOperationParameterSchemaKey) {
        Options options;
        for (const auto operation : kScalarOperations) {
            const auto* signature = core::primitives::scalarPrimitiveSignature(operation);
            const auto id = signature == nullptr ? std::string_view{} : signature->id;
            options.push_back(
                {titleCase(id.substr(id.rfind('.') + 1)), scalarOperationStoredValue(operation)});
        }
        return options;
    }
    if (schemaKey == kVectorOperationParameterSchemaKey)
        return named(
            kVectorOperations,
            {"Add", "Subtract", "Multiply", "Divide", "Scale", "Normalize", "Cross Product"},
            [](const auto value) { return vectorOperationStoredValue(value); });
    if (schemaKey == kVectorReductionParameterSchemaKey)
        return named(kVectorReductions, {"Length", "Dot Product", "Distance"},
                     [](const auto value) { return vectorReductionStoredValue(value); });
    if (schemaKey == kRangeInterpolationParameterSchemaKey)
        return named(kRangeInterpolations, {"Linear", "Smoothstep", "Smootherstep"},
                     [](const auto value) { return rangeInterpolationStoredValue(value); });
    if (schemaKey == kRoundingModeParameterSchemaKey)
        return named(kRoundingModes, {"Round", "Floor", "Ceiling", "Truncate"},
                     [](const auto value) { return selectorStoredValue(value); });
    if (schemaKey == kIntegerOperationParameterSchemaKey)
        return named(kIntegerOperations,
                     {"Add", "Subtract", "Multiply", "Divide", "Modulo", "Minimum", "Maximum"},
                     [](const auto value) { return selectorStoredValue(value); });
    if (schemaKey == kBooleanOperationParameterSchemaKey)
        return named(kBooleanOperations, {"And", "Or", "Xor", "Nand", "Nor"},
                     [](const auto value) { return selectorStoredValue(value); });
    if (schemaKey == kStringCaseParameterSchemaKey)
        return named(kStringCaseModes, {"Upper", "Lower", "Title"},
                     [](const auto value) { return selectorStoredValue(value); });
    if (schemaKey == kStringPadSideParameterSchemaKey)
        return named(kStringPadSides, {"Start", "End"},
                     [](const auto value) { return selectorStoredValue(value); });
    if (schemaKey == kNumberRadixParameterSchemaKey)
        return named(kOfferedRadices, {"Binary", "Octal", "Decimal", "Hexadecimal"},
                     [](const auto value) { return value; });
    if (schemaKey == kCompareOperationParameterSchemaKey)
        return named(kCompareOperations,
                     {"Equal", "Not Equal", "Less", "Less Or Equal", "Greater", "Greater Or Equal"},
                     [](const auto value) { return compareOperationStoredValue(value); });
    return {};
}

std::optional<std::int64_t> parameterEnumValue(const std::string_view schemaKey,
                                               const std::string_view name) {
    const auto wanted = normalized(name);
    if (wanted.empty())
        return std::nullopt;
    for (const auto& option : parameterEnumOptions(schemaKey)) {
        if (normalized(option.label) == wanted)
            return option.value;
    }
    return std::nullopt;
}

} // namespace bloom::document
