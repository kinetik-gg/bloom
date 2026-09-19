#pragma once

#include <bloom/ui/application_preferences.hpp>

namespace bloom::ui {

// An editor panel that can apply global preferences to its live state when the Preferences window
// commits them.
//
// The Preferences window writes the preferences once; the panels that read those values at
// construction would otherwise keep their old in-memory copy until restart, and the next time an
// artist touched a panel's own menu the stale value would be written back over the new one. This
// interface is how MainWindow hands the committed value to every open panel, so the Settings
// window and the panel menus can never disagree.
//
// Implementations apply using their existing setters and must not mutate the document or add a
// second source of truth: preferences are session state, never project truth.
class PreferencesAware {
  public:
    virtual ~PreferencesAware() = default;
    virtual void applyApplicationPreferences(const ApplicationPreferences& preferences) = 0;
};

} // namespace bloom::ui
