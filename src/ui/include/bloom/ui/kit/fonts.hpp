#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QLoggingCategory>
#include <QString>
#include <QStringList>

namespace bloom::ui::kit {

// Diagnostics channel for bundled-font loading. A missing or invalid bundled face is reported
// here and then survived, never crashed on.
Q_DECLARE_LOGGING_CATEGORY(kitFontsLog)

struct BundledFontStatus {
    // Whether each bundled family actually registered. False means the interface is running on the
    // platform's own sans-serif or monospace family -- degraded, reported, and still usable.
    bool interfaceRegistered = false;
    bool monospaceRegistered = false;

    // The distinct family names Qt registered, in load order. Qt reports a face's own family
    // name and, where declared, its typographic family; duplicates across faces are collapsed.
    QStringList registeredFamilies;

    // One human-readable line per face that failed to load, empty when everything registered.
    QStringList diagnostics;
};

// Registers the bundled TTFs with Qt's application font database. Idempotent: the first call does
// the work, every later call returns the same result without re-registering.
const BundledFontStatus& registerBundledFonts();

// The current registration result, registering on first use.
const BundledFontStatus& bundledFontStatus();

// The family list a type role should ask for, best match first, ending in the platform fallback.
//
// Static TTFs name their heavier weights as separate families -- the Medium face registers as
// "Plus Jakarta Sans Medium", not as "Plus Jakarta Sans" at weight 500 -- so a role that wants a
// non-Regular weight has to name that face explicitly or silently get Regular. Listing the exact
// face first and the base family second gets the right glyphs whether or not Qt's font engine
// merges the typographic family on a given platform.
[[nodiscard]] QStringList fontFamiliesForRole(TypeRole role);

} // namespace bloom::ui::kit
