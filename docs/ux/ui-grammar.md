# UI grammar

Status: accepted

Updated: 2026-09-15

Bloom's mechanical interface contract is owned here and implemented by `src/ui/kit`.
[ADR 0021](../decisions/0021-ui-grammar.md) records its rationale and extension procedure.
This contract supersedes conflicting component metrics in [Visual Language](visual-language.md).

## Control vocabulary

Every interactive control is a kit class: `KDropdown`, `KMenuButton`, `KButton`,
`KIconButton`, `KIconToggle`, `KCheckBox`, `KSwitch`, `KValueField`, `KSlider`,
`KSearchField`, `KLineEdit`, `KSection`, or `KColorSwatch`. Text uses TypeRole-bound `KLabel`.
The panel switcher is the icon variant of **KDropdown**, with the same height, styling,
interaction and popup. Menus are created only through `kit::makeMenu`.
Constructing QToolButton, QPushButton, QComboBox, QLineEdit, QCheckBox, QSlider,
QLabel or QMenu outside the kit is a quality violation.

## Metric grid

All dimensions resolve through tokens. One design pixel is one Qt logical pixel.

| Token | Logical pixels | Contract |
| --- | --- | --- |
| `Size::Control` | 26 | All header, footer and row controls |
| `Size::HeaderRow` | 32 | Panel header |
| `Size::FooterRow` | 32 | Optional panel footer |
| `Size::ListRow` | 32 | List and timeline pitch |
| `Size::PropertyRow` | 28 | Property pitch |
| `Size::IconChrome` | 16 | Chrome glyph box |
| `Size::IconControl` | 20 | Control and toggle glyph box |
| `Size::ToggleCell` | 24 | Toggle column pitch |
| `Size::DropdownWidth` | 100 | List dropdown column |
| `Size::DropdownWidthCompact` | 108 | Property dropdown |
| `Size::DropdownWidthWide` | 120 | Header selectors |
| `Size::DropdownWidthExpanded` | 240 | Header selector cap |
| `Spacing::XXS / XS / S / M / L / XL / XXL` | 2 / 4 / 8 / 12 / 16 / 24 / 32 | Gutters |
| `Spacing::ChromePadding / ChromeGap` | 3 / 4 | Equal chrome inset on all edges / every item gap; controls have no private side margins |
| `Spacing::Gutter` | 6 | Panel separation |

Editor literals in fixed extents, QSize, pixel multiplication or geometry arithmetic are
quality violations. Existing specialized canvas tokens remain valid; new dimensions require
an owned semantic token, never a local pixel constant.

## Rasterization and type

Glyphs are SVG rasterizations at integer physical pixel extents for the current DPR, tested
at 1, 1.25, 1.5 and 2. Do not scale an existing pixmap or paint a glyph by hand.
Toggle on uses Fill; off uses Regular, centered in a ToggleCell column.
`TypeRole` is the only font API. The interface family is pinned, bundled Inter
(Regular, Medium, SemiBold; SIL OFL); Geist Mono remains the value face. Intake includes
upstream release identity, digests computed from downloaded bytes, license and manifest.
Render-module text-source DejaVu is independent and unchanged. `QFont(` outside the kit
is a quality violation.

## Declared chrome and rows

Panels return `EditorChromeSpec`: header switcher, selectors, menus and trailing actions;
footer leading controls, transport, readouts and trailing controls. `EditorArea` owns their
layout and one measure-only overflow algorithm, with hysteresis and a stable minimum.
Panels do not construct or paint header/footer widgets. The old take-header/footer and split
provider interfaces are removed. Object names remain stable for interaction and automation.

`KRow` owns list layout: fixed-pitch toggle cells, flexible name, fixed-width dropdown columns
and trailing cell. `KPropertyRow` owns the label, control and reserved diamond columns,
promoting the Properties layout into the kit. Timeline, assets and properties use these
shared row layouts.

Painting outside the kit is limited to viewer canvas, node scene items, and timeline
ruler, lanes, work area and navigator. Other surfaces compose kit widgets.

## Status and tool columns

The existing native window frame and application menu bar remain unchanged. No branding,
workspace tabs or custom title bar are part of this grammar revision.

`WindowStatusBar` composes a single `Control`-height `UiSmall` row: version, colour state,
readiness, dropped frames when measured, cache when populated, and transient/persistent messages.
Colour state is tinted text with no filled badge. It remains visible on every central page,
and reports unavailable colour state when no preview controller exists. Readiness and cache
use muted ink; a colour-state failure remains explicit. `UiSmall` preserves mixed case with
natural tracking. `KSurface` owns neutral surface backgrounds.

`KToolColumn` owns the exclusive vertical arrangement of `KIconToggle` choices. The viewer
exposes Select, Hand and Zoom; Text, Rectangle and Pen remain disabled with honest tooltips.
Hand uses the existing pan mapping; Zoom clicks zoom about the pointer, with Alt-click reversing
the direction. Select retains the command-backed position gesture. Middle-button pan and
existing keyboard zoom remain available. A tool selection is UI state and never authors a node.
Tool glyphs use the same DPR-exact SVG path as other kit controls.

The viewer surrounds its composition with `Color::Canvas` (`#131313`) and uses
`Color::CompositionFrame` (`#454545`) at `kCompositionFrameWidth` (1 logical pixel) for the
frame outline. `ViewerWorkPadding` reserves breathing room around the frame; the left inset
also includes `ToolColumnWidth`. Image fitting, picking, selection overlays and panning all
use this same padded rectangle. View's Background choices remain presentation preferences.

The declared footer order is Channel, RAM Preview, transport, an expanding gap, timecode,
Fit/zoom, and resolution. Background is available through View. The resolution dropdown is
the only visible resolution readout; its tooltip reports the effective Auto factor. The old
background and effective-resolution widget identities remain available to automation.
The time readout defaults to `HH:MM:SS:FF`, honors `timeline/time-format`, accepts either
non-drop timecode or an exact frame index, clamps to the composition, and rejects invalid
fields without changing time. It uses nominal-rate timecode and exact rational frame mapping.

## Node cards

A card uses `NodeTitleBand`, a Title Case derived display name, and a smaller kind eyebrow.
Artist-authored names remain intact. The body uses `PropertyRow` pitch with real kit value
fields, dropdowns, line edits, colour chips and read-only labels hosted in scene proxies.
Parameter sockets sit on the card edge at their corresponding control row; transport-only
ports have separate rows. Vector components share the first component's socket. Connected
parameters retain the existing hidden-editor/driver behavior. Ordered multi-input ports retain
their segmented pill and insertion semantics; reroutes remain dots.

`NodeCardWidth` is the normal floor. Content can require a wider minimum, and an authored
resize remains authoritative above that minimum. Unplaced nodes retain the document's
row/column grouping but use measured card extents plus kit gaps to prevent overlap. Authored
positions are never rearranged, including compact positions stored by existing layer-creation
commands. Artists can rearrange those cards through the existing layout gesture. Links use the socket-kind palette and a horizontal-tangent
cubic spline with `NodeLinkHandleMin`; existing straight/angled preferences remain available.
Every selected card, including the primary selection, uses an Accent outline.

`KDiamond` owns the keyframe indicator's rendering; command dispatch stays in its session
adapter. `KAnchorGrid` owns the nine-point visual grid, while its Properties adapter resolves
bounds off the UI thread. `KListSurface` paints the common alternating empty-row backdrop;
`KRow` owns populated rows and column headings. A heading explicitly identifies its shorter
`Control` pitch through `headerRow`.

## Layout and specialized metrics

The default is five editors in four regions: Viewer and Nodes across the upper left,
Timeline across their combined width, and a full-height right sidebar with Assets over Properties.
These fractions are applied after the complete split tree receives window geometry, so early
minimum-width clamping cannot change the intended proportions.

| Layout token | Value |
| --- | --- |
| `Layout::SidebarShare` | 0.1875 |
| `Layout::TimelineShare` | 0.48 |
| `Layout::NodesShare` | 0.50 |
| `Layout::PropertiesShare` | 0.68 |
| `Layout::WorkspaceVersion` | 2 |

Assets is included in both the default and migrated layouts; editor areas remain replaceable.
Validated version-1 application layouts reset to this complete default. Generic WorkspaceHost
version-1 restoration remains supported. Future schema versions are preserved and not overwritten.
See [Workspace Layout](../architecture/workspace-layout.md) for the migration contract.

| Size token | Pixels | Owner |
| --- | --- | --- |
| `TimelineNameDefault` | 280 | Timeline default name column |
| `TimelineLeftColumn` | 576 | Four 24px toggles + name + two 100px dropdown columns |
| `ViewerWorkPadding` | 48 | Padded image work area |
| `ToolColumnWidth` | 32 | Exclusive tool column |
| `ViewerChannelWidth` | 80 | Channel dropdown |
| `ViewerModeWidth` | 108 | RAM Preview command |
| `ViewerTimecodeWidth` | 104 | Stable time readout |
| `ViewerZoomWidth` | 64 | Fit/zoom dropdown |
| `ViewerResolutionWidth` | 96 | Resolution dropdown |
| `NodeCardWidth / NodeCardMin` | 240 / 128 | Normal card floor / legacy minimum vocabulary |
| `NodeTitleBand` | 32 | Node title and eyebrow |
| `NodeSocketDot / NodeRerouteDot` | 8 / 10 | Scene port geometry |
| `NodeLinkHandleMin` | 32 | Minimum spline tangent |
| `NodeColumnGap / NodeRowGap` | 80 / 24 | Unplaced-node grid |
| `Hairline / SelectionEdge` | 1 / 2 | Pixel-edge arithmetic / scene selection outline |
| `NodeGrid` | 16 | Node snap grid |
| `PlayheadHalfWidth / PlayheadHeight` | 5 / 6 | Timeline playhead head |
| `RulerLabelGap / RulerLabelInset` | 10 / 3 | Timeline ruler labels |
| `MinorTick / MajorTick` | 4 / 8 | Timeline tick extents |
| `ViewerChecker` | 22 | Checkerboard cell |
| `ViewerMinWidth / ViewerMinHeight` | 220 / 176 | Viewer fallback minimum |
| `MultilineHeight` | 78 | Expanded text editors |
| `PropertyLabelCompact` | 64 | Timeline property label |
| `DialogTextWidth / DiagnosticHeight` | 520 / 140 | Read-only explanation / job diagnostics |

`kKeyDiamondRadius` is 4.5 and `kNodeCanvasHalfExtent` is 262144 logical pixels. Ratios,
centering divisors and wheel detents are unitless computations, not pixel dimensions.

## Enforcement and migration

`bloom-quality-check` rejects raw controls (including default construction without parentheses),
forbidden paintEvent, literal dimensions and QFont construction. The allowlist is empty;
adding any entry is itself a hard failure. Balanced extent-call checks detect literal arguments
after nested token calls without consuming unrelated following expressions.

Offscreen fixture-window metric audits run at DPR 1, 1.5 and 2. Whole-window references run
at DPR 1 and 1.5. Clear keyboard focus and deliver Leave before capture. Pixel assertions use
relative brightness. Goldens use a documented small tolerance; drift fails until an explicit
`--update-goldens` commit explains the visual change. References are generated from the
application and never from a design mockup.

The [validation map and capture contract](grammar1-validation.md) identify enforcement, capture generation and the completed migration.

## Automation additions

Existing object names remain stable. New names are `viewerToolColumn`, `viewerSelectTool`,
`viewerHandTool`, `viewerZoomTool`, `viewerTextTool`, `viewerRectangleTool`, `viewerPenTool`,
and `nodeReadOnlyValue`. Node row widgets expose `nodeParameterRole` and
`nodeParameterRowPitch` for geometry audits; list headings expose `headerRow`.

The metric audit covers the status line, six tools, card/control containment and socket-row
alignment at DPR 1, 1.5 and 2. It also verifies the timeline menu set remains expanded at
1600 and 1920 logical-pixel window widths. Whole-window references and final captures run at
DPR 1 and 1.5. Changed geometry tests use the viewer's real padded mapping.

Panel children are clipped by `KSurface::clipPanelChildren` to `Radius::Panel`; corner overlays
provide the antialiased boundary. Header and footer share ChromePadding. Viewer declares
View, Select, Add; Composition commands live under View. Only EditorArea exposes maximize.
New automation names: `viewerAddMenu`, `viewerAddMenuButton`.

Rows own `Spacing::RowPadding` (1 on all edges); KValueField owns `FieldMargin` (1)
inside its allocation, including scene proxies. `PropertyGutter` (8) separates labels and
controls independently of component gaps. KSection owns `SectionPadding` (8 on every edge).
Expanded RGBA rows use a blank-label KPropertyRow so controls align beneath the swatch.

Toggle and disclosure cells are ToggleCell squares (24); their glyphs are IconControl (20),
with Regular off, Fill on, muted disabled and a neutral bordered box. Column headings use
the same glyph size and pitch. KDiamond uses the Bold outline at every DPR.
