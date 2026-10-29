#pragma once

#include <QLocale>
#include <QString>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace bloom::ui::kit {

// A double as the nearest int64 toward zero, saturating at both ends (and NaN at the minimum).
[[nodiscard]] inline std::int64_t valueFieldSaturatingInteger(const double value) {
    if (!(value > static_cast<double>(std::numeric_limits<std::int64_t>::min())))
        return std::numeric_limits<std::int64_t>::min();
    if (!(value < static_cast<double>(std::numeric_limits<std::int64_t>::max())))
        return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(value);
}

// The exact number as text: the shortest decimal that reads back to the very same double, in the
// C locale. The editor is seeded with this and only this, so opening a field never shows the
// floating-point noise a 17-digit rendering does (0.1 stays "0.1").
[[nodiscard]] inline QString valueFieldExactText(const double value) {
    const auto text = QLocale::c().toString(value, 'g', QLocale::FloatingPointShortest);
    return text == QStringLiteral("-0") ? QStringLiteral("0") : text;
}

[[nodiscard]] inline QString valueFieldExactText(const std::int64_t value) {
    return QString::number(static_cast<qlonglong>(value));
}

// The resting presentation at a fixed number of decimal places. Decimals are never trimmed, so the
// decimal points of an X/Y/Z column line up. A value too long for any field (beyond sixteen
// characters) or too small to show at these places falls back to its exact shortest form.
[[nodiscard]] inline QString valueFieldNumber(const double value, const int decimals) {
    auto text = QString::number(value, 'f', decimals);
    if (text.size() > 16 || (value != 0 && std::abs(value) < std::pow(10.0, -decimals)))
        return valueFieldExactText(value);
    // A negative value that rounds to zero at these places reads "0.00", never "-0.00".
    if (text.startsWith(QLatin1Char('-')) &&
        std::all_of(text.begin() + 1, text.end(),
                    [](const QChar c) { return c == QLatin1Char('0') || c == QLatin1Char('.'); }))
        text.remove(0, 1);
    return text;
}

// Scientific notation with `significant` digits (at least one): the narrow-cell fallback.
[[nodiscard]] inline QString valueFieldScientific(const double value, const int significant) {
    auto text = QLocale::c().toString(value, 'e', std::max(0, significant - 1));
    // "1.5e+09" -> "1.5e9": the exponent's sign and leading zeroes are width a narrow cell lacks.
    const auto e = text.indexOf(QLatin1Char('e'));
    if (e >= 0) {
        QString mantissa = text.left(e);
        QString exponent = text.mid(e + 1);
        const bool negative = exponent.startsWith(QLatin1Char('-'));
        if (exponent.startsWith(QLatin1Char('+')) || negative)
            exponent.remove(0, 1);
        while (exponent.size() > 1 && exponent.startsWith(QLatin1Char('0')))
            exponent.remove(0, 1);
        text = mantissa + QLatin1Char('e') + (negative ? QStringLiteral("-") : QString{}) +
               exponent;
    }
    return text;
}

} // namespace bloom::ui::kit
