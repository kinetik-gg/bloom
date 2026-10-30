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
// Zoom and Search both a magnifying glass -- and there is no honest visual difference between
// them. Sharing the asset keeps the vocabulary semantic without vendoring a duplicate file.
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
    CaretUpDown,
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
    Solo,
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
    Rename,
    // Status.
    Check,
    Warning,
    Error,
    Info,
    Link,
    // Data kinds. Composition is the film strip wherever a composition is meant (an Assets row,
    // New Composition, the nested-composition glyph); an image sequence is the Images stack.
    Keyframe,
    Folder,
    FolderOpen,
    Clip,
    Image,
    Audio,
    Composition,
    Text,
    // Panel identity (task U8, issue 131, formal amendment 1, A5): the panel-switcher glyph for
    // each editor kind. Assets' own glyph is Folder above, reused rather than duplicated.
    Stack,
    Clock,
    SlidersHorizontal,
    Graph,
    Reset,
    Handle,
    Jump,
    AlignLeft,
    AlignCenter,
    AlignRight,
    Rectangle,
    Ellipse,
    Polygon,
    Star,
    Line,
    Pen,
    Snap,
    NewFolder,
    Import,
    DeleteAsset,
    Images,
    // #364: the Viewer's Cryptomatte picker.
    Eyedropper,
    // The Viewer's region-of-interest toggle (a dashed selection frame).
    RegionOfInterest,
    // The Viewer's Transparency Grid toggle (a half-filled square: the pinned set has no checker).
    TransparencyGrid,
    // RAM Preview (a lightning bolt: the pinned set has no memory-chip glyph). Kept apart from
    // Composition, whose film strip it used to share.
    RamPreview,
    // A panel's search toggle. Shares the magnifying glass with Zoom: one glyph, two meanings.
    Search,
    // ADR 0031's context bar: the six layer alignments, the two distributions and its overflow.
    AlignLayersLeft,
    AlignLayersHorizontalCentre,
    AlignLayersRight,
    AlignLayersTop,
    AlignLayersVerticalMiddle,
    AlignLayersBottom,
    DistributeHorizontal,
    DistributeVertical,
    Overflow,
    // Panel switcher glyphs: the Script editor (a terminal window) and the MCP Server (plugs).
    ScriptEditor,
    McpServer,
    // The Export panel (a box with an outgoing arrow).
    ExportPanel,
    // The Timeline's Shy switch and master Shy toggle (a ghost: the pinned set has no After
    // Effects style "peeking" glyph), and its label filter (a funnel).
    Shy,
    LabelFilter,
};

// Regular is the default interface weight; Fill marks a selected or toggled state. ADR 0010: add
// another upstream weight only when a control shows a concrete legibility need.
enum class IconWeight : std::uint8_t {
    Regular,
    Fill,
    Bold,
};

// WHERE an icon appears, and therefore how it is drawn (task VIEW-1). Two roles, no more:
//
//   Chrome  -- panel headers, menus, and per-item toggles. Phosphor Bold at IconMedium (16 px).
//   Control -- the transport and the viewer footer. Phosphor Fill at IconLarge (20 px).
//
// The ROLE owns the weight and the box; a call site names the role and never the pair. That is the
// whole point: iconWeight()/iconSize() below are the single definition of both rows, so changing
// one of them changes every icon of that kind everywhere at once, rather than leaving a scatter of
// call sites that each remembered a number.
//
// A glyph that is one ornament INSIDE another control -- a dropdown's chevron, a menu row's check
// mark, a radio row's tick -- is not a chrome icon in its own right: its box comes from the host
// control's own metrics, so it takes only the role's WEIGHT (iconWeight(IconRole::Chrome)) and
// keeps that box. See docs/ux/visual-language.md, "Iconography".
enum class IconRole : std::uint8_t {
    Chrome,
    Control,
};

[[nodiscard]] constexpr IconWeight iconWeight(const IconRole role) noexcept {
    return role == IconRole::Control ? IconWeight::Fill : IconWeight::Bold;
}

[[nodiscard]] constexpr Size iconSize(const IconRole role) noexcept {
    return role == IconRole::Control ? Size::IconLarge : Size::IconMedium;
}

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

// The role-driven spellings of the two above (task VIEW-1). These are what product code calls: the
// weight and the design-pixel box both come from `iconRole`, so no widget spells either.
[[nodiscard]] QPixmap iconPixmap(IconId id, IconRole iconRole, Color role,
                                 State state = State::Normal, qreal devicePixelRatio = 0.0);

// A QIcon carrying the Normal, Active (hover), and Disabled renderings of one id, so an ordinary
// Qt control gets the whole state machine for free.
[[nodiscard]] QIcon icon(IconId id, Size size, Color role = Color::Muted,
                         IconWeight weight = IconWeight::Regular);

[[nodiscard]] QIcon icon(IconId id, IconRole iconRole, Color role = Color::Muted);

// Cache introspection, for tests and for a future memory-pressure hook.
[[nodiscard]] std::size_t iconCacheEntryCount();
void clearIconCache();

} // namespace bloom::ui::kit
