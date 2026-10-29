#pragma once

// Project Settings — Colour: the project's OCIO configuration and working colour space. An
// asynchronous kit::KDialog over `owner`; Apply commits one colour-settings change through
// ProjectHost, and a refused change says why in the dialog instead of closing silently.

class QWidget;

namespace bloom::ui {

class ProjectHost;

void showProjectColorSettingsDialog(ProjectHost& host, QWidget& owner);

} // namespace bloom::ui
