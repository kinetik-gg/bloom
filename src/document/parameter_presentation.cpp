#include <bloom/document/parameter_presentation.hpp>

#include <bloom/document/layer_masks.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/value_nodes.hpp>

#include <array>
#include <vector>
#include <string_view>

namespace bloom::document {
namespace {

using Kind = ParameterPresentationKind;

constexpr double kLowest = std::numeric_limits<double>::lowest();
constexpr double kHighest = std::numeric_limits<double>::max();
// The smallest positive double: the hard minimum of a schema that admits any value above zero.
constexpr double kAboveZero = std::numeric_limits<double>::denorm_min();
constexpr std::string_view kPixels = "px";
constexpr std::string_view kPercent = "%";
constexpr std::string_view kDegrees = "\xc2\xb0";
constexpr std::string_view kSeconds = "s";
constexpr std::string_view kFrames = "f";

struct Entry {
    std::string_view schemaKey;
    ParameterPresentation presentation;
};

// A continuous number. Ranges are in stored units; step and decimals in displayed ones.
ParameterPresentation number(const double minimum, const double maximum,
                                       const double softMinimum, const double softMaximum,
                                       const double step, const int decimals,
                                       const std::string_view unit = {},
                                       const double displayScale = 1.0) {
    return {Kind::Number, minimum,  maximum,          softMinimum, softMaximum,
            step,         decimals, std::string(unit), displayScale, true};
}

ParameterPresentation whole(const double minimum, const double maximum,
                                      const double softMinimum, const double softMaximum,
                                      const std::string_view unit = {}) {
    return {Kind::Integer, minimum, maximum, softMinimum, softMaximum, 1.0, 0, std::string(unit),
            1.0,           true};
}

ParameterPresentation choice() {
    ParameterPresentation presentation;
    presentation.kind = Kind::Choice;
    presentation.decimals = 0;
    presentation.named = true;
    return presentation;
}

// A 0..1 factor shown as one: a blend, a mix, a matte multiplier.
ParameterPresentation factor() { return number(0.0, 1.0, 0.0, 1.0, 0.01, 3); }
// Pixels with no upper bound beyond the schema's own.
ParameterPresentation pixels(const double minimum, const double maximum, const double softMaximum) {
    return number(minimum, maximum, minimum < 0.0 ? -softMaximum : minimum, softMaximum, 1.0, 2,
                  kPixels);
}
// A colour channel: straight RGBA, any finite value (HDR and signed colour stay authorable), with
// the 0..1 display range as its comfortable span.
ParameterPresentation channel() { return number(kLowest, kHighest, 0.0, 1.0, 0.01, 3); }

const std::vector<Entry>& table() {
    static const std::vector<Entry> entries{
        // Layer Output transform and compositing.
        {kPositionParameterSchemaKey, pixels(kLowest, kHighest, 4096.0)},
        {kAnchorParameterSchemaKey, pixels(kLowest, kHighest, 4096.0)},
        {kScaleParameterSchemaKey, number(kLowest, kHighest, 0.0, 2.0, 1.0, 2, kPercent, 100.0)},
        {kRotationParameterSchemaKey, number(kLowest, kHighest, -360.0, 360.0, 1.0, 2, kDegrees)},
        {kOpacityParameterSchemaKey, number(0.0, 1.0, 0.0, 1.0, 1.0, 1, kPercent, 100.0)},
        {kTimeRemapParameterSchemaKey, number(kLowest, kHighest, 0.0, 10.0, 0.01, 3, kSeconds)},
        {kBlendModeParameterSchemaKey, choice()},
        // Sources.
        {kSolidColorParameterSchemaKey, channel()},
        {kSolidWidthParameterSchemaKey, pixels(1.0, kHighest, 4096.0)},
        {kSolidHeightParameterSchemaKey, pixels(1.0, kHighest, 4096.0)},
        {kCheckerCellWidthParameterSchemaKey, whole(1.0, 16777216.0, 1.0, 512.0, kPixels)},
        {kCheckerCellHeightParameterSchemaKey, whole(1.0, 16777216.0, 1.0, 512.0, kPixels)},
        {kCheckerWidthParameterSchemaKey, whole(1.0, 16777216.0, 1.0, 4096.0, kPixels)},
        {kCheckerHeightParameterSchemaKey, whole(1.0, 16777216.0, 1.0, 4096.0, kPixels)},
        {kGradientStartParameterSchemaKey, pixels(kLowest, kHighest, 4096.0)},
        {kGradientEndParameterSchemaKey, pixels(kLowest, kHighest, 4096.0)},
        {kGradientStartColorParameterSchemaKey, channel()},
        {kGradientEndColorParameterSchemaKey, channel()},
        {kGradientRepeatParameterSchemaKey, choice()},
        {"bloom.shape.size", pixels(kLowest, kHighest, 4096.0)},
        {"bloom.shape.line-start", pixels(kLowest, kHighest, 4096.0)},
        {"bloom.shape.line-end", pixels(kLowest, kHighest, 4096.0)},
        {"bloom.shape.corner-radius", pixels(0.0, kHighest, 512.0)},
        {"bloom.shape.inner-ratio", factor()},
        {"bloom.shape.stroke-width", pixels(0.0, kHighest, 64.0)},
        {"bloom.shape.points", whole(3.0, 64.0, 3.0, 64.0)},
        {"bloom.shape.fill-color", channel()},
        {"bloom.shape.stroke-color", channel()},
        {"bloom.shape.kind", choice()},
        {"bloom.shape.fill-rule", choice()},
        {"bloom.shape.stroke-align", choice()},
        {"bloom.shape.stroke-cap", choice()},
        {"bloom.shape.stroke-join", choice()},
        // Text.
        {kTextSizeParameterSchemaKey, number(kAboveZero, kMaximumTextSizePixels, 1.0, 400.0, 1.0,
                                             1, kPixels)},
        {kTextLineHeightParameterSchemaKey,
         number(kAboveZero, kHighest, 0.5, 3.0, 1.0, 1, kPercent, 100.0)},
        {kTextLetterSpacingParameterSchemaKey, pixels(kLowest, kHighest, 100.0)},
        {kTextBoxParameterSchemaKey, pixels(kLowest, kHighest, 4096.0)},
        {kTextColorParameterSchemaKey, channel()},
        {kTextAlignmentParameterSchemaKey, choice()},
        {kTextVerticalAlignmentParameterSchemaKey, choice()},
        {kTextAnchorModeParameterSchemaKey, choice()},
        {kTextOverflowParameterSchemaKey, choice()},
        // Media sources.
        {"bloom.image.start-frame", whole(-1000000000.0, 1000000000.0, 0.0, 1000.0, kFrames)},
        {"bloom.video.start-frame", whole(-1000000000.0, 1000000000.0, 0.0, 1000.0, kFrames)},
        {"bloom.audio.start-frame", whole(-1000000000.0, 1000000000.0, 0.0, 1000.0, kFrames)},
        {"bloom.image.loop-mode", choice()},
        {"bloom.video.loop-mode", choice()},
        {"bloom.image.color-space", choice()},
        {"bloom.video.color-space", choice()},
        {kVideoAudioStreamParameterSchemaKey, whole(0.0, 255.0, 0.0, 16.0)},
        {kAudioLevelParameterSchemaKey, number(0.0, 2.0, 0.0, 2.0, 1.0, 1, kPercent, 100.0)},
        {"bloom.composition-source.composition", choice()},
        {"bloom.composition-source.loop-mode", choice()},
        {"bloom.composition-source.time-offset",
         number(kLowest, kHighest, -10.0, 10.0, 0.01, 3, kSeconds)},
        {"bloom.composition-source.time-scale", number(kLowest, kHighest, 0.0, 4.0, 0.01, 3)},
        {"bloom.data.block", choice()},
        // Image effects.
        {"bloom.blur.radius", pixels(0.0, kMaximumBlurRadiusPixels, 64.0)},
        {"bloom.blur.edge", choice()},
        {"bloom.exposure.stops", number(kLowest, kHighest, -4.0, 4.0, 0.1, 2)},
        {"bloom.offset.amount", number(kLowest, kHighest, -1.0, 1.0, 0.01, 3)},
        {"bloom.multiply.factor", number(kLowest, kHighest, 0.0, 2.0, 0.01, 3)},
        {"bloom.image-mix.factor", factor()},
        {"bloom.multiply-alpha.factor", factor()},
        {"bloom.multiply-alpha.channel", choice()},
        {"bloom.set-alpha.channel", choice()},
        {"bloom.crop.left", pixels(0.0, 16777216.0, 1024.0)},
        {"bloom.crop.top", pixels(0.0, 16777216.0, 1024.0)},
        {"bloom.crop.right", pixels(0.0, 16777216.0, 1024.0)},
        {"bloom.crop.bottom", pixels(0.0, 16777216.0, 1024.0)},
        {"bloom.pad.left", pixels(0.0, 16777216.0, 1024.0)},
        {"bloom.pad.top", pixels(0.0, 16777216.0, 1024.0)},
        {"bloom.pad.right", pixels(0.0, 16777216.0, 1024.0)},
        {"bloom.pad.bottom", pixels(0.0, 16777216.0, 1024.0)},
        {"bloom.rotate-90.quarter-turns",
         whole(static_cast<double>(std::numeric_limits<std::int64_t>::min()),
               static_cast<double>(std::numeric_limits<std::int64_t>::max()), -3.0, 3.0)},
        // Masks.
        {kLayerMaskFeatherSchemaKey, pixels(0.0, kMaximumLayerMaskFeatherPixels, 100.0)},
        {kLayerMaskExpansionSchemaKey, pixels(-kMaximumLayerMaskExpansionPixels,
                                              kMaximumLayerMaskExpansionPixels, 100.0)},
    };
    return entries;
}

// Schemas that are selectors in every editor (the value library's operations, the colour
// pipeline's references, the channel nodes' modes). Named so the registry test can tell a choice
// from a forgotten number.
constexpr std::array kChoiceSchemas{
    std::string_view("bloom.boolean.operation"),
    std::string_view("bloom.channel-shuffle.mode"),
    std::string_view("bloom.compare.operation"),
    std::string_view("bloom.convert.rounding-mode"),
    std::string_view("bloom.convert.radix"),
    std::string_view("bloom.integer.operation"),
    std::string_view("bloom.map-range.interpolation"),
    std::string_view("bloom.math.operation"),
    std::string_view("bloom.ocio-file.direction"),
    std::string_view("bloom.ocio-file.interpolation"),
    std::string_view("bloom.ocio-file.lut"),
    std::string_view("bloom.resize-fit.filter"),
    std::string_view("bloom.resize-fit.policy"),
    std::string_view("bloom.string.case"),
    std::string_view("bloom.string.pad-side"),
    std::string_view("bloom.vector-math.operation"),
    std::string_view("bloom.vector-reduce.operation"),
};

ParameterPresentation fallback(const ParameterValueKind kind) {
    switch (kind) {
    case ParameterValueKind::Float64:
    case ParameterValueKind::Vec2d:
    case ParameterValueKind::Vec3d:
        return {Kind::Number, kLowest, kHighest, -100.0, 100.0, 0.1, 3, {}, 1.0, false};
    case ParameterValueKind::Color4d: {
        auto presentation = channel();
        presentation.named = false;
        return presentation;
    }
    case ParameterValueKind::Integer:
        return {Kind::Integer,
                static_cast<double>(std::numeric_limits<std::int64_t>::min()),
                static_cast<double>(std::numeric_limits<std::int64_t>::max()),
                -100.0,
                100.0,
                1.0,
                0,
                {},
                1.0,
                false};
    case ParameterValueKind::Boolean:
    case ParameterValueKind::String:
    case ParameterValueKind::Path:
        break;
    }
    ParameterPresentation other;
    other.kind = Kind::Other;
    return other;
}

} // namespace

ParameterPresentation parameterPresentation(const std::string_view schemaKey,
                                            const ParameterValueKind kind,
                                            const std::string_view role) {
    for (const auto& entry : table())
        if (entry.schemaKey == schemaKey)
            return entry.presentation;
    for (const auto name : kChoiceSchemas)
        if (name == schemaKey)
            return choice();
    if (kind == ParameterValueKind::Boolean || kind == ParameterValueKind::String ||
        kind == ParameterValueKind::Path) {
        auto other = fallback(kind);
        other.named = true;
        return other;
    }
    // The value library: literals and operands carry no meaning beyond their kind, except where
    // the role says what the number is for.
    if (schemaKey == kScalarOperandParameterSchemaKey ||
        schemaKey == kScalarValueParameterSchemaKey ||
        schemaKey == kVector2OperandParameterSchemaKey ||
        schemaKey == kVector2ValueParameterSchemaKey ||
        schemaKey == kVector3OperandParameterSchemaKey ||
        schemaKey == kVector3ValueParameterSchemaKey) {
        auto presentation = fallback(kind);
        presentation.named = true;
        if (role == kMixFactorPortName) {
            presentation.softMinimum = 0.0;
            presentation.softMaximum = 1.0;
            presentation.step = 0.01;
        }
        return presentation;
    }
    if (schemaKey == kIntegerOperandParameterSchemaKey ||
        schemaKey == kIntegerValueParameterSchemaKey) {
        auto presentation = fallback(ParameterValueKind::Integer);
        presentation.named = true;
        return presentation;
    }
    if (schemaKey == kColorOperandParameterSchemaKey || schemaKey == kColorValueParameterSchemaKey)
        return channel();
    if (schemaKey == kCompareEpsilonParameterSchemaKey)
        return number(0.0, kHighest, 0.0, 0.01, 0.0001, 6);
    if (schemaKey == kLayerMaskOpacitySchemaKey)
        return factor();
    if (schemaKey.starts_with("bloom.resize-fit.")) {
        // Width, height and the pixel aspect pair: whole pixels in [1, 32768].
        return whole(1.0, 32768.0, 1.0, 4096.0,
                     schemaKey.ends_with("width") || schemaKey.ends_with("height") ? kPixels
                                                                                   : std::string_view{});
    }
    return fallback(kind);
}

ParameterPresentation parameterPresentation(const ParameterDefinition& definition) {
    return parameterPresentation(definition.schemaKey, definition.valueKind, definition.role);
}

ParameterPresentation componentPresentation(ParameterPresentation presentation,
                                            const ParameterValueKind kind,
                                            const std::size_t component) {
    constexpr std::size_t kAlpha = 3;
    if (kind == ParameterValueKind::Color4d && component == kAlpha) {
        presentation.minimum = 0.0;
        presentation.maximum = 1.0;
        presentation.softMinimum = 0.0;
        presentation.softMaximum = 1.0;
    }
    return presentation;
}

} // namespace bloom::document
