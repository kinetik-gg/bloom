#include "value_presentation.hpp"

#include <bloom/ui/kit/slider.hpp>

#include <QString>

#include <cmath>
#include <limits>

namespace bloom::ui {
namespace {

// A stored bound in displayed units. The unbounded ends stay unbounded rather than overflowing to
// infinity under a display scale.
[[nodiscard]] double displayed(const double stored, const double scale) {
    if (stored == std::numeric_limits<double>::lowest() ||
        stored == std::numeric_limits<double>::max())
        return stored;
    const double value = stored * scale;
    if (!std::isfinite(value))
        return value > 0 ? std::numeric_limits<double>::max()
                         : std::numeric_limits<double>::lowest();
    return value;
}

} // namespace

kit::ValueFieldPresentation
valueFieldPresentation(const document::ParameterPresentation& presentation) {
    const double scale = presentation.displayScale;
    kit::ValueFieldPresentation field;
    field.integer = presentation.kind == document::ParameterPresentationKind::Integer;
    field.minimum = displayed(presentation.minimum, scale);
    field.maximum = displayed(presentation.maximum, scale);
    field.softMinimum = displayed(presentation.softMinimum, scale);
    field.softMaximum = displayed(presentation.softMaximum, scale);
    field.step = presentation.step;
    field.decimals = presentation.decimals;
    field.unit = QString::fromStdString(presentation.unit);
    field.displayScale = scale;
    return field;
}

void configureValueField(kit::KValueField& field,
                         const document::ParameterPresentation& presentation) {
    field.setPresentation(valueFieldPresentation(presentation));
}

void configureValueField(kit::KValueField& field, const std::string_view schemaKey,
                         const document::ParameterValueKind kind, const std::string_view role) {
    configureValueField(field, document::parameterPresentation(schemaKey, kind, role));
}

void configureComponentField(kit::KValueField& field, const std::string_view schemaKey,
                             const document::ParameterValueKind kind, const std::size_t component,
                             const std::string_view role) {
    configureValueField(field, document::componentPresentation(
                                   document::parameterPresentation(schemaKey, kind, role), kind,
                                   component));
}

void configureSlider(kit::KSlider& slider, const document::ParameterPresentation& presentation) {
    const auto field = valueFieldPresentation(presentation);
    slider.setRange(field.softMinimum, field.softMaximum);
}

} // namespace bloom::ui
