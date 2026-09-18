# UI grammar

Status: accepted

Updated: 2026-09-18

Bloom's mechanical interface contract is owned here and implemented by `src/ui/kit`.
[ADR 0021](../decisions/0021-ui-grammar.md) records its rationale and extension procedure.
This contract supersedes conflicting component metrics in [Visual Language](visual-language.md).

## Geometry and transform levels

- **NATIVE SIZE** is the source geometry in its own units: rectangle width/height, text box,
  font size, line endpoints and path anchors. Stroke width, corner radius and points are authored
  in these units; changing native dimensions does not stretch them.
- **LOCAL TRANSFORM** is the layer's Position/Anchor/Scale/Rotation in parent space. Scale is an
  animatable percentage that stretches the finished shape, including its strokes. Vector Scale
  is edited only through Properties or timeline fields, never through handles.
- **WORLD TRANSFORM** is LOCAL TRANSFORM composed with every ancestor. It is derived, never
  authored. The gizmo works in this space and maps pointer motion back through the parent.

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
at 1, 1.25, 1.5 and 2. Do not scale an existing pixmap or paint a glyph by hand. KDiamond is the explicit vector
primitive exception described below; it resolves geometry through the device transform.
Toggle on uses Fill; off uses Regular, centered in a ToggleCell column.
`TypeRole` is the only interface font API. The interface family is pinned, bundled Inter
(Regular, Medium, SemiBold; SIL OFL); Geist Mono remains the value face. Intake includes
upstream release identity, digests computed from downloaded bytes, license and manifest.
Render-module text-source rendering embeds the same DejaVu Sans and Inter face files for deterministic
CPU evaluation; it is independent of Qt's font database. Artist text fonts are selected through a
searchable `KDropdown`: embedded faces appear first, then the asynchronously enumerated system
catalogue. The selected row creates or reuses a Font asset and stores only its stable reference.
`QFont(` outside the kit is a quality violation.

## Declared chrome and rows

Panels return `EditorChromeSpec`: header switcher, selectors, menus and trailing actions;
footer leading controls, transport, readouts and trailing controls. `EditorArea` owns their
layout and one measure-only overflow algorithm, with hysteresis and a stable minimum.
Panels do not construct or paint header/footer widgets. The old take-header/footer and split
provider interfaces are removed. Object names remain stable for interaction and automation.
Timeline declares a split `footerCanvas`, composed with `EditorArea::buildSplitChrome` and kit
controls, which EditorArea hosts as the panel footer. Its left cell follows the layer-table
divider and contains Split at Playhead and Delete Layer actions, enabled for the current selection.
Both footer cells use the same EditorChromeRowSpec/buildChromeRow path as other panels. The
right cell contains only the shared horizontal zoom slider and, when zoomed in, a draggable
horizontal range scrollbar. There are no plus/minus buttons.
Zoom to Fit remains in View; no Fit button occupies the footer.
The slider maps the full composition through a logarithmic range down to individual frames.

`KRow` owns list layout: fixed-pitch toggle cells, flexible name, fixed-width dropdown columns
and trailing cell. `KPropertyRow` owns the label, control and reserved diamond columns,
promoting the Properties layout into the kit. Timeline, assets and properties use these
shared row layouts.

Painting outside the kit is limited to viewer canvas, node scene items, and timeline
ruler, lanes, graph editor, work area and navigator. Other surfaces compose kit widgets.

## Status and tool columns

The existing native window frame and application menu bar remain unchanged. No branding,
workspace tabs or custom title bar are part of this grammar revision.

`WindowStatusBar` composes a single `Control`-height `UiSmall` row: version, colour state,
readiness, dropped frames when measured, cache when populated, and transient/persistent messages.
The cache cell combines RAM-preview frames with operation-cache hits, misses, retained bytes and
effective budgets; its tooltip names those two cache accounts when the compact cell is dense, and
states the rule the budgets came from: Bloom reserves `max(8 GiB, 40%)` of memory for the rest of
the machine and its default caches never exceed half of physical memory. The tooltip explains where
a number came from; it never becomes a second place to read the numbers themselves.
Colour state is tinted text with no filled badge. It remains visible on every central page,
and reports unavailable colour state when no preview controller exists. Readiness and cache
use muted ink; a colour-state failure remains explicit. `UiSmall` preserves mixed case with
natural tracking. `KSurface` owns neutral surface backgrounds.

`KToolColumn` owns ten enabled, exclusive `KIconToggle` choices. Select (V), Hand (H), Zoom (Z),
Text (T), Rectangle (R), Ellipse (E), Polygon, Star, Line and Pen (P) are viewer-local tools.
Hand pans; Zoom clicks zoom about the pointer, with Alt-click reversing the direction. Middle-button
pan and keyboard zoom remain available. Escape cancels unfinished creation and returns to Select;
after creation it also returns to Select. Choosing a tool alone never authors a node.

Rectangle, Ellipse, Polygon, Star and Line draw a live Accent outline on press-drag. Shift constrains
rectangles and ellipses to equal sides, keeps polygons and stars regular, or snaps a line to 45-degree steps; Alt draws from the press point
as centre. Polygon and Star start with five points. A click creates a 100 × 100 composition-pixel
shape, or a 100-pixel horizontal line, centred on the click. Release creates and selects the layer
with one undo entry. The selection gizmo appears with the evaluated frame.

Pen clicks append straight anchors; dragging an anchor during creation sets symmetric cubic
handles. Clicking the first anchor closes a path with at least three anchors; Enter finishes an
open path with at least two. Escape discards the draft. Selected Path layers display screen-sized
anchor squares and handle circles. Dragging edits the live path, Alt moves a handle independently,
and Delete removes the selected anchor. Text clicks create a layer and enter canvas text editing
with the placeholder selected. Properties mirrors the live text.

All glyphs use the DPR-exact SVG renderer and kit state tints. Rectangle, Pen and Text retain their
Phosphor assets; the four added shape glyphs are Bloom-authored SVG paths.

| Tool icon | Glyph | Shortcut |
| --- | --- | --- |
| Select | cursor | V |
| Hand | hand | H |
| Zoom | magnifying-glass | Z |
| Rectangle | rectangle | R |
| Ellipse | ellipse | E |
| Polygon | pentagon | Column only |
| Star | five-point star | Column only |
| Line | diagonal line | Column only |
| Pen | pen-nib | P |
| Text | text-t | T |

The viewer surrounds its composition with `Color::Canvas` (`#131313`) and uses
`Color::CompositionFrame` (`#454545`) at `kCompositionFrameWidth` (1 logical pixel) for the
frame outline. `ViewerWorkPadding` reserves breathing room around the frame; the left inset
also includes `ToolColumnWidth`. Image fitting, picking, selection overlays and panning all
use this same padded rectangle. View's Background choices remain presentation preferences.

The declared footer order is Channel, ROI toggle, Clear ROI, exposure (EV), gamma, Display / View,
Look, RAM Preview, transport, an expanding gap, timecode, Fit/zoom, and resolution. Background is available through
View. The resolution dropdown is
the only visible resolution readout; its tooltip reports the effective Auto factor. The old
background and effective-resolution widget identities remain available to automation.
The time readout defaults to `HH:MM:SS:FF`, honors `timeline/time-format`, accepts either
non-drop timecode or an exact frame index, clamps to the composition, and rejects invalid
fields without changing time. It uses nominal-rate timecode and exact rational frame mapping.

With Select active, Ctrl-drag defines a composition-space ROI without selecting or moving a layer.
The ROI toggle suspends and restores the rectangle; Clear ROI forgets it. The viewer dims outside
it and draws an Accent hairline. Escape, capture loss, mapping changes, and session changes cancel
an unfinished drag; switching compositions clears the rectangle. These bindings use the same Qt
input and mapping path on Linux, macOS, and Windows.

Exposure and gamma use compact `KValueField` controls named `viewerExposure` and `viewerGamma`,
with `ViewerZoomWidth` and `Control` metrics. Exposure is measured in EV stops; gamma defaults to
one. They affect only this viewer and persist under `viewer/analysis/<area-id>/exposure` and
`gamma` in QSettings. The unhosted test/standalone viewer uses the `default` area key. ROI controls
are named `viewerRoiToggle` and `viewerRoiClear`. Pending display work and failures appear in the
exposure tooltip and task monitor; superseding controls or closing the viewer cancels its work.

`ViewerEditor::probeChanged(ProbeReadout)` feeds the status bar's `windowStatusBarProbe` cell through
the preview controller. The cell follows `makeCell`, using UiSmall typography and Muted ink. It
reports `W: r g b a · D: r g b`, where W is un-premultiplied working-space RGBA and D is either the
painted RGBA8 or the float display-stage value before encoding. Hover identifies the working-space
id and selected display/view, and Ctrl-click cycles the D format. It reads the display buffer before
channel remapping or background painting. The readout can show Sampling or a diagnostic while its
worker runs. A narrow status bar elides the line while preserving the complete tooltip and
accessible name. Leaving the viewer clears the cell immediately, including when an earlier probe is
still completing.

Viewer selection bounds use Accent cosmetic hairlines snapped to device-pixel centres. Eight
`GizmoHandle` squares use Surface fill and Accent outlines; the anchor uses a crosshair ring.
`GizmoRotateZone` is the outside-corner hit radius. These dimensions remain in logical screen
pixels as zoom, pan, proxy and pixel aspect change. The same ViewerMapping drives overlays and
hit-testing. Selection geometry never enters process pixels or export.

## Node cards

A card uses one `NodeTitleBand` row: its Title Case derived display name is left-aligned in Ui,
and its category is right-aligned in muted UiSmall at the same vertical center.
Artist-authored names remain intact. The body uses `PropertyRow` pitch with real kit value
fields, dropdowns, line edits, colour chips and read-only labels hosted in scene proxies.
Parameter sockets sit on the card edge at their corresponding control row; transport-only
ports have separate rows. Vector components share the first component's socket. Connected
parameters retain the existing hidden-editor/driver behavior. Ordered multi-input ports retain
their segmented pill and insertion semantics; reroutes remain dots.

`NodeCardWidth` is the normal floor. Content can require a wider minimum, and an authored
resize remains authoritative above that minimum. `Arrange All` lays out the graph left to right:
longest-path ranks place sources and value drivers before their consumers, and stable barycenter
sweeps order each rank to reduce crossings. Each rank uses its maximum measured card width plus one
`Spacing::L` gap; cards in a rank use their measured heights plus the same gap, and shorter ranks
are vertically centred. Disconnected components stack below one another. Nodes in a group remain a
contiguous rank block, and the frame is recomputed from its members after the transaction.
`Arrange Selection` applies the same rule to the induced selection and preserves its bounding-box
top-left. Both commands are one undoable `MoveNodes` transaction; Arrange All fits the canvas after
publication.

Authored positions are never rearranged by creation. A new card keeps its requested free position,
or searches the nearest rank-grid slot around its connection (down, up, then the next column) using
an indexed collision test against every card and a `Spacing::L` clearance. Structured layer
creation puts the Layer card one gap left of Merge and its source one gap left of Layer. This rule
also applies to Assets drops and viewer/timeline layer actions, so existing cards never move and a
new card never touches another card. Links use the socket-kind palette and a horizontal-tangent
cubic spline with `NodeLinkHandleMin`; existing straight/angled preferences remain available.
Every selected card, including the primary selection, uses an Accent outline.


Keyframe diamonds have four forms: a muted outline for a constant, a gold outline for an animated
parameter with no key at the playhead, a gold half fill when some components are keyed there, and
a gold fill when all components are keyed there. A component diamond uses the same forms except
half fill, and toggles only that component. Parameter diamonds toggle all components together.

Properties and node cards retain aggregate parameter diamonds and place component diamonds after
each vector field or RGBA channel. Timeline rows put component diamonds before numeric fields;
the parameter disclosure expands individual X/Y/Z or R/G/B/A rows. Automation names are
`propertiesComponentDiamond`, `nodeComponentKeyframeDiamond`, and `timelineComponentDiamond`.
The timeline and Properties Parent dropdowns list None and eligible layer names, submit the shared
parent command, and restore the authored choice on refusal.

`KDiamond` owns the keyframe indicator's rendering; command dispatch stays in its session
adapter. `KAnchorGrid` owns the nine-point visual grid, while its Properties adapter resolves
bounds off the UI thread. `KListSurface` paints the common flat empty-row backdrop;
`KRow` owns populated rows and column headings. A heading explicitly identifies its shorter
`Control` pitch through `headerRow`.

## Layout and specialized metrics

The default is five editors in two rows. The full-width top row is Assets, Viewer, Nodes,
Properties at 16% / 31% / 32% / 19%; the full-width bottom row is Timeline. The rows are 68% / 32%
of the content height. These weights are authored in `WorkspaceHost` as per-mille values and are
applied after the complete split tree receives window geometry, so early minimum-size clamping
cannot change the intended proportions.

| Layout token | Value |
| --- | --- |
| `WorkspaceHost` top-row weights | 160 / 310 / 320 / 190 |
| `WorkspaceHost` row weights | 680 / 320 |
| `TimelineEditor` layer-table share | 0.37 of the Timeline width |
| `Layout::WorkspaceVersion` | 2 |

Assets is included in both the default and migrated layouts; editor areas remain replaceable.
Validated version-1 application layouts reset to this complete default. Generic WorkspaceHost
version-1 and earlier version-2 trees remain restorable. `Window → Reset Workspace` restores these
weights, resets the Timeline divider to 37%, and persists both results. Future schema versions are
preserved and not overwritten.
See [Workspace Layout](../architecture/workspace-layout.md) for the migration contract.

| Size token | Pixels | Owner |
| --- | --- | --- |
| `TimelineNameDefault` | 280 | Timeline default name column |
| `TimelineLeftColumn` | 576 | Four 24px toggles + name + two 100px dropdown columns |
| `GizmoHandle / GizmoRotateZone` | 8 / 16 | Viewer transform handles / outside-corner rotation radius |
| `ViewerWorkPadding` | 48 | Padded image work area |
| `ToolColumnWidth` | 32 | Exclusive tool column |
| `ViewerChannelWidth` | 80 | Channel dropdown |
| `ViewerModeWidth` | 108 | RAM Preview command |
| `ViewerTimecodeWidth` | 104 | Stable time readout |
| `ViewerZoomWidth` | 64 | Fit/zoom dropdown |
| `ViewerResolutionWidth` | 96 | Resolution dropdown |
| `NodeCardWidth / NodeCardMin` | 240 / 128 | Normal card floor / legacy minimum vocabulary |
| `NodeTitleBand` | 32 | Node name and category on one row |
| `NodeSocketDot / NodeRerouteDot` | 8 / 10 | Scene port geometry |
| `NodeLinkHandleMin` | 32 | Minimum spline tangent |
| `NodeColumnGap / NodeRowGap` | 80 / 24 | Legacy unplaced-node grid |
| `NodeArrangeGap` | `Spacing::L` (16) | Rank, component and creation clearance |
| `GraphValueAxis / GraphHandleDot` | 48 / 6 | Graph editor value gutter / ease-handle dot |
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
| `SplitHandle` | 6 | Draggable divider hit zone (`KSplitHandle`); paints a `TimelineSeparator`-width hairline centered in it |

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
`viewerHandTool`, `viewerZoomTool`, `viewerTextTool`, `viewerRectangleTool`, `viewerEllipseTool`,
`viewerPolygonTool`, `viewerStarTool`, `viewerLineTool`, `viewerPenTool`,
`propertiesFilterStrip`, `propertiesFilterAll`, `propertiesFilterObject`,
`propertiesFilterTransform`, `propertiesFilterSource`, `propertiesFilterGraph`,
and `nodeReadOnlyValue`. Node row widgets expose `nodeParameterRole` and
`nodeParameterRowPitch` for geometry audits; list headings expose `headerRow`.

The metric audit covers the status line, ten enabled viewer tools, the five Properties filter
choices, card/control containment and socket-row alignment at DPR 1, 1.5 and 2. It also verifies the timeline menu set remains expanded at
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

Timeline switches retain ToggleCell squares (24), with IconChrome (16) glyphs: unchecked is an
empty neutral bordered box, checked uses the same Regular glyph as the column heading. Hidden
audio retains its cell; Spacing::XS separates switch boxes in both header and layer rows.
Headers and rows share the ChromePadding left inset,
column geometry, and Ui text role. Layer and nested-property disclosures are unboxed 24px hit
targets with their existing 16px chevrons and an XS gap before the text. KDiamond uses the Bold
outline at every DPR. Colour parameter rows show only the swatch; expanding the parameter exposes
the individual R/G/B/A numeric rows, so narrowing the table never crushes four inline fields.
Layer rename uses the shared KLineEdit at the name label's horizontal bounds, vertically centered
in the row; it never covers the chevron or expands as text is typed. It follows column resizing.
Return or focus loss commits a changed name once; unchanged text creates no transaction and Escape
cancels without committing on the subsequent focus loss.

The timeline's graph editor REPLACES the key lanes rather than sitting beside them: in graph mode
the lane region paints only its Surface backdrop and the curve view covers it, because two views of
the same keys at once only make an artist ask which one they are editing. Its left gutter is
`Size::GraphValueAxis` wide and carries the ACTIVE curve's value ticks -- the curve of the primary
keyframe selection, else the one on the selected row, else the first -- and the gutter is painted
over the curves so a line running off the left edge slides under the axis instead of colliding with
its labels. A scalar curve strokes in `Color::Keyframe`; a component curve takes
`Color::ComponentX/Y/Z/W`, which R/G/B/A borrow unchanged so one hue always means "the first
component"; selection is Accent, which none of the five is. Ease handles are drawn only for a
SELECTED key on an eased segment, as a hairline to a `Size::GraphHandleDot` dot. The graph toggle
persists under `timeline/graph-editor`. View exposes checkable Keyframes and Graph Editor choices;
enabling either turns the other off, and both may be off to show plain layer lanes.
`timeline/keyframes-visible` and `timeline/snapping` persist Keyframes and Snap to Frames.
These controls appear only in View, with no header tool buttons. Header menu order is Add, View,
Select, with text-only menu buttons and no caret indicators; the composition dropdown is absent.
These are the unchanged shared KMenuButton/EditorChromeRowSpec controls and application theme used
by Assets, Viewer and Nodes: no Timeline-specific menu painting, stylesheet or background palette.
Right-clicking empty layer-table space opens a shared kit popup with the existing Add submenu,
then direct Keyframes, Graph Editor, Snap to Frames, Zoom to Fit, Select All and Deselect All actions.
There are no View/Select wrapper submenus or selection-only layer commands; action objects and
checked state are shared with the header. Opening the popup never changes the layer selection.
Timeline's fullscreen button sits at the right edge of the layer-table header, beside the divider.
Keys off hides both key lanes and collapsed summary
glyphs. Empty key-lane labels paint no chip. Lane snapping is `snapping && !Shift`.

Timeline lanes use `LanePadding` (12) on both sides of their time axis. The layer column/lanes split
is a draggable `KSplitHandle` (`Size::SplitHandle`, 6, its hit zone; it paints a `TimelineSeparator`
(2) Background hairline centered in that zone), used identically for the header split so the ruler
and lane region always move together with the column headings and rows below them. It is clamped
between the layer table's own minimum (name, Blending and Parent columns all still visible) and a
maximum that leaves `PanelMinWidth` for the lanes. A fresh Timeline defaults to 37% of its live
width; an existing `timeline/layer-column-width` pixel value wins, and a double-click or Reset
Workspace returns to the 37% ratio. All timeline rows share TimelineRow pitch and a zero origin;
the 28px KPropertyRow is centered within that pitch.
Selected rows use SurfaceRaised with no edge stripe. Every populated and empty row uses a
Background hairline separator (the window color, never a lighter border), without alternating fills.
The work-area slider occupies the lane half
of the layer-column header row, directly below the ruler. Its active range uses SurfaceRaised,
matching selected layer rows; outside the range stays Surface, with accent endpoint handles.
Cached-frame strips use green Ok. Ruler tick labels and the current-frame readout are centered on
their needles. The Accent playhead has a solid tab with a downward-pointing tip below its readout,
joining the single-pixel line through the work area and lanes; the readout never paints over it.
The readout reserves its rectangle against tick labels. The footer navigator hides
at fit, keeping the zoom slider visible. It uses a rounded 10px neutral BorderHover thumb with no
painted background or track; hover/drag uses BorderActive. Its ends advertise resizing, and its
body advertises panning.
Clip trim edges show SizeHorCursor using the same
hit tolerance as trimming; locked layers do not advertise trimming. The vertical scrollbar appears
only when the expanded row content exceeds the viewport. Only then does a ScrollBarHover-width
gutter reserve its space in the ruler, work-area and body rows together. Otherwise lanes extend
to the panel's right edge, with no fullscreen or scrollbar gutter.
Object, Transform, Source groups are collapsible Title Case rows, joined by one group per upstream
value node driving the layer, titled by that node's display name. KPropertyRow's leading-indicator
layout places the diamond or disclosure in a ToggleCell column, then the compact label and
bounded controls; vector component labels live inside fields, three per row so a Vector 3 shows all
of its components. New name: `timelinePropertyDisclosure`.

The expanded layer's hierarchy nests one step per depth: the layer row itself (never indented), a
group (Object/Transform/Source or a DRIVE-1 upstream node), that group's parameter rows, and an
expanded parameter's own component rows each indent their label one `Spacing::M` step further right
than the level above. Only the label column indents; the diamond and every value cell stay in the
same fixed column regardless of depth, so a parameter row's and its own expanded component rows'
editors always line up.

A DRIVEN parameter row never shows an empty cell. It hides every editor it would otherwise carry --
and its diamond, which has nothing to key -- and shows a Ghost KButton carrying `IconId::Link` and the
driver node's display name, followed by a Value-role KLabel holding the resolved value read-only.
Properties and the timeline use the same two elements in the same order and read the same resolved
string. New names: `timelinePropertyDriven`, `timelinePropertyDriverLink`,
`timelinePropertyDrivenValue`; the Properties spellings `propertiesDrivenDisplay`,
`propertiesDriverLink` and `propertiesDrivenValue` are unchanged.

`Color::OnAccent` is white (#ffffff). Kit button painters use it for ink on accent fills,
including transport, loop, snap and keyframe toggles. KToolColumn is sticky at the canvas left
edge, paints the header Surface, and owns ChromePadding and ChromeGap around bordered ToggleCells.
KDropdown's minimum is the measured widest item plus its icon, padding and chevron; requested
fixed widths are floors. The chrome builder respects that minimum after assigning density.

Properties has a leading `propertiesFilterStrip` KToolColumn at `Size::ToolColumnWidth`. Its five
exclusive KIconToggles are All, Object, Transform, Source and Graph. All is the default and the
choice persists as `properties/filter`; the strip never changes section collapse state. Object
includes the no-selection Composition section, Source covers Solid/Text/Shape/Image/Audio source
sections, and Graph covers Merge inputs plus every upstream section. A group with no section for
the current selection is disabled, and a persisted group that disappears after selection changes
falls back to All. Tooltips state the complete section set so the icon-only controls remain
discoverable.

Node Add menu and search order is Sources, Layers, Compositing, Colour, Values, Math, Convert, String,
Logic, Time, Color, Vector, Utilities, Output. The UI category projection owns normalization;
the Colour image-effect section projects `NodeCategory::Color`, while Color holds value nodes.
Categories are not persisted, so no migration is needed. Utilities contains only reroute and
Separate/Combine plumbing (reroutes remain link gestures). Time includes Time, frame readouts and
time conversions; composition readouts and literals remain Values. HSV operations belong to Color.
Kit menus with `columnFlow` use real action-backed controls in additional columns, capped at
`kMenuWindowHeightShare` (0.5) of the owner window and positioned within that window. Empty-canvas
double-click opens Add search at the cursor. Footer count is hidden at zero; snap and link style
are exposed by View, with their legacy footer controls hidden.
`kNodeLinkWidth` (1) is identical for idle, selected and hovered links; activation applies
`kLinkActiveLightness` (135 percent) to the socket-kind tint, preserving its hue.
Node parameter rows are KPropertyRows with a separate fixed diamond column. Their numeric fields
use compact resting precision: at most two decimals, no trailing zeros; editing retains full
precision. KDiamond draws vector geometry through the painter's device transform, including canvas
zoom. Vertices and `kDiamondStroke` (1.5, Bold icon weight at IconSmall) resolve to integer device
pixels. No diamond pixmap is cached or scaled. New automation name: `nodePropertyRow`.


## Colour Surfaces

Colour controls distinguish `Display` sRGB from stored `Reference` linear sRGB through the
session's `colorConverter()` accessor. Properties colour rows and node-card chips show the same
converted colour as the qualified Bloom Neutral viewer. Picker hex text, HSV/HSL fields, spatial
controls, recent colours and screen samples are display sRGB; editing them commits Reference
values through the existing session command. A Reference-tagged colour cannot be painted directly
as a QColor. Opening or closing the picker never changes the document.

Expanded Properties RGBA fields show normalized display numbers for in-range colours. Signed or
HDR reference RGB switches the whole row to exact reference numbers with a `reference` suffix;
the chip continues to show the clamped display colour. Entering an RGB number outside [0, 1]
authors that component in reference space. Editing one field preserves every untouched reference
channel exactly. Pending or unavailable conversion disables the reference chip and labels numeric
values as reference. Existing document numbers are preserved; this is a presentation boundary,
not a data migration. The legacy timeline colour-row adapter remains pending this conversion.

## Image Assets And Source Cards

Assets rows use `KRow` with a leading Chrome kind glyph: Composition (`film-slate`), Image
(`image`), Sequence (`images`), Audio (`music-notes`), or Font (`text-t`). Kind text remains
Composition, Image, Sequence [member count], Audio · duration, or Font · family/style.
A missing/changed first member has a warning glyph; the context menu exposes Relink and Remove.
The footer is four `KIconButton` controls with Chrome glyphs and exact tooltips New Composition,
New Folder, Import, Delete. New Folder is live in both Add and the footer. Compositions have a
separate expandable **Compositions** root; folders form a hierarchy for imported assets. Folder
rows use the kit caret disclosure, with Qt owning indentation and keyboard navigation. Expansion
and selection survive project projections; filtering temporarily opens matching ancestors.

Assets and folders rename in place through a kit line edit, from Rename or F2. Escape restores
the row without an edit. Every accepted rename uses the shared command transaction. Tagged rows
compose compact `KButton` chips in `KRow`'s trailing area: the first tag and a `+n` count when
more tags exist. The full tag list is available in the tooltip. Clicking a tag filters by it;
the count opens Edit Tags. Narrow rows show a compact count for the complete tag set, preserving
space for the asset name. The context menu also exposes **Edit Tags…**, with one kit line edit
per tag, Add Tag and Remove Tag controls. Applying to multiple selected assets replaces their
sets in one transaction.

Search accepts `tag:` followed by a case-insensitive tag substring; plain text matches names and
tags. Unmatched ancestors remain visible only when a descendant matches. Internal asset drags
move onto a folder, move back to the root by dropping on empty space, or reorder above/below
siblings. The internal selection MIME coexists with `application/x-bloom-asset`, retaining
canvas and Timeline media drops. Internal drags are tied to the originating tree and snapshot;
a changed or replaced project refuses a stale drag. Composition canvas drops remain refused
with a user-visible message until composition sources are supported.

File > Import, footer Import and file drops onto Assets all prepare one worker import transaction.
A drop onto Assets also accepts an `smb://` URL a file manager hands Bloom for a mounted network
share, resolving it through the same GVFS mount the file manager used; an `smb://` URL naming a
share this machine has not mounted yet is still accepted (the cursor stays honest about there
being something to do) but imports nothing, reporting "Connect to `<host>/<share>` in your file
manager first" through the status bar's transient-message cell rather than dropping it silently.
Import, Relink, Open Project, Save Project As and Export Frame file dialogs list every mounted
network share in their sidebar alongside the platform's own entries.
Dragging an image/sequence row to Nodes creates an Image source; dragging an Audio row creates an
Audio source. Dropping media on Timeline creates the matching wired Layer. Image source cards and
Properties use an Asset `KDropdown` listing the project's Image
and Sequence assets by their display names, with the kind glyph and Image / Sequence [n]
text. Selection commits the stable asset id through the shared parameter setter. A removed id
remains selected as **Missing asset**, in muted ink, with the id in its tooltip. An Image source
card derives its title from the asset name; the title band's category remains **Sources**.
Timeline's imported default layer labels follow the asset display name; independently authored
Layer names remain intact. Rename changes project metadata and does not rename media files.

Loop Mode offers **Hold / Loop / Ping-pong**; Color Space offers **Auto / sRGB / Linear / Raw**.
The card, generic Properties rows and timeline source twirl-downs read the same UI vocabulary table.
Start Frame remains an integer and Premultiply a toggle. Dimensions (`1920 × 1080`) and sequence
Range (`24 frames · 0–23`) are read-only `KLabel` rows on the card and in Properties, read directly
from the imported asset record. Range uses the actual numbered endpoints and is absent for stills.

An Image card reserves `ImageThumbnail`'s 72-pixel body cell, with a `SurfaceSunken` background,
for a cached proxy no larger than 64 pixels on either axis. The cell fits that image without
changing its aspect ratio. Sequence proxies follow session time, Start Frame and Loop Mode using
the runtime's exact rational frame mapping and the Image source timing contract, including
holding a preceding member across a sequence gap.
Asset/parameter/time changes invalidate the displayed proxy; a cancellable worker publishes the
newest result. The controller caches proxies by content digest, selected frame and interpretation,
bounded to 512 cache entries. File resolution, hashing, decode and downsampling run on the worker;
painting only reads a published `QImage`. Missing, unreadable and pending sources show the warning
glyph in muted ink, never a blank cell. Thumbnail work has task progress and cancellation; shutdown
cancels without blocking the UI. Relative-brightness probes pin the decoded and missing states.
Automation names are `nodeImageAsset`, `nodeImageDimensions`, `nodeImageRange`,
`propertiesImageAsset`, `propertiesImageLoopMode`, `propertiesImageColorSpace`,
`propertiesImageDimensions`, and `propertiesImageRange`.

Text box rows (`Box`, `Wrap`, `Vertical Alignment`, and `Overflow`) are ordinary registry rows.
For a selected boxed text layer, Select-tool viewer overlays draw the box and corner/edge handles;
dragging a handle resizes the box in one session interaction and commits one transaction on release.
Shift preserves aspect ratio and near composition edges snap to the edge. Double-clicking text focuses
the Properties text field; inline canvas text editing is intentionally out of scope.

Audio source cards reserve the same 72-pixel body cell for a waveform summary. The summary is
decoded and bucketed on the worker; the card, Assets panel and timeline only paint its immutable
min/max ranges. Audio source Properties reuse the asset `KDropdown`, expose Start Frame and a
linear 0–2 `KSlider` with its `KDiamond`, and show read-only duration, sample rate and channel
count. The `Audio` socket uses its own `SocketAudio` token, separate from Image and Accent.

Audio timeline rows use `DataAudio` for their clip bar and paint the published bucket ranges with
that token's ink. Their speaker cell is a live mute command backed by the Layer `enabled` flag;
image, solid and text rows hide that speaker control while reserving its column. Video rows show
it only when imported stream metadata reports audio. Video audio toggles connect/disconnect the
source's audio input at the Layer boundary through undoable graph commands, leaving its image
connection intact. Video rows retain ordinary layer selection, expansion and property controls.
Solo and range remain the same Layer semantics as other rows. An unavailable audio
file shows the warning glyph and remains relinkable from Assets.

New Composition and the composition Properties section expose a `KColorChip` Background Colour.
Viewer Solid mode paints that authored RGBA colour, initially opaque black. Black, White and
Checkerboard remain session choices. This viewer background does not alter composition pixels
or export alpha.

## Live Value Editing

A `KValueField` is a horizontal scrub handle, including its label. Drag past the platform drag
threshold to change one single-step per logical pixel; Ctrl uses one tenth of that step and Shift
uses ten times the step. A click opens an inline editor with the value selected. Valid numeric text,
Up/Down and Page Up/Page Down changes update the session's live value and every visible readback
immediately. Intermediate text such as a minus sign does not replace the last valid value.

A pointer release or key release finishes a scrub/step; Enter, Tab and focus-out finish typing.
One completed gesture is one undo entry. Escape restores the starting value without history.
Properties, node cards and timeline fields use the same kit gesture signals. The active field keeps
its text/caret or scrub base while peer fields show the live number. Colour-picker spatial drags and
channel edits use the same live/commit/cancel boundary; closing an active picker accepts its edit.
Preview rendering runs independently of these control readbacks.

## Canvas text editing

Double-click a text layer with Select, press Enter on a selected text layer, or click with Text to
place a new layer and type. The caret and selection use the render layout's UTF-8 byte positions,
including wrapped lines and vertical alignment, mapped through the evaluated world transform and
viewer zoom. The caret uses `Color::Accent` and a cosmetic `Size::Hairline`, snapped to device-pixel
centres. Selection uses Accent with `kDisabledOpacity`; IME preedit uses an Accent underline.
These are display overlays and do not appear in rendered frames or exports.

Click or drag to position/extend the selection. Arrows, Home/End and Shift move/select within text;
Ctrl+Left/Right move by word, Ctrl+A selects all, and platform Copy/Cut/Paste bindings work. On macOS,
Command also works for the control-modified editing commands. Delete and Backspace edit the string;
layer deletion, nudging, playback and tool shortcuts are suspended while the canvas owns text input.

Enter accepts point text on one line. Box text, existing multiline text, and text made multiline
with Shift+Enter use Enter for a line break and Ctrl+Enter (Command+Enter on macOS) to accept.
Focus loss accepts; Escape restores the original string without history. A completed gesture is
one `Edit Text` transaction. The Properties single-line and expanded text fields use the same
live/commit/cancel seam. Time, composition and revision changes cancel stale canvas input.

Font resolution and layout run on a cancellable worker with one active and one newest request.
The canvas reports preparation or a diagnostic while geometry is unavailable; typing and Properties
readback remain responsive. Native input-method composition previews through the String override,
then replaces its preedit exactly once on commit. Uncommitted preedit is discarded on cancellation
or teardown. The native input-method candidate rectangle follows the transformed caret.

## Native geometry handles

Viewer corner and edge handles edit NATIVE SIZE for Shape, Solid and boxed text. Edges resize one
axis; corners resize both, and Shift preserves aspect. Point text shows corners only and resizes
font size uniformly. Box text resizes the wrapping box while keeping font size. Line endpoints and
path anchors are source geometry; path anchors and tangent handles appear on selection without an
edit mode or double-click. Stroke width and corner radius remain native authored values.

LOCAL TRANSFORM Scale remains a percentage field in Properties and timeline and stretches the
finished geometry, strokes included. No vector handle gesture edits it. Raster images, video and
nested compositions have no editable native geometry and retain Scale handles. Rotation and anchor
handles always edit transforms. The gizmo uses WORLD TRANSFORM, including every ancestor, and
compensates Position so the opposite resize handle stays fixed. A gesture previews live and commits
one undo transaction. Properties and timeline keep their existing source-size and transform rows.

During a native-size or transform gesture, source-size and Scale readbacks in Properties and timeline
refresh from the same session overrides as the viewer. Source-size gestures leave Scale unchanged;
transform field edits leave native size unchanged. Cancel restores readbacks without history, and
release commits the frozen-start values as one transaction. No property rows are added or renamed.

## Save And Recovery Messages

Every save outcome is reported honestly and in the artist's terms; none of them is ever collapsed
into "saved". A save that refuses a value the format cannot spell reads

> Save failed: `<field>` is not a finite number.

naming the exact field (for example
`project/Composition[7]/AnimationCurve[12]/Keyframe[4]/value`). The project file on disk is
untouched and the session stays open and editable, so the artist can correct that value and save
again; the message is a status report, not a dialog, and never an abort.

On the launch after an abnormal exit, a recovery file describing work the project on disk does not
hold is offered once, before the artist starts editing: "Bloom closed unexpectedly with unsaved
changes. Recover the unsaved changes from `<file>`?" with Yes/No. Yes opens that file through the
ordinary Open flow, unsaved-change prompt included; No leaves it alone. Nothing is offered when the
last shutdown was clean, and a recovery file older than its saved project is never offered.

## Memory Pressure Message

When the operating system reports less memory available than Bloom reserved for it, the caches are
trimmed to half their budgets and the status bar shows one transient notice:

> Memory pressure: caches trimmed

It is a status report in the same row as every other notice, not a dialog and not a warning colour:
nothing failed, no work was lost, and the artist has nothing to decide. It says what Bloom did, in
the past tense, because by the time it appears the trim has already happened. It appears once per
episode of pressure rather than on every poll, so a machine that stays busy does not repeat it, and
it never names a byte count -- the cache cell beside it already reports what the caches hold.
