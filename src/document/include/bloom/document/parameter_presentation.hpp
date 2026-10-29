#pragma once

#include <bloom/document/node_definition_registry.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace bloom::document {

// How a parameter is presented for editing, by schema: registry metadata, never project truth. It
// is not serialized, takes no part in any schema or registry version, and validation does not read
// it -- isScalarWithinSchemaDomain() and the per-schema validators stay the authority on which
// values are legal, and the hard range below only mirrors them so an editor can refuse a typed
// value before the command would. Every editing surface (Properties, Timeline rows, node cards)
// configures its numeric fields from this one record, so the same parameter scrubs, steps, rounds
// and reads the same everywhere (bloom::ui::configureValueField()).
enum class ParameterPresentationKind : std::uint8_t {
    // A continuous number (Float64, or each component of a Vec2d/Vec3d/Color4d).
    Number,
    // A whole number edited as one, exactly (int64).
    Integer,
    // A closed vocabulary or a reference the editor offers as a list, stored as an integer.
    Choice,
    // Anything else: strings, booleans, paths, unregistered schemas.
    Other,
};

struct ParameterPresentation {
    ParameterPresentationKind kind = ParameterPresentationKind::Number;
    // The hard range, in STORED units: what the schema admits.
    double minimum = std::numeric_limits<double>::lowest();
    double maximum = std::numeric_limits<double>::max();
    // The soft range, in stored units: the span a slider covers and the range a scrub is
    // comfortable in. Always inside the hard range.
    double softMinimum = 0.0;
    double softMaximum = 1.0;
    // One scrub pixel or one arrow press, in DISPLAYED units.
    double step = 1.0;
    int decimals = 2;
    // The displayed unit (UTF-8), and the factor from the stored to the displayed number: an
    // opacity stored in [0, 1] and shown in percent has displayScale 100 and unit "%".
    std::string unit;
    double displayScale = 1.0;
    // True when the table names this schema; false for the generic fallback by value kind. The
    // registry test requires every built-in parameter to be named.
    bool named = false;

    friend bool operator==(const ParameterPresentation&, const ParameterPresentation&) = default;
};

// The presentation of one schema key. `kind` is the parameter's declared value kind, used for the
// fallback of an unnamed schema; `role` distinguishes the value library's shared operand schemas
// where a role carries meaning (a Mix factor is a 0..1 blend, a Clamp bound is any number).
[[nodiscard]] ParameterPresentation parameterPresentation(std::string_view schemaKey,
                                                          ParameterValueKind kind,
                                                          std::string_view role = {});
[[nodiscard]] ParameterPresentation parameterPresentation(const ParameterDefinition& definition);

// One component's presentation: the parameter's own, except that a colour's alpha (component 3 of
// a Color4d) is confined to [0, 1], which every straight RGBA value requires
// (core::Color4d::isValid()).
[[nodiscard]] ParameterPresentation componentPresentation(ParameterPresentation presentation,
                                                          ParameterValueKind kind,
                                                          std::size_t component);

} // namespace bloom::document
