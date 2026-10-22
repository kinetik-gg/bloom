#pragma once

#include <QString>
#include <cmath>
#include <limits>

namespace bloom::ui::kit {

// Keep finite extremes legible without making a field hundreds of glyphs wide. The editor always
// receives round-trip precision; only the resting presentation uses the requested decimal places.
[[nodiscard]] inline QString valueFieldNumber(const double value, const int decimals,
                                              const bool compact) {
    auto text = QString::number(value, 'f', decimals);
    if (text.size() > 16 || (value != 0 && std::abs(value) < std::pow(10.0, -decimals)))
        return QString::number(value, 'g', std::numeric_limits<double>::max_digits10);
    if (compact && text.contains('.')) {
        while (text.endsWith('0'))
            text.chop(1);
        if (text.endsWith('.'))
            text.chop(1);
    }
    return text == "-0" ? QStringLiteral("0") : text;
}

} // namespace bloom::ui::kit
