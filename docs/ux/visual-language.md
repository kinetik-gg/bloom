# Visual Language

Status: accepted

Updated: 2026-09-15

## Purpose

Bloom needs a coherent visual foundation for a dense professional interface. Icons and typography
must remain legible at small sizes, work across Linux, macOS, and Windows, and be usable without a
web runtime or JavaScript toolchain.

The [UI grammar](ui-grammar.md) owns control, row and chrome metrics and component measurements. This document owns palette, iconography and interaction recipes. The palette and interaction tables below match `src/ui/include/bloom/ui/kit/tokens.hpp` exactly; the
header is the single implementation of them, and `src/ui/tests/kit_tokens_tests.cpp` asserts each
value listed here.

## Design Tokens

Kinetik is Bloom's visual language. Every color, gap, corner, size, elevation, type role, state,
and duration in the interface comes from the vocabulary below. No Bloom surface spells a raw hex
value, a raw pixel gap, or a raw duration of its own: the tokens are implemented once in
`bloom::ui::kit` (`src/ui/include/bloom/ui/kit/tokens.hpp`), and both the application stylesheet and
every painter resolve through them, so a stylesheet rule and a `QPainter` cannot disagree.

### Unit Rule

One design pixel is exactly one Qt logical pixel. Every number in these tables is in design pixels
at 1x. Physical pixels are derived from the device pixel ratio only inside painting code, never by
scaling a token where it is defined.

The one place that derivation is mandatory is a hairline. A plain one-logical-pixel pen straddles
two physical pixels at 125% and 150% scaling and reads as a blurred grey line, so hairlines are
snapped to a whole number of physical pixels at the device pixel ratio in use
(`kit::snappedHairlineWidth`).

### Color

| Role | Value | Use |
| --- | --- | --- |
| `Canvas` | `#131313` | Viewer work area |
| `CompositionFrame` | `#454545` | Viewer frame outline |
| `Background` | `#111111` | The window, the workspace, and the visible gutters between panels |
| `Surface` | `#141414` | Panel chrome: headers, status bar, toolbars |
| `SurfaceRaised` | `#1B1B1B` | Menus, popups, dialogs, and raised controls |
| `Field` | `#202020` | Input cells: value fields, slider tracks |
| `ControlSurface` | `#0E0E0E` | Header-variant icon buttons and every dropdown closed field -- darker than `Background` itself, by design |
| `Foreground` | `#FFFFFF` | Primary text and active icons |
| `Muted` | `#999999` | Secondary text, resting icons, units |
| `Faint` | `#666666` | Placeholder text, tertiary labels, ruler ticks and separators |
| `Border` | `#222222` | Resting hairlines |
| `BorderHover` | `#454545` | Hovered hairlines, scrollbar thumbs |
| `BorderActive` | `#444444` | The active-panel border only -- a subtle neutral, never `Accent` |
| `Accent` | `#0C8CE9` | Selection, focus, active state, the primary action |
| `AccentHover` | `#3AA5F0` | An accent surface under the pointer |
| `AccentPressed` | `#0A73C2` | An accent surface being pressed |
| `Keyframe` | `#F5C542` | Keyframes and animation markers |
| `Ok` | `#3FBF6B` | Success and ready states |
| `Warn` | `#E6A23C` | Warnings and unsupported states |
| `Error` | `#E0554E` | Failures and destructive actions |
| `Brand` | `#E879AB` | The Bloom logo only, never interface chrome |

Data-type palette, for identifying what a thing *is*:

| Role | Value | Kind |
| --- | --- | --- |
| `DataSequence` | `#E0554E` | Image sequences |
| `DataClip` | `#3FBF6B` | Clips |
| `DataComposition` | `#8B5CF6` | Compositions |
| `DataImage` | `#3AA5F0` | Still images |
| `DataAudio` | `#7C5CFF` | Audio |

`Background`, `Surface`, `SurfaceRaised`, and `Field` form the surface ladder, darkest first. The
state recipes below step along exactly this ladder and nothing else.

### Radius

| Token | Value | Use |
| --- | --- | --- |
| `Small` | `3` | Controls, chips, and a dropdown or menu popup's own frame -- the frame rounds, never the rows inside it |
| `Medium` | `6` | Cards |
| `Panel` | `4` | Panel bodies and their rounded-corner mask -- its own step, not a reuse of `Small` |
| `Large` | `12` | Dialogs |
| `XLarge` | `16` | Full-screen surfaces |
| `Full` | pill | Resolved as half the shape's own extent: switches, scrollbar thumbs, slider handles |

### Border

| Token | Value | Rule |
| --- | --- | --- |
| Hairline | `1` | Snapped to whole physical pixels at any device pixel ratio -- no blur at 125% or 150% |
| Focus | `1`, `Accent` | The control's **own single** hairline turns `Accent`. There is no second outline outside it, and Qt's own focus rectangle is suppressed |
| Window | `1`, `Border` | The application window's own edge |

A control shows exactly one border, and its color is the whole state channel: `Border` at rest,
`BorderHover` under the pointer, `Accent` while active -- focused, being edited, or holding an open
popup. Focused means keyboard focus only: `Qt::TabFocusReason` and `Qt::BacktabFocusReason` turn on
the focus affordance, while pointer-assigned focus leaves the control at its resting or hover state.
**Focus wins over hover**: a control that is both keeps `Accent`, so putting the pointer on the thing
you are editing never takes the focus indication away. A control that is borderless at rest (a
`KValueField` cell) paints its resting border transparent and gains the outline only on hover or
keyboard focus. `kit::borderForInteraction()` is the one implementation;
`src/ui/tests/kit_focus_border_tests.cpp` pins every control at all four points.

The color widgets are the documented exception: `KColorChip` and `KRangeSelector` (through
`kit::drawFocusRing()`) and `KColorSwatches` and `KColorPicker` (through accent pens of their own)
still draw a `1.5` accent ring outside the keyboard-focused element, because their focusable target is a
color field, a swatch, or a handle whose own border color is the artist's data rather than a state
channel -- a border-color change there could not carry focus at all.

### Controls, editor surfaces and layout

The [UI grammar](ui-grammar.md) owns the complete control vocabulary, spacing and size grid,
shared rows, declared headers/footers, viewer tool column, node cards, status line and default
workspace proportions. Implement these through the kit rather than duplicating component
measurements here. The [workspace layout contract](../architecture/workspace-layout.md) owns
persistence and migration. The existing application menu bar and native window frame remain.

Properties remains a command-backed projection: colour values use the existing colour picker,
text uses the supported render face (DejaVu Sans), driven rows navigate upstream, and anchor
placement resolves immutable local bounds asynchronously. UI typography is independent of the
rendered text face. Viewer guides, backgrounds, channel inspection and zoom remain presentation
state; they never change cached composition pixels or export semantics.

### Elevation

| Token | Shadow |
| --- | --- |
| `Flat` | none |
| `Popup` | `0 4 16 rgba(0,0,0,.5)` |
| `Dialog` | `0 12 40 rgba(0,0,0,.6)` |
| `Drag` | `0 18 48 rgba(0,0,0,.55)` |

A raised surface also steps up one level on the surface ladder and keeps its hairline. Elevation is
never the only thing separating a surface from what is behind it.

### Type Roles

| Role | Family | Size | Weight | Use |
| --- | --- | --- | --- | --- |
| `Ui` | Inter | `12` | 500 | The default interface text |
| `UiSmall` | Inter | `10.5` | 500 | Small labels and chrome: mixed case, natural tracking |
| `Value` | Geist Mono | `11.5` | 500 | Every numeric, unit, hex, and timecode surface |
| `Title` | Inter | `13` | 600 | Dialog and section titles |

Sizes are in design pixels. `Value` is monospaced so a column of numbers stays aligned and a
changing digit does not reflow the text beside it.

Pinned static Inter faces provide Medium for Ui/UiSmall and SemiBold for Title; the value
role names Geist Mono Medium. TypeRole selects these families and platform fallbacks.

### State

| State | Recipe |
| --- | --- |
| Hover | One surface step up, plus `BorderHover`. At the top of the ladder the step clamps and the border change carries the state alone |
| Accent-item hover | A full-width `Accent` bar with `Foreground` text -- menu and list rows, never a rounded pill. The row is rectangular and spans the popup frame edge to edge; the frame's own rounded corners clip the bar, so only the frame is ever rounded |
| Menu row | A reserved icon column so text aligns with or without an icon, the shortcut in `Faint`, and the submenu caret as the Phosphor `CaretRight` glyph. Painted by `kit::AltUnderlineProxyStyle`, not by the stylesheet: QSS has no selector for a shortcut column, and any `QMenu` rule with a box makes `QStyleSheetStyle` draw the whole row itself |
| Pressed | `AccentPressed` for an accent surface; one surface step down otherwise |
| Selected | An `Accent` fill, or a 2px inset accent edge where a fill would hide content |
| Focus | The control's own single border turns `Accent`, and stays `Accent` while hovered -- see Border above. Never a second outline |
| Disabled | Ink at 40% opacity, and no hover response at all |

A filled control that is not accent-colored -- a destructive action, for instance -- reproduces the
relation the accent triple already states rather than inventing its own pair of colors: hover
blends toward `Foreground` by the same amount `AccentHover` does, and press scales the channels by
the same factor `AccentPressed` does.

Color is never the only carrier of a state. A disabled control also stops responding; a selected
one also changes its fill or its edge; a toggle also moves its thumb.

### Motion

| Token | Duration | Curve | Use |
| --- | --- | --- | --- |
| `Fast` | `80ms` | ease-out | Hover feedback and toggles |
| `Pop` | `140ms` | ease-out, with a `4` design-pixel rise | Menus and popups appearing |
| `None` | `0ms` | -- | Playhead, scrub, and viewer transforms |

`None` is a rule, not a default. Direct editor feedback is never eased: an eased playhead or an
eased slider handle shows a value that is not the value.

Reduced motion is honored through an explicit kill switch (`BLOOM_REDUCED_MOTION`), because Qt 6.8
exposes no cross-platform reduced-motion style hint. Under it every duration collapses to zero and
every animated control jumps straight to its end state -- a toggle that cannot animate must still
be a toggle.

## Iconography

Phosphor Icons is Bloom's default interface icon family.

- Use the official raw SVG assets. Bloom does not depend on a JavaScript, Node, npm, or web runtime
  to obtain or render icons.
- Vendor a curated set of icons used by the product, pinned to an upstream release or commit. Do
  not use a Git submodule or download interface assets while configuring, building, or launching
  Bloom. The vendored subset, its pinned release, its archive digest, and a digest for every file
  are recorded in `src/ui/kit/third_party/phosphor-icons/provenance.md`.
- Retain the upstream MIT license and record asset provenance beside the vendored files.
- Preserve vendored upstream SVGs unchanged and enumerate them explicitly with `qt_add_resources`
  under a Bloom-owned resource prefix.
- Use `regular` as the default visual weight and `fill` for selected or toggled states.
- Two icon ROLES fix where each weight and size is used, and a call site names the role rather than
  the pair (`kit::IconRole`, `src/ui/include/bloom/ui/kit/icons.hpp`):

  | Role | Where | Weight | Box |
  | --- | --- | --- | --- |
  | `Chrome` | Panel headers, menus, per-item toggles | Phosphor Bold | `IconMedium` (16 px) |
  | `Control` | The transport and the viewer footer | Phosphor Fill | `IconLarge` (20 px) |

  `iconWeight()` and `iconSize()` are the single definition of both rows, so changing one of them
  changes every icon of that kind in the application at once, rather than leaving a scatter of call
  sites that each remembered a number.
- A glyph that is one ornament INSIDE another control -- a dropdown's chevron, a menu row's check
  mark, a radio row's tick, a `KButton`'s own icon -- is not a chrome icon in its own right. Its box
  comes from the host control's metrics, so it takes only the role's weight
  (`iconWeight(IconRole::Chrome)`) and keeps that box.
- Add another upstream weight only when a role needs it. The vendored subset carries `regular`,
  `fill` and `bold` complete over all 48 ids, because a role that asks for a weight must find it for
  every icon: a missing file renders as a silent blank, not an error.
- Access icons through a typed, semantic C++ API such as `IconId::SplitHorizontal`. Product code
  must not spread upstream filenames or resource paths through widgets.
- Render and tint SVGs through Qt, with caching that accounts for icon identity, size, state,
  palette color, and device-pixel ratio. Resolve Phosphor's `currentColor` from the applicable Qt
  palette role; do not assume the raw SVG automatically follows the application palette.
- An icon does not replace an accessible name. Icon-only controls require a tooltip and accessible
  text; destructive or ambiguous actions should include a visible label where space permits.
- Do not communicate an important state through icon color alone.

The complete Phosphor catalog is not embedded in Bloom's main executable by default. If a concrete
workflow later needs the entire catalog, package it as a pinned external Qt `.rcc` resource and load
it through `QResource`. This preserves a native C++/Qt build and avoids making every application
build compile thousands of unused assets.

Official sources:

- [Phosphor core assets](https://github.com/phosphor-icons/core)
- [Phosphor MIT license](https://github.com/phosphor-icons/core/blob/main/LICENSE)
- [Qt resource system](https://doc.qt.io/qt-6/resources.html)

## Typography

Inter is Bloom's primary interface typeface. Geist Mono is Bloom's monospaced
typeface.

Use Inter for:

- menus, editor headers, controls, labels, dialogs, properties, and timeline text
- headings and ordinary artist-facing documentation rendered inside the application
- numeric controls when proportional text is appropriate

Use Geist Mono for:

- code, expressions, scripts, logs, console output, and technical identifiers
- timecode, frame counters, channel values, and aligned numeric readouts where fixed character
  widths materially improve scanning

Do not use the monospaced family as a decorative substitute for hierarchy. Weight, spacing, and
layout should carry hierarchy in the normal interface.

## Font Packaging And Loading

- Vendor native font assets from pinned upstream releases; do not use a Git submodule, install them
  through npm, or fetch them from a network while configuring, building, or launching Bloom.
- Retain each upstream license file and record the exact version or commit.
- Prefer upstream variable TTF assets when they behave consistently through the supported Qt
  version on all three platforms. Keep a tested static-font fallback if variable-font behavior or
  packaging differs.
- Load bundled fonts through Qt's application font facilities. A missing or invalid bundled font
  must produce a diagnostic and fall back to the platform sans-serif or monospace family rather
  than preventing Bloom from opening.
- Let Qt provide glyph fallback for writing systems not covered by the bundled families.
- Use device-independent font sizes and verify the interface at common fractional and high-DPI
  scale factors. Do not rasterize interface text into image assets.
- Platform-owned chrome may retain its operating-system typeface when the platform does not allow
  application font control. Bloom does not introduce platform-specific menu or window chrome solely
  to force typography.

The interface ships Inter Regular, Medium and SemiBold static TTFs; values retain Geist Mono
Regular and Medium. Provenance, computed digests and licenses live in
`src/ui/kit/third_party/inter/` and `src/ui/kit/third_party/geist-mono/`, inventoried in
`THIRD_PARTY_NOTICES.md`. DejaVu Sans Book remains solely for the render text source.


Official sources:

- [Inter](https://rsms.me/inter/)
- [Geist and Geist Mono](https://github.com/vercel/geist-font)

## Ownership Boundary

Icon IDs, font roles, palette roles, and rendering helpers belong to `src/ui`. UI components consume
these semantic roles rather than owning independent asset-loading or font-selection logic.
Optional editor modules use the same host-owned visual roles for standard controls. A module may
provide domain-specific artwork through an explicit registered resource boundary, but it must not
silently replace Bloom's global visual language.

Project documents never store an interface font or icon choice as render-affecting state. Fonts
selected by artists for composition content are project assets and follow a separate media,
licensing, substitution, and missing-dependency workflow.


### Timeline layer row controls

`kit::KRow` owns the layer table: four `ToggleCell` controls, a flexible Name, and two
`DropdownWidth` columns. `KIconToggle` rasterizes SVG at the current DPR, with Regular off and
Fill on. The audio cell is disabled with an explanatory tooltip. A kit disclosure button expands
properties; Parent is a disabled `KDropdown` showing `None`. The grammar owns all dimensions.

| Row control | Object name | State |
| --- | --- | --- |
| Blending | `layerBlendingDropdown` | Enabled. Offers every implemented blend mode, in the one shared order, starting at the layer's own authored mode. Authors the layer the row DRAWS, never the selection |
| Parent | `layerParentDropdown` | Visible, disabled `None` placeholder; tooltip explains that parenting does not exist yet |

A disabled placeholder always states its reason in its tooltip rather than merely looking
unresponsive. The Properties panel's own Blending row (`blendModeEditor`) offers the same vocabulary in
the same order, as does a layer node card's (`nodeBlendModeDropdown`): one set of words, one order,
one write path.

### Timeline property rows and key summaries

Layer, group-heading and parameter rows share the `ListRow` pitch and one vertical
scroll offset. A 16 px Phosphor CaretRight/CaretDown beside the layer name discloses expansion.
Group headings read TRANSFORM, APPEARANCE and SOURCE. Parameter names are indented beneath the
layer name; the 64 px name column, shared diamond, and inline value column stay on the left of the
lane divider. The default layer width is derived from four `ToggleCell`s, `TimelineNameMin` and two
`DropdownWidth` fields. The child-row left inset is derived from the layer control table;
control gaps are `Spacing::XS` (4 px), with `Spacing::XXS` (2 px) inside component cells.

Position, Anchor and Scale have compact X/Y labels and two `KValueField` cells on one row.
Rotation and Opacity use one field; Blending uses `KDropdown` with no animation diamond.
Source color uses `KColorChip` with exact authored RGBA in its tooltip; text Size uses a numeric
field. Scale and Opacity display percentages, Rotation degrees, and Position, Anchor and Size
pixels. Values use the same setters and units as Properties. The left controls are pooled by
viewport and read values at the session time. Group headings carry no value or diamond.

Each parameter's right-hand lane shares the ruler's time mapping and row center. Hold keys are
squares, Linear keys diamonds, and Ease In-Out keys circles, in `Keyframe` gold with Accent
selection. Drag previews are outlined diamonds; box selection uses an Accent outline and a
15 percent Accent fill, and magnetic snapping displays a vertical guide. A collapsed layer
paints the union of its key times as 6 px gold diamonds over its bar. These summary diamonds
have an 8 px hit tolerance and expand the layer; they do not select or move keys.

### Timeline layer kinds

The timeline's clip bar carries its layer's kind as a data-type color. Kind is also named in text by
the row's own tooltip, so the color is a second channel rather than the only one.

| Layer kind | Palette token |
| --- | --- |
| Solid | `DataComposition` |
| Text | `DataClip` |
| Unrecognized | `Muted` |

Neither kind references media, and the data-type palette's five roles all name kinds of referenced
media, so these two are the least-wrong available choices rather than literal matches. The rejections
are on record: `DataImage` (`#3AA5F0`) is byte-identical to `AccentHover` and would make a clip read
as a selected surface while swallowing the 1px `Accent` playhead crossing it; `DataSequence`
(`#E0554E`) is byte-identical to `Error` and would make a clip read as failed; `DataAudio`
(`#7C5CFF`) is a neighbouring purple to `DataComposition` and would not be told apart from a solid.
`DataClip` is byte-identical to `Ok`, which is the remaining collision and the mildest one. Kit-owner
gap: a `DataText` role would remove that collision. A future media-backed or pre-composition layer
kind takes its own role here on the day it ships.

### Node socket kinds

Sockets and the links leaving them identify a *transport* kind, which is a different question from
what an item in a project is, so they have their own palette rather than borrowing the `Data*` roles.
Seven separated hues for eight kinds, none of them `Accent` or `AccentHover`:

| Socket kind | Palette token | Value | Hue |
| --- | --- | --- | --- |
| Image | `SocketImage` | `#2FC8A0` | teal |
| Color | `SocketColor` | `#F2713C` | vermilion |
| Scalar | `SocketScalar` | `#8FD44A` | yellow-green |
| Integer | `SocketInteger` | `#4AC8D4` | cyan |
| Vector2 | `SocketVector` | `#C87AF0` | violet |
| Vector3 | `SocketVector` | `#C87AF0` | violet |
| Boolean | `SocketBoolean` | `#E0567B` | rose |
| String | `SocketString` | `#F0C93C` | gold |

The two vector widths share one token deliberately: they are one family, and giving them adjacent
violets would have said "these connect" when a cross-width link is refused. What distinguishes them is
the socket's own name and tooltip -- which is the rule below, not an exception to it.

The same token inks the socket and every link leaving it. Socket labels must still identify the kind:
color is never the only carrier.

This retires the collision the previous mapping carried -- `DataImage` and `AccentHover` are both
`#3AA5F0`, so an Image socket was indistinguishable from a hovered accent surface. `DataImage` and
`AccentHover` still share that value; nothing in the node editor reads it any more.

While a link drag is in flight every socket states whether the link could land on it: a compatible
socket brightens toward `Foreground` by the filled-hover blend, an incompatible one fades to the
disabled ink, and the socket the drag started from keeps its resting ink because it is the thing in
the artist's hand rather than a target. Releasing ends the drag and restores every resting ink.

### Node editor interaction states

These use design pixels in graph space at 100% zoom. They scale with the canvas transform.

| Surface/state | Rendering or interaction contract |
| --- | --- |
| Port socket | Kind-coloured edge dots aligned to parameter rows; geometry is owned by the [UI grammar](ui-grammar.md#node-cards) |
| Ordered multi-input | Each Merge's ordered image port: a vertical pill in the kind's `Socket*` token, `kStackSlotPitch` long per ordered slot, divided by `Surface` hairlines. A `Muted` caret marks the position under the pointer during a drag, and the pill highlights compatible image drops; the card shows its input count |
| Card eyebrow | Each card carries a `UiSmall`/`Faint` kind label above its display name |
| In-card vocabulary row | A parameter whose value is a closed vocabulary rather than a number takes a Compact `KDropdown` in the card's control column, sized and stretched exactly as a `KValueField` row is. Blending is one example; its value is not animatable and carries no keyframe indicator |
| Socket hover/hit | Hover grows the circle to 12px; its hit radius is 16px (12px beyond the resting 4px radius); tooltip is `<port name> · <kind>` |
| Selected node | 2px inset Accent outline, painted above the card/header surfaces |
| Primary/active node | The same Accent outline; primary identity still belongs to the session selection |
| Muted node | Body and in-node controls at 50% opacity; normal header and existing Phosphor `Hidden`/eye-slash badge, without strikethrough |
| Collapsed node | Header-only `Radius::Full` pill; sockets distributed along the header edges; parameter controls hidden |
| Link | Schema kind ink, widened 12px hit stroke; hover/selected-endpoint emphasis uses brighter ink and 2px stroke |
| Incompatible drag | Error link ink; release publishes nothing. Compatible sockets brighten and incompatible ones dim for the duration of the drag |
| Structural socket/link | Explanatory tooltip and forbidden drag cursor; cut/rewire/insertion unavailable |
| Resize | Right-edge 6px grab zone with horizontal resize cursor; preview is local and release commits width |
| Add search | `KSearchPopup` composes the existing dropdown SurfaceRaised, Border hairline, Small radius, Popup elevation and Accent result states with a Surface filter field. Results are grouped under `UiSmall`/`Faint` section headings that are neither selectable nor choosable; the list is exactly as tall as its rows and headings. A refused result is a disabled row carrying its reason in its tooltip alone -- never in its label. The popup rounds once, at the shared dropdown surface: the list's own mask rounds only the edges it shares with that surface, so the edge beneath the filter field stays square |
| Unavailable command adapter | Cards have an arrow cursor and sockets a forbidden cursor with explanation; application offers only its existing working authoring paths |

`KSearchPopup` object names are `kSearchPopup` and `kSearchFilter`; its reused dropdown subtree
retains `kDropdownPopup`, `kDropdownSurface` and `kDropdownList`. Node interaction additions are
`nodeContextMenu`, `nodeAddSearchAction`, `nodeAddLayerOutputAction`, `nodeAddLayerStackAction`,
`nodeAddCompositionOutputAction`, `nodeSelectAllAction`, `nodeDuplicateAction`, `nodeDissolveAction`,
`nodeMuteAction`, `nodeCollapseAction`, `nodeRenameAction`, `nodeDeleteAction`, `nodeRenameEditor` and
`nodeBlendModeDropdown`.
Existing scene/view/editor, canvas/Add menu, Add Solid/Text, navigation action, color-chip and
position-field object names are unchanged. The legacy named Add actions remain routable contracts
when Add… replaces the visible submenu. The architecture's application-integration limit determines
which authoring affordances can currently be offered.

### Layer Labels And Bars

`Color::Label1..8` is a dedicated display-label palette, independent of socket and media colors:
`#D97C7C`, `#D9A66C`, `#C9C76B`, `#83BD84`, `#68BABA`, `#799ED2`, `#AA8ACC`, `#CE89B4`.
A custom RGB label uses the same row swatch and bar fill. Kind colors remain the default.

Bars span the layer's half-open range, with trim grips and frame snapping. The
[UI grammar](ui-grammar.md) owns row pitch, bar geometry, selection edges and toggle sizes.
`KListSurface` supplies alternating empty rows and `KRow` populated rows; canvas lanes
paint the matching backdrop. All toggle glyphs use the kit SVG path and Phosphor provenance.

### Nested Merge Rows

The timeline draws a direct nested Merge as one collapsed row in `DataComposition`, using the
Merge display name. It has no expansion affordance, solo or lock control; enabled is editable.
The Properties input list shows plain images with disabled Normal blend and 100% opacity fields.
New automation object names: `mergeInputsPanel`, `mergeInputRow`, `mergeInputName`,
`mergeInputBlendMode`, and `mergeInputOpacity`. Existing object names are unchanged.
