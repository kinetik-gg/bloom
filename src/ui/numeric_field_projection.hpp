#pragma once

#include <bloom/ui/kit/value_field.hpp>
#include <cmath>
#include <limits>

namespace bloom::ui {

// Percentage presentation can overflow even when the authored factor is finite. In that case show
// the factor in its native unit; never leave a stale or clamped field standing in for document
// truth.
inline void projectNumericField(kit::KValueField& field, const double value, const double scale,
                                const QString& unit, const QString& nativeUnit) {
    // The value, unit and inverse factor are one projection. A live gesture owns all three until
    // its commit/cancel signal publishes session truth again after clearing hasLiveEdit().
    if (field.hasLiveEdit())
        return;
    const auto magnitude = std::abs(scale);
    bool scaled =
        scale != 0 && std::isfinite(scale) &&
        (magnitude <= 1 || std::abs(value) <= std::numeric_limits<double>::max() / magnitude);
    double displayed = scaled ? value * scale : value;
    if (!std::isfinite(displayed) || (displayed == 0 && value != 0)) {
        scaled = false;
        displayed = value;
    }
    field.setDisplayScale(scaled ? scale : 1.0);
    field.setUnit(scaled ? unit : nativeUnit);
    field.setValue(displayed);
}

// The stored value behind a field's shown number: the field's own display scale divides it back.
[[nodiscard]] inline double numericFieldValue(const kit::KValueField& field) {
    return field.storedValue();
}

} // namespace bloom::ui
