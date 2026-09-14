# Visual Language

Status: accepted

Updated: 2026-09-14

## Purpose

Bloom needs a coherent visual foundation for a dense professional interface. Icons and typography
must remain legible at small sizes, work across Linux, macOS, and Windows, and be usable without a
web runtime or JavaScript toolchain.

This document owns Bloom's current iconography, interface type, and design-token decisions. The
tables below are product truth and match `src/ui/include/bloom/ui/kit/tokens.hpp` exactly; the
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
popup. **Focus wins over hover**: a control that is both keeps `Accent`, so putting the pointer on
the thing you are editing never takes the focus indication away. A control that is borderless at
rest (a `KValueField` cell) paints its resting border transparent and gains the outline only on
hover or focus. `kit::borderForInteraction()` is the one implementation;
`src/ui/tests/kit_focus_border_tests.cpp` pins every control at all four points.

The color widgets are the documented exception: `KColorChip` and `KRangeSelector` (through
`kit::drawFocusRing()`) and `KColorSwatches` and `KColorPicker` (through accent pens of their own)
still draw a `1.5` accent ring outside the focused element, because their focusable target is a
color field, a swatch, or a handle whose own border color is the artist's data rather than a state
channel -- a border-color change there could not carry focus at all.

### Spacing

| Token | Value |
| --- | --- |
| `XXS` | `2` |
| `XS` | `4` |
| `S` | `8` |
| `M` | `12` |
| `L` | `16` |
| `XL` | `24` |
| `XXL` | `32` |
| `Gutter` | `6` |
| `PanelHeader` | `10` |
| `MenuItemY` | `6` |
| `MenuItemX` | `10` |

`Gutter` is the visible `Background` gap between panels, AND (task C1, item C4) the window's own
inner padding: the central area that hosts panels insets itself from the window's edge by the same
`Gutter` on all four sides, so a panel never touches the window border either. Panels float on the
window; they do not share edges. `PanelHeader` is the panel header's own vertical padding --
deliberately off the base-4 scale, not rounded to a nearby step. `MenuItemY` and `MenuItemX` are a
menu row's own padding, off the base scale for the same reason: a menu row is denser vertically and
roomier horizontally than the scale offers. `MenuItemY` sizes a `QMenu` popup's own rows
(`kit::AltUnderlineProxyStyle`, owned by the kit foundation); the menu BAR's own row (task C1, item
C3) instead gets its vertical breathing room from `Spacing::S` padding around the whole bar, with
`MenuItemX` alone governing each bar item's own horizontal padding.

### Size

| Token | Value | Use |
| --- | --- | --- |
| `ControlCompact` | `22` | Dense chrome controls |
| `Control` | `26` | The default control height |
| `ControlRoomy` | `32` | Prominent controls and dialog buttons |
| `IconSmall` | `12` | Dense chrome |
| `IconMedium` | `16` | Default |
| `IconLarge` | `20` | Prominent actions |
| `TitleBar` | `34` | `kit::TitleBar`'s own row height. Compiled and tested, but currently unused: task C1 moved Bloom to native (OS) window chrome only, so `MainWindow` never constructs `kit::TitleBar` today -- the token and the widget both stay ready for a possible future custom-chrome/CSD return |
| `PanelHeader` | `30` | The node graph's own card header height and row-pitch multiplier (`node_editor.cpp`) -- despite the name, not the editor panel's own header row below |
| `EditorHeader` | `48` | An editor panel's header row |
| `TimelineRow` | `34` | No longer the timeline's row pitch. The layer-stack rows, their clip lanes, and the keyframe lanes all step by `32` (`ControlRoomy`), the pitch the timeline design specifies; this token survives only as a stylesheet variable until the kit either restates it as `32` or retires it |
| `ScrollBar` | `8` (`12` on hover) | Overlay scrollbars with pill thumbs |
| `MenuMinWidth` | `200` | The narrowest a `QMenu` popup may be |
| `PanelMinWidth` | `300` | Every `EditorArea`'s own strict minimum width, in Figma design px |
| `ValueCellMin` | `72` | The numeric floor a Properties value cell (`kit::KValueField`) retains at the panel minimum |

`MenuMinWidth` is a floor, never a cap: `kit::AltUnderlineProxyStyle` claims it for every menu ROW,
and a menu's width is the widest row it holds, so a long label still widens the popup past it. It is
applied to the row rather than to the popup window because a row that stopped short of the frame
could not carry the full-width accent hover bar the State table requires.

An editor panel's footer strip (task C1, item C5) is not a distinct token: it reuses `Control`
(`26`) exactly, the same way its header reuses `EditorHeader`. The footer is `Surface`-backed with
the header's own `Border` hairline, just on its top edge, and is empty by default -- see this
task's report for why an editor's own existing bottom bar (the viewer's status readout, the
timeline's transport) is not moved into it yet. Editors that offer a task-specific footer, such as
Assets and Nodes, use the same strip rather than inventing a second chrome treatment.

### Editor panel rows

| Panel | Header menus | Body | Footer |
| --- | --- | --- | --- |
| Assets | View, Add, Select | Searchable two-column tree: Name and Kind | New Composition, disabled New Folder, disabled Import, right-aligned Delete |
| Timeline | Left cell: switcher, Add/View/Edit/Select, elided composition name (`Type::UI`), fullscreen. Right cell: work-area strip, tick labels, and playhead head | Column headings first; synchronized layer stack, expanded property rows and their keyframe lanes | Body's bottom row: transport/readout under the layer column, `Control`-high navigator under the lanes |

The timeline header splits at exactly the body's layer-column divider. Its ruler begins at the lane
region's x origin and reserves the same vertical-scrollbar gutter. The ruler lives inside the
`EditorHeader` row; it has no separate body row. A continuous `1px` Accent playhead stroke connects
the header, the right side of the column-heading row, and all lanes. The work-area strip shows the
persisted range, with Accent grips at both endpoints and the same zoom/scroll axis as the ruler.

Timeline major labels use the frame cadence `1, 2, 5, 10, 24, 48, 96, …`, chosen from available pixel
density and actual label font metrics. Ten-frame spacing is used at fit when it has enough room;
labels are never allowed to overlap or cross the ruler's right edge. The navigator shows the full
duration with an Accent-dim visible-range window and solid Accent edge grips. Zoom and scroll
change the window and projections, while layer-row height stays `32` (`ControlRoomy`).

Assets uses the `Folder` panel-switcher icon and the `DataComposition` vocabulary for composition
rows. Its disabled affordances keep their honest reason in a tooltip, and its composition actions
use the ordinary `Accent` selected-row treatment; no media thumbnail or import chrome is implied
before the media pipeline exists.

#### Panel width rule (task WIDTH-1)

Every `EditorArea` overrides `minimumSizeHint()` to report exactly `PanelMinWidth` wide by its own
header-plus-footer height, computed without ever consulting the hosted editor's own size hints. A
`QSplitter` reads that fixed floor, not whatever the hosted editor happens to want, so switching a
Properties selection -- more `KValueField` cells, a longer parameter label -- can never grow past
what a narrower pane already had and nudge a splitter handle. The owner's own framing: "let it have
min width of something like 300px ... so inner sections and users can compromise to also have that
strict min width instead of kicking borders around."

The rule applies uniformly to every panel kind, but only Properties needs help holding to it:
Properties is the one hosted editor that is a *form* rather than a canvas, so `EditorArea` hosts it
inside a `QScrollArea` with `Qt::Ignored` on the horizontal axis (the scroll area's own minimum
width never asks the panel's content layout for more room than it already has) and
`widgetResizable` set, so the real `PropertiesEditor` widget is actually resized down to whatever
width the panel currently has. Properties owns an inner body scroll area beneath its search
header. At the 300-design-pixel minimum, row labels elide (`Qt::ElideRight`, with full names in
tooltips), numeric cells retain `ValueCellMin`, short component prefixes claim their measured glyph
width, and RGBA fields use two columns. The body scrolls vertically without requiring horizontal
scrolling. The node graph, timeline, and viewer retain their existing canvas and wrapper behavior;
node cards retain their separate minimum-width rule.

### Viewer header

The Viewer header keeps the panel switcher first, followed by the compact composition selector
(`viewerCompositionSelector`), its trailing composition-command button, the active object selector
(`viewerObjectSelector`), View and Select menu buttons, and the fullscreen toggle
(`viewerFullscreenButton`). The composition selector lists document compositions; the object
selector lists the current composition's layer display names and shows `None` when selection is
empty. New, Duplicate, Delete, and Rename Composition use the same command path as Assets and the
main Composition menu. F11 remains owned by the main window and is named in the fullscreen
button's tooltip and accessible label.

Viewer header menus never wrap or clip. When the available width cannot hold both menu buttons,
they are replaced by one `…` overflow button containing the same View and Select menus. The
composition and object selectors remain visible and elide their closed-field text within their
own bounds.

### Viewer overlays

Viewer guides are display-only paint layered above the delivered composition pixels; they never
enter a cached frame, export, or project render. All guide toggles are off by default. Safe Areas
draw action-safe and title-safe rectangles, with Broadcast (4:3, 90%/80%), HD (16:9, 93%/90%,
EBU R95), Cinema, Social (centre 1:1 and 4:5 crops), and Custom presets. Safe-area percentages
are stored on the composition; the remaining guide toggles are viewer display preferences.
Centre Cross and Thirds use the transformed composition rectangle. `viewerRulers` paints top and
left composition-pixel rulers that follow zoom and pan, and Pixel Grid appears only at 400% or
above. Guide lines and selected bounds use relative brightness/semantic kit colors so their
readability survives background and zoom changes.

### Viewer content bounds

For each selected layer, paint a 1 px `Accent` polygon and a solid filled 6 px diameter anchor dot.
Geometry follows the same composition-to-view transform as the image, including zoom, pan and pixel
aspect; line width and dot diameter stay in screen design pixels. This is canvas painting with no
object names. It reads geometry retained by the delivered frame, including display-only playback cache hits.

Text Alignment uses the existing `nodeOperandSelector` in node cards and the new
`timelinePropertyAlignment` dropdown in source property rows. Width, Height, Line Height, and Letter
Spacing reuse generic scalar cells and animation diamonds. Existing object names are unchanged.
### Properties sections

The Properties panel groups its rows into `kit::KSection` -- a header row of
`[chevron][Title Case title][spring][Reset]["..." menu]` above a collapsible body. The title is
Title Case rather than the uppercase `editorSectionTitle` micro-type used elsewhere, because this
header carries controls of its own and uppercase beside two buttons reads as shouting.

A layer is grouped as **Object** (Visible / Solo / Locked switches, Blending, Opacity as a slider
plus a value cell plus its keyframe diamond), **Transform** (Position X/Y with an axis-link toggle,
Rotation as slider plus cell, Scale X/Y with a proportional-link toggle that is engaged by default,
Anchor X/Y), and then one section per source (Solid Source, Text Source). The timeline's twirl-down
rows use the same group names, so the two surfaces name the same things identically. With nothing
selected the panel shows only the read-only Composition section.

Collapsed state persists per section under `properties/sections/<id>/collapsed`. A section's Reset
writes each of that group's parameters back to the value the node definition registry declares as
its default, through the same session setter the row itself uses. Each parameter reset is an
ordinary undoable command. The header menu's Collapse all / Expand all are answered by the panel, which is
the only thing that knows the full set of sections; a section that has nothing to reset (the
composition view, a merge's inputs) hides its Reset rather than offering a control that would do
nothing.

Both sliders share their row's commit with the paired value cell. `kit::KSlider` carries no scrub
gesture signals, so the ADR 0017 boundary is the pointer release: while the handle is dragged the
cell mirrors the slider and nothing is written; the release -- or a keyboard step, which is not a
drag -- is the single commit. The rotation slider spans one turn each way and pins at its ends; the
cell stays the authority for a wound value past that, which the schema accepts and the slider
cannot reach.

The remaining parameters of a selected definition render in its source section. The definition
owns kind, animation support, role and default; the schema-owned selector vocabulary supplies the
closed enum choices. Labels derive from role names, with separators converted to spaces. Existing
hand-crafted controls keep their identities and command paths.

| Parameter kind | Properties control |
| --- | --- |
| Scalar | Value field and a diamond when animatable |
| Vec2 / Vec3 | Two / three numeric components sharing one parameter |
| Color | Color chip opening the existing picker; expandable RGBA fields |
| String | Text field; Text content also offers a multiline expander |
| Integer / Boolean | Integer field / switch |
| Closed enum | Dropdown carrying the schema's stored integer values |

The header search filters row labels by case-insensitive substring. Unmatched rows and empty
sections hide; clearing the query restores them. During a search, matching section bodies are
shown without changing their persisted collapsed state. Escape clears search and focuses the panel.

After the selection's own sections, upstream nodes appear once each in breadth-first order through
image inputs and driver links, to depth three. Merge and Output terminate traversal. A layer's
direct source is already represented by its source section and is not duplicated. One trailing
“and N more upstream” row counts unique nodes beyond the limit. Upstream controls use the same
registry rows and edit their exact parameters without changing selection. “Jump to node” selects
that node and frames it in the existing node canvas in the same window.

Driven rows replace editable values with a link glyph, the driver's display name, and its resolved
value. Clicking the link navigates to the driver. Resolution is cancellable background work using
the CPU reference value evaluator; “Resolving…” is indeterminate activity, and failures display a
diagnostic rather than an invented value. A private snapshot copy with muted built-in probe nodes makes
detached value branches inspectable without publishing changes to the project. Non-animated value
nodes with driver kinds restricted by the image compiler use the same private mute path, retaining
their value kernels. Inspection can resolve these values independently of current image-source
render support. Only the newest
request may update the panel; closing it cancels work and releases workers off the UI thread.

Right-click a parameter row or its control to Reset to default. Constant and animated values use
the same parameter setter as editing; driven values use the existing disconnect command, which
restores the registered default and keeps the binding undoable. Locked parameters and read-only
information rows disable Reset.

At the unchanged 300-design-pixel panel minimum, the header stays fixed and the body scrolls
vertically. Labels and section titles elide, numeric fields retain their kit floor, and the
hand-crafted RGBA fields use two columns. Content never requires horizontal scrolling. These Qt
controls and command paths apply equally on Linux, macOS and Windows; platform qualification is
separate from the layout contract.

New object names: `propertiesSearchField`, `propertiesScrollArea`, `propertiesScrollBody`,
`propertiesRegistryPanel`, `propertiesSection_registry`, `propertiesRegistryRow`,
`propertiesRegistryDiamond`, `propertiesRegistryEnum`, `propertiesRegistryBool`,
`propertiesRegistryMultiline`, `propertiesRegistryString`, `propertiesRegistryInteger`, `propertiesRegistryValue`,
`propertiesRegistryColor`, `propertiesRegistryColorExpand`, `propertiesTextMultiline`,
`propertiesTextExpand`, `propertiesUpstreamPanel`, `propertiesSection_upstream-<node-id>`,
`propertiesJumpToNode`, `propertiesMoreUpstream`, `propertiesDrivenDisplay`, `propertiesDriverLink`,
`propertiesDrivenValue`, `propertiesRowContextMenu`, and `propertiesResetToDefault`. Existing object
names are retained. Registry rows expose `parameterId` and `role`; upstream sections expose `nodeId`.

### Viewer footer

One row, `Size::Control` tall, on `Surface` with a `Border` hairline along its top edge. Left to
right:

| Control | Object name | Notes |
| --- | --- | --- |
| Channel | `viewerChannelDropdown` | RGBA, RGB, R, G, B, Alpha |
| Zoom | `viewerZoomDropdown` | Fit, 25, 50, 100, 200, 400, plus one trailing custom value |
| Resolution | `viewerResolutionDropdown` | Auto, Full, Half, Quarter; persisted in `viewer/resolution` |
| Effective resolution | `viewerResolutionReadout` | `Auto · ¼`; part of the Resolution control, not an item of its own |
| Background | `viewerBackgroundDropdown` | Solid, Checkerboard, Black, White; persisted in `viewer/background` |
| Transport | see below | Go to start, step back, play/pause, step forward, go to end, loop, RAM Preview |
| Frame / time readout | `viewerTimeReadout` | `TypeRole::Value`; click to type an exact frame number |

The transport's buttons are `viewerStepToStartButton`, `timelineStepBackButton`, `playPauseButton`,
`timelineStepForwardButton`, `viewerStepToEndButton`, `timelineLoopIndicator` (a real toggle now,
not a status glyph), and `timelineRamPreviewButton`. The four names that still begin with
`timeline` are the ones that moved here from the Timeline's own bottom row: they changed parent,
not identity, and renaming them would have broken the contract every test and every future
automation reads them by.

Each button is a `Size::Control` square carrying an `IconRole::Control` glyph. The readout reserves
a fixed width from the widest string it can ever show, not from its current text, so a count
changing sixty times a second never relayouts the row beside it.

Channel is a presentation remap and nothing else: it is applied while packing the frame for the
canvas, after display-referred conversion, and never reaches an export, a cached frame, or the
display buffer the colour pipeline produced. Alpha shows the alpha channel as luminance; R, G and B
show that one channel as grey; RGB is the composite with alpha forced opaque.

Background chooses the canvas surround. Solid is the application's own `Background` token, and the
control says so: a composition carries no background colour in the document model, so a
"composition background" would be an invented value. Black and White are literal, because a known
value is the entire reason an artist asks for them.

### Window status bar

One kit strip at the bottom of the main window, `windowStatusBar`, `Size::Control` tall, on
`Surface` with a `Border` hairline along its top edge. Not `QStatusBar`: that brings its own
chrome, size grip and item model. It is a row of the central column, so it stays visible whichever
central page is authoritative.

Left to right, each cell silent when it has nothing true to say:

| Cell | Object name | Shown when | Token |
| --- | --- | --- | --- |
| Colour-state chip | `windowStatusBarColorChip` | Always | Chip colour follows the preview's qualification state: `Ok`, `Warn`, or `Error` |
| Preview state | `windowStatusBarPreviewState` | Always | `Accent` rendering, `Ok` ready, `Warn` unsupported, `Muted` cancelled, `Error` failed |
| `N dropped` | `windowStatusBarDroppedFrames` | While a playback run is counting | `Muted` at zero, `Warn` above it |
| Cache | `windowStatusBarCache` | While a RAM preview run is caching, or while the cache holds frames | `Accent` |
| Message | `windowStatusBarMessage` | While there is a notice or running work | `Foreground` |
| Version | `windowStatusBarVersion` | Always | `Faint`, `UiSmall`, right-aligned |

The numeric cells use `TypeRole::Value`, the monospaced role, so a count never reflows the cells
beside it as it changes. "Silent when it has nothing to say" is the rule they share: outside a
playback run there is no dropped-frame figure, and with an empty cache there is no cache reading --
a zero shown out of context reads as a measurement, which would be a different claim.

Messages have two lifetimes. A notice -- a rejected command, an export that landed, a cancellation
-- clears itself after five seconds. Work that is still running -- `Saving…`, `Opening…`, range
export progress -- persists until it is replaced, because a message that vanished while the work
continued would be a lie. A notice takes precedence while it lasts; the persistent message is what
is left when it expires.

Nothing is painted over the viewer canvas. The readiness chip and the failure banner that used to
be drawn on top of the pixels are cells in this strip; a viewer canvas shows the composition, and
everything else reports here.

### Nodes footer (task NODES-1)

Left to right: a zoom dropdown (`nodeZoomDropdown`, the same Fit/25/50/100/200/400 items the
Viewer's own dropdown offers), a grid-snapping switch (`nodeSnapSwitch`, a `KSwitch`), and a link
style dropdown (`nodeLinkStyleDropdown`, a `KDropdown` offering Spline/Straight/Angled) -- the same
two settings View's own Grid Snapping toggle and Link Style submenu offer, so the footer and the
header menu can never show a stale value for the other. Right-aligned: the selection readout
(`nodeSelectionReadout`), `Muted` `UiSmall`, reading "N nodes". Unlike the Viewer's own footer, this
one is ordinary child widgets in a `QHBoxLayout` rather than one surface the editor paints itself --
there is no per-frame readout here that needs a single paint pass to stay in sync.

### Assets footer (task ASSETS-1)

The footer places New Composition, disabled New Folder, and disabled Import on the left, with
Delete aligned to the right. New Folder explains `Folders arrive with asset organisation`; Import
explains `Image and sequence import arrives with the media pipeline`. The panel body is a
two-column tree whose Kind column reads `Composition`, with a search field above it.

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
| `Ui` | DejaVu Sans | `12` | 500 | The default interface text |
| `UiSmall` | DejaVu Sans | `10.5` | 500 | Panel headers: uppercase, `+0.07em` tracking |
| `Value` | Geist Mono | `11.5` | 500 | Every numeric, unit, hex, and timecode surface |
| `Title` | DejaVu Sans | `13` | 600 | Dialog and section titles |

Sizes are in design pixels. `Value` is monospaced so a column of numbers stays aligned and a
changing digit does not reflow the text beside it.

Static faces are shipped rather than the upstream variable fonts (see Font Packaging And Loading
below). How a role asks for a face depends on how the upstream family is cut. The interface family's
three faces all declare the one family name `DejaVu Sans` and differ by style (`Book` / `Bold` /
`Oblique`), so an interface role names that family and the role's own weight picks the face: `Ui`
and `UiSmall` at 500 resolve to Book, `Title` at 600 resolves to Bold. The monospaced family is cut
the other way -- its Medium face registers as its own family `Geist Mono Medium` -- so the `Value`
role asks for that exact face first, the base family second, and the platform family last.

`Ui` and `UiSmall` are `12` and `10.5` rather than the `12.5` and `11` they were under the previous
interface face: DejaVu Sans renders visibly larger at an equal pixel size, and the earlier numbers
read oversized in dense chrome once the family changed.

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

DejaVu Sans is Bloom's primary interface typeface. Geist Mono is Bloom's monospaced
typeface.

Use DejaVu Sans for:

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

Initial interface faces are Book, Bold, and Oblique for DejaVu Sans, and Regular and Medium for
Geist Mono. Book carries the `Ui`/`UiSmall` roles and Bold carries `Title`; the Oblique face is
shipped with the family but no implemented component asks for an italic role yet.

The shipped set is the static TTFs, not an upstream variable font: it is exactly five faces, and a
static face resolves the same way on every supported Qt platform without depending on the platform
font engine's named-instance handling. The vendored assets, their pinned releases, their archive
digests, and a digest for every file are recorded in
`src/ui/kit/third_party/dejavu-sans/provenance.md` and
`src/ui/kit/third_party/geist-mono/provenance.md`, and inventoried in the repository's
`THIRD_PARTY_NOTICES.md`.

Official sources:

- [DejaVu fonts](https://github.com/dejavu-fonts/dejavu-fonts)
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

The layer-stack column carries Bold 16 px visibility, solo and lock glyphs with distinct on/off
states, a label swatch and name, and a Compact Blending dropdown. Audio and Parent are hidden:

| Row control | Object name | State |
| --- | --- | --- |
| Blending | `layerBlendingDropdown` | Enabled. Offers every implemented blend mode, in the one shared order, starting at the layer's own authored mode. Authors the layer the row DRAWS, never the selection |
| Parent | `layerParentDropdown` | Hidden until parenting exists |

A disabled placeholder always states its reason in its tooltip rather than merely looking
unresponsive. The Properties panel's own Blending row (`blendModeEditor`) offers the same vocabulary in
the same order, as does a layer node card's (`nodeBlendModeDropdown`): one set of words, one order,
one write path.

### Timeline property rows and key summaries

Layer, group-heading and parameter rows share the 32 px `ControlRoomy` pitch and one vertical
scroll offset. A 16 px Phosphor CaretRight/CaretDown beside the layer name discloses expansion.
Group headings read TRANSFORM, APPEARANCE and SOURCE. Parameter names are indented beneath the
layer name; the 64 px name column, shared diamond, and inline value column stay on the left of the
lane divider. The layer column is 396 px, widened by one `ControlRoomy` token so paired
values fit their units. The child-row left inset is 112 px, derived from the layer control table;
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
| Port socket | 8px circle in its schema kind's `Socket*` token; inputs left, outputs right; one expanded row per port |
| Ordered multi-input | Each Merge's ordered image port: a vertical pill in the kind's `Socket*` token, `kStackSlotPitch` long per ordered slot, divided by `Surface` hairlines. A `Muted` caret marks the position under the pointer during a drag, and the pill highlights compatible image drops; the card shows its input count |
| Card eyebrow | A layer card's `UiSmall`/`Faint` "Layer" line above its own name, because the name is the layer's |
| In-card vocabulary row | A parameter whose value is a closed vocabulary rather than a number takes a Compact `KDropdown` in the card's control column, sized and stretched exactly as a `KValueField` row is. Today's one instance is a layer's Blending. It carries no keyframe indicator, because the value is not animatable |
| Socket hover/hit | Hover grows the circle to 12px; its hit radius is 16px (12px beyond the resting 4px radius); tooltip is `<port name> · <kind>` |
| Selected node | 2px inset Accent outline, painted above the card/header surfaces |
| Primary/active node | 2px inset Foreground outline; primary identity still belongs to the session selection |
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

Bars span the layer's half-open range, inset vertically by `Spacing::XS`, with a one-pixel inner
border from `hoverFillFor(barColor)` and shallow `Elevation::TimelineBar` shadow. Both ends have
trim grips; snapping shows a vertical guide. Row drag shows an Accent insertion rule. Toggle
states use `Visible`/`Hidden`, `Check` for solo and `Locked`/`Unlocked`, rendered in the curated
Bold weight at `Size::IconMedium` (16 px). Their source and hashes are in the Phosphor provenance
record; other icon weights and meanings are unchanged.

### Nested Merge Rows

The timeline draws a direct nested Merge as one collapsed row in `DataComposition`, using the
Merge display name. It has no expansion affordance, solo or lock control; enabled is editable.
The Properties input list shows plain images with disabled Normal blend and 100% opacity fields.
New automation object names: `mergeInputsPanel`, `mergeInputRow`, `mergeInputName`,
`mergeInputBlendMode`, and `mergeInputOpacity`. Existing object names are unchanged.
