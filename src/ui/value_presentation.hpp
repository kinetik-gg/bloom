#pragma once

#include <bloom/document/parameter_presentation.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <cstddef>
#include <string_view>

namespace bloom::ui::kit {
class KSlider;
}

namespace bloom::ui {

// The one place a document parameter presentation becomes a kit field's configuration (lane B).
// Properties, Timeline rows and node cards all configure their numeric fields here, so the same
// parameter has the same range, step, decimals, unit and display scale on every surface; nothing
// else may call setRange/setSingleStep/setDecimals/setUnit on a parameter field. The kit itself
// never sees the document: it takes the displayed-unit kit::ValueFieldPresentation this builds.
[[nodiscard]] kit::ValueFieldPresentation
valueFieldPresentation(const document::ParameterPresentation& presentation);

void configureValueField(kit::KValueField& field,
                         const document::ParameterPresentation& presentation);
void configureValueField(kit::KValueField& field, std::string_view schemaKey,
                         document::ParameterValueKind kind, std::string_view role = {});
// One component of a vector or colour parameter (document::componentPresentation: a colour's
// alpha is confined to [0, 1]).
void configureComponentField(kit::KValueField& field, std::string_view schemaKey,
                             document::ParameterValueKind kind, std::size_t component,
                             std::string_view role = {});

// A slider beside a parameter field spans the presentation's soft range, in displayed units.
void configureSlider(kit::KSlider& slider, const document::ParameterPresentation& presentation);

} // namespace bloom::ui
