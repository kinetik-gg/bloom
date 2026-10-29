#pragma once

// The application's one typed owner of its global (user-wide, non-project) preferences.
//
// Settings used to be read and written ad hoc at each call site: a panel read a QSettings key in
// its constructor and wrote it back from a menu toggle, with the key string and the default spelled
// again in both places. That is fine while exactly one surface edits a key, and it stops being fine
// the moment a Preferences window is a second writer. This header is the single definition of every
// global key, its default, and its stored spelling, so the window, the panels, and the tests cannot
// disagree about any of them.
//
// Scope: global preferences only. Per-area viewer state (`viewer/analysis/<area>`,
// `viewer/display-view/<revision>/<area>`, `viewer/look/<area>`), per-composition safe-area
// presets, per-project export settings, the legacy `appearance/chrome` key, window geometry,
// the workspace layout, and the user's composition presets (the `compositionPresets` array, owned
// by composition_presets.hpp) are deliberately NOT here.
// Project truth lives in the document; these are session preferences that must never influence
// evaluation semantics (docs/architecture/overview.md, "State Categories").
//
// This is a public bloom_ui header (src/ui/include/bloom/ui), not a private one: the
// PreferencesAware interface in preferences_aware.hpp is a public editor base class and names
// ApplicationPreferences, so any target that includes a public editor header already sees this
// type. Placement does not widen its ownership: it remains the single definition of every global
// key, default, and stored spelling, and no target outside bloom_ui should write global
// preferences directly.

#include <bloom/ui/kit/tokens.hpp>

#include <cstdint>
#include <string>

class QSettings;

namespace bloom::ui {

enum class TimelineTimeFormat : std::uint8_t {
    Frames,
    Timecode,
};

enum class NodeLinkStyle : std::uint8_t {
    Spline,
    Straight,
    Angled,
};

enum class ViewerResolutionPreference : std::uint8_t {
    Auto,
    Full,
    Half,
    Quarter,
};

// The Viewer's surround outside the composition frame (ViewerBackground).
enum class ViewerBackgroundPreference : std::uint8_t {
    Solid,
    Grey,
    Black,
    White,
};

// One value per global key. Byte budgets of 0 mean "use the machine-derived default", exactly as
// the existing readers treat a missing, zero, or unparseable override. An empty
// mediaDiskCacheDirectory means "use the platform cache directory".
struct ApplicationPreferences final {
    // General / playback.
    bool audioEnabled = true;
    bool loopPlayback = true;

    // Memory and caches. Startup-read: a running session does not reconfigure its caches.
    std::uint64_t operationCacheBytes = 0;
    std::uint64_t ramPreviewBytes = 0;
    bool mediaDiskCacheEnabled = true;
    std::string mediaDiskCacheDirectory;
    std::uint64_t mediaDiskCacheBudgetBytes = 0;

    // Timeline.
    TimelineTimeFormat timelineTimeFormat = TimelineTimeFormat::Frames;
    bool timelineSnapping = true;
    bool timelineKeyframesVisible = true;
    bool timelineGraphEditor = false;
    // 0 means "use TimelineEditor's own default width".
    int timelineLayerColumnWidth = 0;

    // Node graph.
    NodeLinkStyle nodeLinkStyle = NodeLinkStyle::Spline;
    bool nodeSnap = false;
    double nodeGridSize = kit::px(kit::Size::NodeGrid);

    // Viewer.
    ViewerResolutionPreference viewerResolution = ViewerResolutionPreference::Auto;
    ViewerBackgroundPreference viewerBackground = ViewerBackgroundPreference::Solid;
    // The checkerboard inside the composition frame. Off by default (After Effects), so the
    // composition's own background colour shows.
    bool viewerTransparencyGrid = false;
    bool viewerSafeAreas = false;
    bool viewerCentreCross = false;
    bool viewerThirds = false;
    bool viewerRulers = false;
    bool viewerPixelGrid = false;

    friend bool operator==(const ApplicationPreferences&, const ApplicationPreferences&) = default;
};

// The defaults above, named so a Reset affordance and a test share one definition.
[[nodiscard]] ApplicationPreferences defaultApplicationPreferences() noexcept;

// Reads every global key, falling back to the default for a missing, unparseable, or out-of-range
// value. A relative media-disk-cache directory reads as empty (the existing
// mediaDiskCacheDirectoryFromSettings() rule), and an unrecognized enum spelling reads as its
// default.
[[nodiscard]] ApplicationPreferences loadApplicationPreferences(const QSettings& settings);

// Writes exactly the keys this model owns, in the spellings the existing readers parse. A 0 byte
// budget is written as 0 (the readers treat it as "default"); a relative media disk cache directory
// is refused and cleared rather than silently persisted.
void saveApplicationPreferences(QSettings& settings, const ApplicationPreferences& preferences);

// Stored spellings, exposed so a test and a UI control can share the exact vocabulary the readers
// already use instead of re-spelling it.
[[nodiscard]] const char* timelineTimeFormatValue(TimelineTimeFormat format) noexcept;
[[nodiscard]] const char* nodeLinkStyleValue(NodeLinkStyle style) noexcept;
[[nodiscard]] const char* viewerResolutionValue(ViewerResolutionPreference policy) noexcept;
[[nodiscard]] const char* viewerBackgroundValue(ViewerBackgroundPreference background) noexcept;

// The stored Viewer surround and Transparency Grid, read with the one migration rule both the
// Preferences model and the Viewer use: the pre-split "Checkerboard" background (which drew the
// checker in the frame) reads as Solid with the grid on, unless a grid value was stored since, so
// an old choice keeps showing a checker and a later grid choice is never overridden. Missing or
// unrecognized values read as Solid with the grid off.
struct ViewerBackdropSetting final {
    ViewerBackgroundPreference background = ViewerBackgroundPreference::Solid;
    bool transparencyGrid = false;
};
[[nodiscard]] ViewerBackdropSetting viewerBackdropFromSettings(const QSettings& settings);
inline constexpr auto kViewerBackgroundSettingKey = "viewer/background";
inline constexpr auto kViewerTransparencyGridSettingKey = "viewer/transparency-grid";

} // namespace bloom::ui
