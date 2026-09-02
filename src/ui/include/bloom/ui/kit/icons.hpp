#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <span>

namespace bloom::ui::kit {

// Bloom's semantic icon vocabulary. Product code names the meaning, never the upstream file: no
// widget anywhere spells "square-split-horizontal" or ":/bloom/kit/phosphor-icons/...". Backed by
// a curated, pinned Phosphor subset (src/ui/kit/third_party/phosphor-icons/provenance.md).
//
// A few ids deliberately share one upstream glyph -- Minimize and Subtract are both a minus rule,
// and there is no honest visual difference between them. Sharing the asset keeps the vocabulary
// semantic without vendoring a duplicate file.
enum class IconId : std::uint8_t {
    // Window and panel chrome.
    Close,
    Minimize,
    Maximize,
    Restore,
    SplitHorizontal,
    SplitVertical,
    ContextMenu,
    Menu,
    // Disclosure.
    CaretDown,
    CaretUp,
    CaretRight,
    CaretLeft,
    // Transport.
    Play,
    Pause,
    StepBack,
    StepForward,
    Loop,
    // Per-item toggles.
    Visible,
    Hidden,
    AudioOn,
    AudioOff,
    Locked,
    Unlocked,
    // Editing and tools.
    Add,
    Subtract,
    Zoom,
    Pan,
    Grab,
    Select,
    Settings,
    Delete,
    // Status.
    Check,
    Warning,
    Error,
    Info,
    Link,
    // Data kinds.
    Keyframe,
    Folder,
    Sequence,
    Clip,
    Image,
    Audio,
    Composition,
    Text,
};

// Regular is the default interface weight; Fill marks a selected or toggled state. ADR 0010: add
// another upstream weight only when a control shows a concrete legibility need.
enum class IconWeight : std::uint8_t {
    Regular,
    Fill,
};

// Every id, in declaration order. Exists so a test can prove the whole vocabulary renders rather
// than spot-checking the ids someone remembered to list.
[[nodiscard]] std::span<const IconId> iconIds();

// The Qt resource path backing an id at a weight. Public for diagnostics and tests only; product
// code has no reason to call it.
[[nodiscard]] QString iconResourcePath(IconId id, IconWeight weight);

// The tint an icon takes in a given state: hover, pressed, and selected all resolve to Foreground
// because the icon sits on an accent or raised surface then; disabled fades the role's own color
// to kDisabledOpacity. Colour is never the only carrier of a state -- see visual-language.md.
[[nodiscard]] QColor iconTint(Color role, State state);

// A tinted, DPR-aware pixmap. `size` is the icon box in design pixels; the returned pixmap carries
// its device pixel ratio, so it is `size` logical pixels on a side at any ratio.
//
// Results are cached on the full identity ADR 0010 requires: icon id, weight, size, the resolved
// tint (which is exactly where the palette role and the interaction state land), and the device
// pixel ratio. `devicePixelRatio` of 0 means "whatever the application is using".
[[nodiscard]] QPixmap iconPixmap(IconId id, Size size, const QColor& tint,
                                 qreal devicePixelRatio = 0.0,
                                 IconWeight weight = IconWeight::Regular);

// The same, resolving the tint from a palette role and an interaction state.
[[nodiscard]] QPixmap iconPixmap(IconId id, Size size, Color role, State state = State::Normal,
                                 IconWeight weight = IconWeight::Regular,
                                 qreal devicePixelRatio = 0.0);

// A QIcon carrying the Normal, Active (hover), and Disabled renderings of one id, so an ordinary
// Qt control gets the whole state machine for free.
[[nodiscard]] QIcon icon(IconId id, Size size, Color role = Color::Muted,
                         IconWeight weight = IconWeight::Regular);

// Cache introspection, for tests and for a future memory-pressure hook.
[[nodiscard]] std::size_t iconCacheEntryCount();
void clearIconCache();

} // namespace bloom::ui::kit
