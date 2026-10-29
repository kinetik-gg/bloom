#pragma once

#include <span>

class QApplication;
class QWidget;

namespace bloom::ui::kit {

// Bloom's type roles always win over platform and theme fonts (visual-language.md, "Type roles").
//
// A platform theme gives Qt an application font and one font per widget class
// (QPlatformTheme::font(), copied into QApplication's class font hash when the application starts
// and again on every QApplication::setStyle()). qt6ct answers every class with its "general" font
// and re-sets the application font once the event loop runs; CoreText and Windows answer item
// views, labels and buttons with their own sizes. Any of these outranks QApplication::setFont() for
// a widget that never named a font, so chrome would render in the desktop's face.

// The class names Qt fills from the platform theme (Qt 6 initializeWidgetFontHash()).
[[nodiscard]] std::span<const char* const> platformFontClasses();

// Re-asserts the type roles: the application font is TypeRole::Ui, every platform class entry
// resolves to its role (Ui; UiSmall for Qt's small/mini control fonts), and a widget that resolved
// a platform face before this pass takes its role font. A widget whose font was set explicitly
// (Qt::WA_SetFont -- a kit role, or a text layer's own face) is never touched. Idempotent, and
// cheap when nothing has drifted.
void applyTypeRoleFonts();

// Gives `widget` its role font when it has no explicit font and resolves a platform family. Called
// from the kit proxy style's polish(), so every widget passes through it once. Roles other than
// Ui (Value, Title, UiSmall) are always named by the widget itself.
void enforceTypeRole(QWidget& widget);

// Re-applies the type roles whenever Qt or the platform theme could have brought platform fonts
// back: an application font change (answered synchronously, before any widget re-resolves), a
// theme change and a style change. Idempotent.
void installTypeRoleEnforcement(QApplication& application);

} // namespace bloom::ui::kit
