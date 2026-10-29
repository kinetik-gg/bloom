// The parameter presentation table (lane B): every built-in parameter is named, every hard range
// agrees with the schema's own domain, the soft range sits inside it, and the presentation is
// registry metadata only -- the default value of every built-in parameter is presentable.

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/parameter_presentation.hpp>

#include <cmath>
#include <iostream>
#include <string>
#include <variant>

namespace {
using namespace bloom::document;
int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}

void everyBuiltInParameterIsNamed() {
    for (const auto& definition : builtInNodeDefinitions().definitions()) {
        for (const auto& parameter : definition.parameters) {
            const auto presentation = parameterPresentation(parameter);
            const std::string where = definition.key.typeId + "/" + parameter.role + " (" +
                                      parameter.schemaKey + ")";
            expect(presentation.named, "no named presentation for " + where);
            expect(presentation.minimum <= presentation.softMinimum &&
                       presentation.softMinimum <= presentation.softMaximum &&
                       presentation.softMaximum <= presentation.maximum,
                   "the soft range lies outside the hard range for " + where);
            expect(presentation.step > 0.0 && presentation.decimals >= 0 &&
                       presentation.displayScale != 0.0,
                   "a usable step, decimals and scale for " + where);
            const bool numeric = parameter.valueKind == ParameterValueKind::Float64 ||
                                 parameter.valueKind == ParameterValueKind::Vec2d ||
                                 parameter.valueKind == ParameterValueKind::Vec3d ||
                                 parameter.valueKind == ParameterValueKind::Color4d;
            if (numeric)
                expect(presentation.kind == ParameterPresentationKind::Number,
                       "a continuous parameter presents as a number: " + where);
            if (parameter.valueKind == ParameterValueKind::Integer)
                expect(presentation.kind == ParameterPresentationKind::Integer ||
                           presentation.kind == ParameterPresentationKind::Choice,
                       "an integer parameter presents as a whole number or a choice: " + where);
            // The default must be editable in the field the presentation configures.
            if (const auto* value = std::get_if<double>(&parameter.defaultValue)) {
                expect(*value >= presentation.minimum && *value <= presentation.maximum,
                       "the default lies outside the hard range for " + where);
                expect(isScalarWithinSchemaDomain(parameter.schemaKey, *value),
                       "fixture: the default is inside the schema domain for " + where);
            }
            if (const auto* value = std::get_if<std::int64_t>(&parameter.defaultValue);
                value && presentation.kind == ParameterPresentationKind::Integer)
                expect(static_cast<double>(*value) >= presentation.minimum &&
                           static_cast<double>(*value) <= presentation.maximum,
                       "the integer default lies outside the hard range for " + where);
        }
    }
}

// The hard range is the schema's domain, never a narrower editor cap (issue #322): probing just
// inside and just outside each bounded edge must agree with isScalarWithinSchemaDomain().
void hardRangesMatchTheSchemaDomain() {
    for (const auto& definition : builtInNodeDefinitions().definitions()) {
        for (const auto& parameter : definition.parameters) {
            if (parameter.valueKind != ParameterValueKind::Float64)
                continue;
            const auto presentation = parameterPresentation(parameter);
            const std::string where = parameter.schemaKey;
            // Shape parameters and the compare tolerance are validated by their own schema code
            // (shape.cpp, parameter.cpp) rather than by the scalar gate this probe asks.
            if (where.starts_with("bloom.shape.") || where == kCompareEpsilonParameterSchemaKey)
                continue;
            for (const double edge : {presentation.minimum, presentation.maximum}) {
                if (std::abs(edge) >= std::numeric_limits<double>::max())
                    continue;
                expect(isScalarWithinSchemaDomain(parameter.schemaKey, edge),
                       "the presentation's edge is outside the schema domain for " + where);
            }
            if (presentation.minimum > std::numeric_limits<double>::lowest() &&
                presentation.minimum != std::numeric_limits<double>::denorm_min())
                expect(!isScalarWithinSchemaDomain(
                           parameter.schemaKey,
                           std::nextafter(presentation.minimum,
                                          std::numeric_limits<double>::lowest())),
                       "the schema admits values below the presentation minimum for " + where);
            if (presentation.maximum < std::numeric_limits<double>::max())
                expect(!isScalarWithinSchemaDomain(
                           parameter.schemaKey,
                           std::nextafter(presentation.maximum, std::numeric_limits<double>::max())),
                       "the schema admits values above the presentation maximum for " + where);
        }
    }
}

void knownPresentations() {
    const auto opacity = parameterPresentation(kOpacityParameterSchemaKey, ParameterValueKind::Float64);
    expect(opacity.unit == "%" && opacity.displayScale == 100.0 && opacity.minimum == 0.0 &&
               opacity.maximum == 1.0,
           "opacity is a percentage of a 0..1 factor");
    const auto red = parameterPresentation(kSolidColorParameterSchemaKey, ParameterValueKind::Color4d);
    expect(red.step == 0.01 && red.softMaximum == 1.0,
           "a colour channel scrubs a hundredth per pixel, not a whole unit");
    const auto mix = parameterPresentation("bloom.image-mix.factor", ParameterValueKind::Float64);
    expect(mix.step == 0.01 && mix.maximum == 1.0, "a mix factor scrubs in hundredths");
    const auto frames = parameterPresentation("bloom.image.start-frame", ParameterValueKind::Integer);
    expect(frames.kind == ParameterPresentationKind::Integer && frames.unit == "f",
           "a start frame is a whole number of frames");
    const auto rotation =
        parameterPresentation(kRotationParameterSchemaKey, ParameterValueKind::Float64);
    expect(rotation.unit == "\xc2\xb0" && rotation.maximum == std::numeric_limits<double>::max(),
           "rotation is in degrees and may wind freely");
    const auto alpha = componentPresentation(red, ParameterValueKind::Color4d, 3);
    expect(alpha.minimum == 0.0 && alpha.maximum == 1.0 &&
               componentPresentation(red, ParameterValueKind::Color4d, 0).maximum == red.maximum,
           "a colour's alpha alone is confined to [0, 1]");
    const auto unknown = parameterPresentation("bloom.unregistered", ParameterValueKind::Float64);
    expect(!unknown.named && unknown.kind == ParameterPresentationKind::Number,
           "an unregistered schema falls back by kind, and says so");
}

} // namespace

int main() {
    everyBuiltInParameterIsNamed();
    hardRangesMatchTheSchemaDomain();
    knownPresentations();
    return failures == 0 ? 0 : 1;
}
