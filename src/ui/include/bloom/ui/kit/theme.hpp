#pragma once

#include <QPalette>
#include <QString>

class QApplication;

namespace bloom::ui::kit {

// The Kinetik theme installer. Owns the whole application-level appearance: the QPalette every
// palette-clean painter reads, the application stylesheet every QSS-hooked surface reads, and the
// interface font. Called once from apps/bloom/main.cpp BEFORE MainWindow is constructed; safe to
// call again at any time (it recomputes and reinstalls rather than accumulating).
//
// It replaces MainWindow::applyFoundationTheme(): a window is not the right owner of the
// application's visual language, and a one-shot literal QSS blob cannot stay in agreement with the
// tokens. Every objectName-keyed rule that lived in that blob is re-expressed here through tokens,
// with the objectNames themselves untouched.
void installKinetikTheme(QApplication& application);

// The palette installKinetikTheme() installs, built entirely from tokens.
[[nodiscard]] QPalette kinetikPalette();

// The application stylesheet installKinetikTheme() installs, generated entirely from tokens: no
// color, radius, spacing, or size literal is spelled anywhere in the sheet's source text.
[[nodiscard]] QString kinetikStyleSheet();

// Expands a Kinetik QSS template: "{color.Accent}" becomes the accent hex, "{space.S}" the spacing
// value in design pixels, and so on for radius, size, and border widths. Exposed so a test can
// prove the expansion is total -- an unresolved placeholder is a defect, not a silent no-op.
[[nodiscard]] QString expandTokens(const QString& templateText);

} // namespace bloom::ui::kit
