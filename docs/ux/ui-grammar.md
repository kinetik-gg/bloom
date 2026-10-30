# UI grammar

Status: accepted

Updated: 2026-10-30

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
`KIconButton`, `KIconToggle`, `KCheckBox`, `KSwitch`, `KValueField`, `KSlider`, `KRangeSelector`,
`KTextField` (and its `KLineEdit` and `KSearchField` forms), `KTextArea`, `KFieldFrame`, `KSection`,
`KRadioGroup`, or `KColorSwatchRow`. Text uses TypeRole-bound `KLabel`.
Read-only progress uses `KProgressBar` (see "Import Progress Dialog" below). A read-only picture
uses `KThumbnail`: the `ImageThumbnail` square on `SurfaceSunken` (`Cell::Card`), or the Assets
summary strip's 64 × 40 Field well (`Cell::Strip`), showing a worker-decoded proxy or a glyph fitted
without changing its aspect ratio; it never loads anything.
The panel switcher is the icon variant of **KDropdown**, with the same height, styling,
interaction and popup. Menus are created only through `kit::makeMenu`. A popup is a
`KPopupWindow` and a list in one is a `KPopupList` (dropdowns, the Add search, the colour picker);
no surface positions, shadows or dismisses a popup of its own.
Dialogs are `KDialog` and its forms: `kit::confirm`, `kit::inform`, `kit::promptText`,
`kit::promptRange`, `kit::chooseFile` and the `KTaskDialog` progress-then-summary modal (see
"Dialogs" below). The contextual bar under the menu bar is the `ContextBar`; the one active tool
lives in the `ToolController`.
Constructing QToolButton, QPushButton, QComboBox, QLineEdit, QCheckBox, QSlider,
QLabel, QMenu, QPlainTextEdit or QTextEdit outside the kit is a quality violation, and so is using
QMessageBox, QInputDialog, QFileDialog, QColorDialog, QFontDialog, QDialogButtonBox, QSpinBox or
QDoubleSpinBox anywhere outside the kit.

## Escape confirms

**Escape confirms everywhere.** This is the one statement of the rule; every other section and
document refers here. Return, Escape, Tab and focus loss all commit typing and finish an inline
edit; a pointer release or key release finishes a scrub or step; in the Viewer, Escape keeps what
already exists (a shape, mask, path or text mid-gesture commits at its current state, a gizmo
drag keeps the change made so far, a region-of-interest drag keeps its region) and returns to
Select. Nothing is thrown away by Escape: discarding is Undo, one step per completed gesture.
The only text that is refused is text that does not parse, carries another unit or falls outside
the parameter's range, and that is refused by every key alike (see "Live Value Editing").

## Metric grid

All dimensions resolve through tokens. One design pixel is one Qt logical pixel.

| Token | Logical pixels | Contract |
| --- | --- | --- |
| `Size::Control` | 26 | Every header, footer and row control: buttons, icon buttons, toggles, dropdowns, value and text fields, segmented bars |
| `Spacing::ControlInset` | 1 | The air between a control's widget and its painted box on every side: every control paints a `ToggleCell`-tall (24) box |
| `Size::HeaderRow` | 32 | Panel header |
| `Size::FooterRow` | 32 | Optional panel footer |
| `Size::ListRow` | 32 | The one list pitch: timeline, Assets, Jobs and kit lists |
| `Size::PropertyRow` | 28 | Property pitch |
| `Size::IconMedium` | 16 | Chrome glyph box |
| `Size::IconLarge` | 20 | Control and toggle glyph box |
| `Size::ToggleCell` | 24 | Toggle column pitch |
| `Size::DropdownWidth` | 100 | List dropdown column |
| `Size::PropertiesDropdownWidth` | 108 | Property dropdown |
| `Size::DropdownWidthWide` | 120 | Header selectors |
| `Size::DropdownWidthExpanded` | 240 | Header selector cap |
| `Spacing::XXS / XS / S / M / L / XL / XXL` | 2 / 4 / 8 / 12 / 16 / 24 / 32 | Gutters |
| `Spacing::ChromePadding / ChromeGap` | 3 / 4 | Equal chrome inset on all edges / every item gap; controls have no private side margins |
| `Spacing::Gutter` | 6 | Panel separation |

Editor literals in fixed extents, QSize, pixel multiplication or geometry arithmetic are
quality violations. Existing specialized canvas tokens remain valid; new dimensions require
an owned semantic token, never a local pixel constant.

## One control look, one control height

Every clickable control that is not text entry wears ONE look, the Secondary recipe of
`kit::buttonColors()` (the Viewer footer's RAM Preview button is the reference): a `SurfaceRaised`
plate, one surface rung up under the pointer and one down while pressed, a hairline border by state,
`Foreground` ink for labels and glyphs alike, an `Accent` border under keyboard focus, and the
`Accent` plate with `OnAccent` ink when checked (loop on, ROI on, a linked scale, an open sampler).
It covers `KButton` Secondary, every `KIconButton` (the transport, footer toggles, View options,
panel header maximize and full screen, the Timeline and Assets footer buttons, dialog preset
buttons), `KIconToggle`, every `KDropdown` closed field (the panel switcher, the composition and
layer pickers, Fit, Resolution, Channel, Display / View, the Timeline row's Blending, Parent and
Matte, Properties and node-card dropdowns, dialog dropdowns), the frame of a square `KColorChip`
(the colour itself is the swatch inside it), the `KSection` header buttons, and the
`KRadioGroup` segmented bar, whose segments share the plate, hover rung and Selected plate.

The exceptions are deliberate and closed:

| Exception | Why |
| --- | --- |
| Panel menu titles (`KMenuButton`: View, Select, Add, the Timeline and Nodes menus) | Menus, like a menu bar's titles, not buttons: flat text with the `Accent` hover bar |
| Text entry (`KValueField`, `KTextField`/`KLineEdit`, `KSearchField`, `KTextArea`, the Viewer frame field) | The one text-entry recipe, `kit::fieldColors()`: a `Field` well, so an editable value never reads as a button. A field standing in a chrome row or on a node card keeps a resting `Border` (`KValueField::setOutlinedAtRest`); a property cell (Properties, Timeline rows) rests borderless. A refused entry wears `Error` over every other state |
| Timeline layer switches (eye, audio, solo, lock, shy) | The glyph is the state (an eye that is there or not), so a switch never takes the Accent plate; its cell is the Secondary plate. The Label cell is a `kit::KSwatchButton`: the Secondary plate carrying the label colour as an `IconSmall` square with its edge-ink hairline, never tinted |
| Ghost | Only where a control deliberately has no resting surface: disclosure carets (layer and property disclosure, the RGBA and multiline expanders) and the window title bar buttons |

There is no Recessed recipe and no `ControlSurface` token any more: both existed only for the
header icon buttons and dropdown fields this look replaced.

Every control in a row is one `Size::Control` (26) tall at every device pixel ratio and paints
its body in `kit::controlBody()`: `controlBounds()`, `Spacing::ControlInset` in on every side, so
every painted body is `ToggleCell` (24) tall and shares its row's vertical centre. Icon buttons
and toggles are `Control` squares, the context bar's tools included. A dense strip whose geometry
is a fixed cell pitch (the timeline's layer switches) uses `ToggleCell` cells instead, which are already
exactly that body; that is the one compact token, and no call site sizes a control of its own.
`KButton`'s Compact size is the same height with tighter padding, and `KDropdown::ControlSize::Dense`
likewise. `control_geometry_tests.cpp` walks every chrome row, panel header and row of the
main-window fixture (the context bar, Viewer, Timeline, Nodes, Assets, Properties) and holds both rules, and checks
that every kind's resting colours come from the one recipe.

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
layout and one measure-only overflow algorithm, with hysteresis and a stable minimum. A row is
either a menu-collapse row (menus fold into a "…" button once the row, with any elidable dropdown
at its `setElideFloor` width, no longer fits) or a priority row: controls in Leading, Centre
(`beginCentre`) and Trailing groups separated by `Spacing::ChromeGroupGap` (12), the Centre group
centred while it fits, and controls that carry a collapse rank (`setCollapse`) or a compact form
(`setCompactForm`) giving way in rank order into an icon overflow button whose menu offers each
collapsed control as its own action or submenu. The row publishes `collapseThreshold` (the width
everything needs), `collapsed` and `collapseLevel`; a control that changes width re-lays the row
out. A panel declares `contentMinimumHeight` when its content cannot shrink to nothing; the panel
minimum is header + footer + that floor.
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
promoting the Properties layout into the kit. Timeline and properties use these shared row
layouts. Tables with sortable columns (Assets, Jobs) use the kit theme's item-view and header
styling at the `ListRow` pitch, with no per-row widgets.

Painting outside the kit is limited to viewer canvas, node scene items, and timeline
ruler, lanes, graph editor, work area and navigator. Other surfaces compose kit widgets.

## Export panel

The Export panel (`exportEditor`, editor id `bloom.export`) is the After Effects Render Queue in
Bloom's components. The panel header (`exportHeader`) holds the editor picker and **Add
composition** (`exportAddCompositionButton`, Secondary, shown once the list has a job); on the right
**Stop All** (`exportStopAllButton`, Danger, shown only while something renders) and **Start All**
(`exportStartAllButton`), then the maximize button. Start All is the only Primary of the panel and
is Primary and enabled only while a checked job is ready; otherwise it is a disabled Secondary with
its reason. The empty state's own Primary is **Add composition** (`exportAddActiveButton`) under
"Nothing to render yet." and "Or drag compositions here from Assets."

**Current Render** (`exportCurrentRender`) is a strip on the Surface fill above the list, folded by
default (a per-user setting): caret, the UiSmallStrong title and one Muted line, "Rendering SC020 ·
frame 404 of 2156 · 0:28 elapsed · about 2 min left" or "Idle" (with the platform's reason where
export cannot run). Opened, it adds a `KProgressBar` no wider than `Size::DialogWideWidth`.

The render list (`exportRenderList`) is the Assets row system: a `QTreeWidget` with real column
headers, rows of `Size::AssetRow`, one `Size::AssetIndent` per level (the caret column), text one
header padding in from its cell, and the row background painted across the viewport. A job and its
lines share one band and the bands alternate, so each job reads as a block. Columns, sized to their
content: **Render** (the row's caret, then a `KCheckBox` centred in its cell: include in Start All),
**#**, **Composition** (Foreground), **Status**, **Started**, **Render Time** (Muted), then an
untitled cell that takes the slack and holds the row's ⋯ button (`exportJobMenuButton`), so no
column stretches across a wide panel. Below the panel's width for them, Started and then Render
Time hide, and Composition and Status narrow. Status words are plain: Queued, Needs output, Needs
attention, Waiting for your OK, Getting ready, Rendering 25%, Paused while you work in the Viewer,
Checking the file, Done, Done, with a note, Failed, Stopped. Every status is Muted ink; only Failed
uses Error. While a job runs, a thin `KProgressBar` sits after its words in the Status cell. There
is no label colour and no switch. A job row drags to a new place (an Accent line shows where), and
compositions dropped from Assets become jobs at that line, as one undo entry.

An open job shows one line per setting, each a spanned child row with its own caret:

- `Render Settings:` (`exportRenderSettingsLine`): ▾ (`exportRangePicks`: Work area, Whole
  composition, Current frame) and the range as a `KButton::Variant::Link`
  (`exportRenderSettingsLink`), which opens **Render Settings**.
- `Output Module:` (`exportOutputModuleLine`), one per output: ▾ (`exportPresetPicks`: the formats,
  video first, then image sequences), the preset as a Link (`exportOutputModuleLink`) that opens
  **Output Module**, **+** and **−** (`exportAddOutputButton`, `exportRemoveOutputButton`; − is
  disabled with its reason on the last output), and `Output To:` with ▾ (`exportDestinationPicks`:
  Default name and recent folders) and the resolved destination as a Link (`exportOutputToLink`)
  that opens `kit::chooseFile` in Save mode. On a narrow panel Output To folds onto a second row
  under its own right-aligned label.

The line labels are right-aligned in one shared column, so every value starts at one x; the
module links share one width, so +, − and Output To line up across outputs. A line's caret opens
one Muted summary (`exportSummaryLine`) under its values: the frames and timecode, the first and
last file, the estimated size and free space, and what blocks it. A Link is plate-less Accent text,
AccentHover and underlined under the pointer, Muted when disabled, with the focus border only on
keyboard focus; the ▾ quick picks are plate-less carets.

**Render Settings** (`exportRenderSettingsDialog`) and **Output Module**
(`exportOutputModuleDialog`) are `KDialog`s of `Width::Wide` built from `KSection` groups holding
two `KForm` columns side by side, as After Effects groups them; fields keep their content width.
Render Settings has one group, `Composition “SC020”`: Range (Work area, Whole composition, Custom,
One frame) on the left; Start, End (frame `KValueField`s with their timecode for Custom and One
frame, readouts otherwise) and Duration on the right. Output Module has the preset's own group
(`“ProRes 422 HQ”`: Format, If a file is there | Post-render, Cached frames), **Quality** (Profile,
Compression, Look, Audio | Color space, or Display and View; only what applies to the format) and,
for image sequences, **Numbering** (Start number or Use the frame numbers | Digits). OK writes
every change as one undoable edit; Cancel writes nothing.

When an output's file exists under Ask first, the job reads **Waiting for your OK**, and Start
(in the ⋯ menu) or Start All asks once with `kit::confirm` (`exportReplaceConfirm`): "Replace
SC020.mov?" with Replace (Danger), Choose another name and Cancel (the default). The ⋯ menu holds
the verbs that fit the state (Start; Pause and Stop; Resume and Stop; Stop; Render again),
**Details…**, Duplicate, Remove, Move up, Move down, Copy as bloom-cli command and Copy as Python.
**Details…** (`exportJobDetailsDialog`) holds the result sentence, the plain verification lines, a
Show in folder per saved output and the log, with **Copy details** for bug reports; raw codes and
the host's own wording appear only in what Copy details copies.

## Status bar, context bar and tools

The existing native window frame and application menu bar remain unchanged. No branding,
workspace tabs or custom title bar are part of this grammar revision.

`WindowStatusBar` is one `Control`-height row of labelled segments, left to right, separated by
`Border` hairlines (`StatusSeparatorHeight`) and `Spacing::M` gaps. Every value has a short visible
label in the UI font (`UiSmall`, Muted ink) and the value itself in the mono face (`TypeRole::Value`,
Geist Mono) in a cell whose minimum width is that of its widest text, so a changing number never
moves its neighbours. No value appears without its label, and no bare number. Plain words only: the
render path reads GPU or CPU, never "GPU-prepared / CPU-painted", and caches are never "Ops". The
segments:

- **Viewer**: Render (GPU or CPU, tinted Ok, Warn or Error by the colour state), Exposure, Channel,
  Dropped (a dash outside a playback run), and Decode while the composition reads video. The full
  colour-state and readiness sentences ("Qualified · Bloom Neutral", "Shown EV 0 · RGBA · Current")
  are the tooltips. A previous display shows Muted values.
- **Activity**: the transient or persistent notice, else RAM-preview progress (a `KProgressBar` and
  "Caching frames for RAM preview · 12 of 386"), else a preview that is rendering, failed,
  unsupported or cancelled, else the Faint placeholder "Idle". The compile-failure Details button
  is at the end of this segment. It takes the free width and elides.
- **Pixel** (`windowStatusBarProbe`): the Viewer's probe readout; shown only while it has one.
- **RAM**: used/budget of the preview and operation caches together ("2.5/7.7 GB") and Cache, the
  preview cache's frame count ("385 f"). The tooltip keeps the budget explanation (Bloom reserves
  `max(8 GiB, 40%)` of memory for the rest of the machine; default caches never exceed half of
  physical memory) and the hit and miss counters of each cache; it explains where a number came
  from and never becomes a second place to read the numbers themselves.
- **Disk cache**: "12% hit · 536.3 MB", "off", or a dash before the first lookup; hits, misses and
  entries in the tooltip.
- **MCP**: a small text segment, "MCP :6767" in the mono face and Muted ink, only while the server
  runs ("MCP failed" in Error ink after a failure); there is no button. Clicking it, or Return or
  Space with it focused, opens the MCP Server panel. Nothing shows while the server is stopped.
- **Version**: Faint, at the far right.

A segment that has nothing to report keeps its place with a dash so the layout does not jump. In a
narrow window whole segments collapse by priority, never their labels: Version, MCP, Disk cache,
Pixel, then RAM; Activity keeps `StatusActivityMinWidth`. `KSurface` owns neutral surface
backgrounds.

**The context bar** ([ADR 0031](../decisions/0031-contextual-tool-bar.md)) is the second fixed
surface: one `ContextBarHeight` (32) row directly under the menu bar, a `Control` row inside
`ChromePadding` vertically and one `Spacing::Gutter` at its left and right edges (the outer margin
the workspace panels use, so the bar's content lines up with them), on `Surface` with one `Border`
hairline below it. It is a row of the window's
central column, not a `QToolBar`, so it cannot be docked, floated or moved and the native macOS
menu bar is untouched; Window > Context Bar hides it, and that choice persists with the workspace
UI state (`workspace/context-bar-visible`), never in the project. While the read-only placeholder
is showing, the bar is disabled. It is a kit priority row (`EditorArea::buildChromeRow`) in three
regions: tools and their options on the left, the selection actions in the centre, and the
Workspace field on the right:

- **Tools**, on the left: eleven tools in eight `KIconToggle` slots in three groups separated by a
  `ChromeGroupGap`: navigation (Select V, Hand H, Zoom Z), creation (the shape slot, Line, Pen P,
  Text T) and the Cryptomatte eyedropper. Rectangle R, Ellipse E, Polygon and Star share one slot
  that shows the last shape chosen and carries a small corner mark; a press and hold or a
  right-click opens its flyout menu of all four (After Effects' shape slot). Each slot wears the
  one control look with the Selected plate when its tool is active (the tool-column Ghost
  exception is gone). A tooltip is the tool's name and shortcut ("Select (V)"), then how to use
  it; the shape slot adds how to reach the other shapes. The tools never collapse.
- **Tool options**, after the tools: only options backed by real session or document state, and
  only for the active tool. Select and the Pen show the anchor handle-mode strip (below); the shape
  tools, Line and the Pen show Fill (a check box and a square colour chip), Stroke (the same) and
  the stroke width; Text shows the font, size and alignment. With a shape or text layer selected
  the options are that layer's, edited with the same parameter commands Properties uses (one undo
  step per edit, a key at the playhead on an animated value); with none selected they are the style
  new shapes and new text start with, which is session state in the `ToolController` and authors
  nothing until something is drawn. Hand, Zoom and the eyedropper have no options. There is no
  transform-gizmo mode and no snapping in the Viewer, so Select has no slot for them.
- **Selection actions**, in the centre of the row: Align (left edges, horizontal centres, right
  edges; top edges, vertical middles, bottom edges), Distribute (horizontally, vertically) and
  an "Align To" dropdown (Composition, Selection, Key Object). Align To starts on Composition
  (`ToolController`'s session default). Key Object is the primary selected
  layer, named in the dropdown ("Key Object (Title)") and its tooltip. These are the same actions as
  Composition > Align.

- **Workspace**, against the right edge: a `KDropdown` styled like Align To (`DropdownWidthWorkspace`)
  with a "Workspace" label. Its list is the built-in presets (Default, Node, Export, Animation), a
  separator, the artist's saved presets, a separator, then "Save current layout as…" (a
  `kit::promptText` for the name) and, while a saved preset is active, "Update ‘name’" and
  "Delete ‘name’…" (a `kit::confirm`). The field reads the active preset, "Node (edited)" after the
  arrangement changed by hand, or "Custom".
  It is the last group to fold into the overflow menu, where it is a Workspace submenu.

Slots are stable within a tool: an option or action that cannot apply stays where it is, disabled,
with the reason in its tooltip (a line has no fill, fewer than three layers to
distribute, one layer aligned to its own selection, a Viewer frame that is not yet current). The one
exception (owner, 2026-10-30) is the handle-mode strip: with no path or mask selected, or with a
tool that does not edit points, it is hidden rather than disabled; with a path selected but no
anchors it is shown disabled. A
narrow window collapses groups right to left into one overflow button (`Overflow`, a double caret)
at the end of the tool options: Distribute first, then Align To, then Align, then the fill/stroke
and text options, then the handle modes. The menu offers each collapsed group whole: the action
groups as submenus, the option groups as the same controls bound to the same state.

Bar buttons, toggles, segmented controls, check boxes, colour chips and the overflow button are
`Qt::NoFocus`: clicking them never takes keyboard focus from the Viewer. A field (the stroke width,
the font, the size, Align To) takes focus while it is edited, commits on Enter or Escape like every
field, and then hands focus back to the Viewer that last had it. Every control has an accessible
name.

Hand pans; Zoom clicks zoom about the pointer, with Alt-click reversing the direction. Middle-button
pan and keyboard zoom remain available. Choosing a tool alone never authors a node.

Creation tools are one-shot. Once a shape, mask or text object is committed (drag release for
Rectangle, Ellipse, Polygon, Star and Line; closing or finishing a Pen path; committing or clicking
away from new text) the Viewer returns to Select, for a new layer and for a new mask on the selected
layer alike. The next click outside therefore selects or deselects and never creates another
object; drawing again means choosing the tool again. Each created object is exactly one undo entry.

Escape confirms (see "Escape confirms") in the Viewer: it keeps what already exists, never throws work away,
and then returns to Select. Discarding is Undo, one step per created or edited object.

- A shape or mask mid-drag commits at its current size.
- The Pen finishes with the anchors placed so far as an open path (two or more anchors). A mask is
  an area, so a Pen mask always closes, and needs at least three anchors.
- Text keeps what was typed, exactly like a click-away.
- Editing existing text, a Path layer's anchors or handles, or a mask's anchors keeps the edit made
  so far as one command.
- Empty or zero-size work creates nothing: emptied new text (withdrawn, so neither Undo nor Redo
  holds it), a Pen draft below its minimum, or a press-release inside the drag distance.

A press-release inside the drag distance with a primitive tool is a zero-size gesture: it creates
nothing and leaves the tool active, so a slip of the mouse never authors an object. Escape at that
point also creates nothing and returns to Select. A move, rotate, scale or anchor-point gizmo drag
keeps the change made so far as one command, and a region-of-interest drag keeps the region drawn so
far. Only a drag whose frozen mapping was invalidated (a format, viewport or time change) is
cancelled, because there is nothing valid left to confirm.

Rectangle, Ellipse, Polygon, Star and Line draw a live Accent outline on press-drag. Shift constrains
rectangles and ellipses to equal sides, keeps polygons and stars regular, or snaps a line to 45-degree steps; Alt draws from the press point
as centre. Polygon and Star start with five points. Release creates and selects the layer
with one undo entry. The selection gizmo appears with the evaluated frame.

Pen clicks append straight anchors; dragging an anchor during creation pulls Mirrored handles.
Clicking the first anchor closes a path with at least three anchors; Enter or Escape finishes an
open path with at least two.

**Editing a confirmed path.** A selected Pen shape path and a selected mask are edited by the same
implementation, with the same gestures, and draw the same screen-sized anchor squares (Accent when
selected) and handle circles. Every action previews live in the Viewer through a session-only
override, changes the document once, on release, as one undo step (a mask path that is keyed gets
a key at the playhead), and follows the Escape-confirms rule above.

- Select: click an anchor to select it (a click on one anchor of a multi-selection selects just
  that one); Shift-click adds or removes it; a marquee dragged from empty canvas selects the
  anchors inside it (Shift adds); a plain click on empty canvas still deselects. Dragging a
  selected anchor moves every selected anchor with its handles.
- Handles follow the anchor's mode. Corner has none; Mirrored keeps them collinear and equal, so
  dragging one moves the other symmetrically; Aligned keeps them collinear, so dragging one
  rotates the other at its own length; Free leaves the other alone; Auto derives both from the
  neighbouring anchors (Catmull-Rom) and recomputes them whenever a neighbour moves, and dragging an
  Auto handle turns the anchor Aligned.
- Alt-click an anchor toggles Corner and its previous curved mode (Mirrored the first time).
  Alt-drag from an anchor pulls out new Mirrored handles. Alt-drag a handle moves it alone and
  leaves the anchor Free.
- The anchor mode strip -- a kit segmented control with Corner, Mirrored, Aligned, Free and Auto --
  is the Select and Pen tools' option in the context bar (`contextBarAnchorMode`), and acts on the
  anchors selected in the Viewer that last had focus; it is hidden while no path or mask is selected
  and disabled while none of its anchors is. It shows the selection's mode, or no option for a mixed selection,
  and choosing one converts every selected anchor. A curved mode on a Corner pulls handles sized
  from its neighbours; Mirrored and Aligned straighten existing handles onto their common tangent.
- Right-click on an anchor or the selection offers the same five modes, Delete Point(s) and, for a
  shape path, Open Path / Close Path.
- With the Pen, clicking a segment adds an anchor there by splitting the curve, so the shape is
  unchanged; clicking an anchor removes it; clicking an end of an open shape path continues drawing
  from that end (clicking the first anchor then closes it, Enter or Escape finishes it as one edit).
  A Pen edit selects its result and returns the Viewer to Select.
- Delete or Backspace removes the selected anchors. A path keeps a valid minimum: a mask (always
  closed) keeps three anchors, a shape path two (a closed shape path left with two opens); a
  removal below it is refused and leaves the path unchanged.

There is no separate Convert Point tool: Alt on the Select tool and the Pen does that job (Alt-click
converts, Alt-drag pulls or breaks handles), the mode strip and the context menu cover explicit
conversion, and one fewer tool keeps the tool set small. The anchor mode is document state;
anchor selection is Viewer gesture state, so Properties shows no per-anchor row. Text clicks create a layer and enter canvas text editing
with the placeholder selected; the typed text folds into that creation, so a new text layer is one
undo entry. Properties mirrors the live text.

All glyphs use the DPR-exact SVG renderer and kit state tints. Rectangle, Pen and Text retain their
Phosphor assets; the four added shape glyphs are Bloom-authored SVG paths.

| Tool icon | Glyph | Shortcut |
| --- | --- | --- |
| Select | cursor | V |
| Hand | hand | H |
| Zoom | magnifying-glass | Z |
| Rectangle | rectangle | R |
| Ellipse | ellipse | E |
| Polygon | pentagon | Bar only (the shape slot's flyout) |
| Star | five-point star | Bar only (the shape slot's flyout) |
| Line | diagonal line | Bar only |
| Pen | pen-nib | P |
| Text | text-t | T |
| Cryptomatte | eyedropper | Bar only, or Properties' Pick in Viewer |

The viewer surrounds its composition with `Color::Canvas` (`#131313`) and uses
`Color::CompositionFrame` (`#454545`) at `kCompositionFrameWidth` (1 logical pixel) for the
frame outline. `ViewerWorkPadding` reserves breathing room around the frame on every side; the
canvas has no tool column (ADR 0031). Image fitting, picking, selection overlays and panning all
use this same padded rectangle. View > Background (the surround) and the Transparency Grid remain
presentation preferences.

### Viewer header and footer

The header holds the panel switcher, then two selectors that say what they select: the
composition shown (`viewerCompositionSelector`) and the selected layer (`viewerObjectSelector`),
each item's tooltip naming its role ("Composition shown in the Viewer: …", "Selected layer: …").
Empty, they read "No composition" and "No layers" (or "No layer selected" when a composition has
layers and none is selected), disabled and muted. They elide down to `DropdownWidth` before the
View, Select and Add menus collapse into the "…" overflow. The composition commands live in the
application's Composition menu only.

The footer (`viewerFooter`) is a priority row of three groups:

| Group | Controls |
| --- | --- |
| Leading (view and display) | Channel (`ViewerChannelWidth`), Transparency Grid toggle, ROI toggle, Clear ROI, exposure (EV) and gamma (`ViewerAdjustWidth`), Display / View (its items' widest, `DropdownWidth`..`ViewerDisplayViewWidth`), Look |
| Centre (time) | Go to start, step back, play/pause, step forward, go to end, loop, the frame field, centred in the row while there is room and never stranded from the transport |
| Trailing (framing and quality) | Zoom (`ViewerZoomWidth` floor), Resolution (`ViewerResolutionWidth`), RAM Preview |

As the footer narrows, controls leave the row in this order, each offered in the View options
overflow (`viewerFooterOverflowButton`, `SlidersHorizontal`, menu `viewerFooterOverflowMenu`) under
its full label: Look, Display / View, exposure and gamma (as labelled fields plus Reset Exposure
and Gamma), ROI and Clear ROI, Transparency Grid, Channel; then Resolution compresses to Auto / Full / ½ / ¼ at
`ViewerResolutionCompactWidth`; then RAM Preview, go to start/end, then Zoom and Resolution; and
last of all the frame field drops its duration, keeping the current value in a box that fits any
timecode. Step back, play, step forward, loop and the frame field never collapse. No control is
clipped at any width down to the panel minimum.

Every footer control is one `Control` slot painting a `ToggleCell` body in the one control look.
The Transparency Grid, ROI, Look and loop toggles and the transport are `KIconButton`s with
`IconRole::Control` glyphs in `Foreground` ink; a checked toggle shows the one Selected plate. The Transparency Grid toggle carries the Phosphor
`square-half` glyph (the pinned set has no checkerboard) and is View > Transparency Grid's own
action (`viewerTransparencyGridAction`). The ROI toggle carries the Phosphor `selection` glyph;
Clear ROI shows only while a region exists. RAM Preview is a labelled Secondary `KButton` toggle whose label reads the
controller's progress (Caching n%, RAM Cached). Exposure and gamma show an `NonDefaultMark` Accent
dot while they differ from 0 / 1 and reset by a double-click on their label or from their context
menu. Display / View shows the view name (qualified by its display only when several displays
share it); the display, view and colour space are in its tooltip and the View menu. Background is
chosen from View; the retired `viewerBackgroundDropdown` and `viewerResolutionReadout` identities
remain for automation.

The frame field (`viewerTimeReadout`) closes the transport cluster, one `ChromeGap` after loop like
every transport button. It is a text-entry `Field` box one control tall (`kit::KFieldFrame`) showing the current frame in
the `Value` role and the composition's duration after it in muted ink: `120 / 240` in frames, or
`00:00:05:00 / 00:00:10:00` in timecode. Its width is fixed from the composition's longest value
(its last frame's label and the duration) and changes only with the composition or the reading, so
nothing moves during playback; the current value right-aligns in its column. A click opens the kit
line edit in the same box; Return, Escape and clicking away all commit through the kit inline-edit
policy. Text that is neither a frame nor a timecode is refused: Return keeps the editor open with an
`Error` border and the reason, and Escape or clicking away closes it on the current frame. Its context menu switches Frames / Timecode, and its tooltip names the reading and how to
change it. With no composition it shows `00:00:00:00` (or `0`) disabled in the same box, with no
duration.

### Viewer canvas and empty state

The canvas has two backdrops, as in After Effects, and they are separate questions:

- **The surround** is the content area outside the composition frame. View > Background chooses
  it: Solid (the default, the `Canvas` token), Grey (`CanvasGrey`), Black (`CanvasBlack`) or White
  (`CanvasWhite`). It never draws inside the frame. Stored as `viewer/background` by name.
- **Inside the frame**, behind transparent and partly transparent pixels, the Viewer shows the
  composition's own Background Colour from Composition Settings, opaque. View > Transparency Grid
  (off by default, `viewer/transparency-grid`) replaces it with the Viewer checker
  (`ViewerCheckerLight` / `ViewerCheckerDark` on the `ViewerChecker` grid from the content area's
  top-left).

Both are display-referred and the image composites over them in display space (the authored
Background Colour is a display value; see `docs/architecture/color-management.md`), identically on
the CPU path and the GPU present. They are presentation only: never process or export alpha, never
image content in a cache. Editing the Background Colour is one non-render-affecting undoable step,
so the Viewer re-presents the frame it already has instead of re-evaluating. A stored pre-split
`Checkerboard` background reads as Solid with the Transparency Grid on (unless a grid value was
stored since).

With no composition the canvas shows the surround under the `viewerEmptyState` invitation: a Title
"No composition", a Muted hint that dropped media files are imported into Assets, and the Primary
`viewerNewCompositionButton`, which runs the application's New Composition command. A composition
with no layers shows just its frame and background, with no invitation. The Viewer's own minimum is
`ViewerMinHeight`; hosted, the panel adds its header and footer.

### Viewer menus

One action object per command, shared by the menus, the footer and its overflow, and the canvas
context menu. Dropdown-backed submenus mirror their footer dropdown item for item.

| Menu | Contents |
| --- | --- |
| View | Fit (`Ctrl+0`), 100% (`Ctrl+1`), Zoom In, Zoom Out, Zoom ▸ (the zoom presets), Resolution ▸; Channel ▸, Display / View ▸, Look, Reset Exposure and Gamma, Region of Interest, Clear Region of Interest; Guides ▸ (Safe Areas `Ctrl+Alt+S`, Safe Area Preset ▸, Custom Safe Areas…, Centre Cross, Thirds, Rulers, Pixel Grid), Background ▸ (Solid, Grey, Black, White: the surround), Transparency Grid |
| Select | Select All / Deselect All / Invert (`Ctrl+A`, `Ctrl+Shift+A`, `Ctrl+Alt+A`), naming their target: anchors while a path is edited (the Pen tool, a selected mask, or an anchor selection), layers otherwise |
| Add | Solid, Text; the shape kinds; Import and Add… (the drop path's import, then layers at the playhead) |
| Canvas context menu | On a layer: Select it, Duplicate Layer, Delete Layer; then Fit, 100%, Zoom In, Zoom Out; Add ▸, Guides ▸; Composition Settings…. The anchor menu stays first while a path is edited |

The frame field defaults to `HH:MM:SS:FF`, honors `timeline/time-format`, accepts either
non-drop timecode or an exact frame index, clamps to the composition, and rejects invalid
fields without changing time. It uses nominal-rate timecode and exact rational frame mapping.

With Select active, Ctrl-drag defines a composition-space ROI without selecting or moving a layer.
The ROI toggle suspends and restores the rectangle; Clear ROI forgets it. The viewer dims outside
it and draws an Accent hairline. Escape keeps the region drawn so far; capture loss, mapping
changes, and session changes cancel an unfinished drag; switching compositions clears the
rectangle. These bindings use the same Qt
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
adapter. `KAnchorGrid` owns the nine-point visual grid and its input (a click on a dot,
or the arrow keys), while each adapter says which point is selected and what choosing one does:
the Properties adapter resolves bounds off the UI thread and sets the layer's anchor, and the
Composition dialog's resize anchor only records the chosen point.

A `KValueField` at rest paints a value that fits its cell, at its configured decimal places --
trailing zeroes are kept, so the decimal points of an X/Y/Z column line up. When the number and unit
are wider than the cell, it drops the unit first, then decimal places, then falls back to scientific
notation with fewer significant digits, and finally to an ellipsis, so a digit or a sign is never
cut by the cell edge or painted over its axis prefix. A rounded reading is underlined with a dotted
`Faint` hairline; the hover tooltip and the editor keep the exact value (the shortest text that
reads back to the same double). An in-field label is measured and painted in `UiSmall` and elides
inside its `ValueFieldLabelColumn` cap. `KListSurface` paints the common flat empty-row backdrop;
`KRow` owns populated rows and column headings. A heading explicitly identifies its shorter
`Control` pitch through `headerRow`.

## Layout and specialized metrics

The default is the Default workspace preset: Assets, Viewer and Properties side by side, with the
Timeline under Assets and Viewer. The workspace splits 80% / 20% (left region, Properties), the left
region 66% / 34% (Assets/Viewer row, Timeline), and the row 25% / 75%. These weights are authored in
`WorkspacePresets`, kept in the host's own splitters, and applied after the complete split tree
receives window geometry, so early minimum-size clamping cannot change the intended proportions.

| Layout token | Value |
| --- | --- |
| Default preset column weights | 800 / 200 |
| Default preset left-region weights | 660 / 340 |
| Default preset top-row weights | 250 / 750 |
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
| `TimelineToggleColumn` | 136 | Video, Audio, Solo, Lock and Label: five `ToggleCell`s with XS gaps |
| `TimelineSwitchColumn` | 28 | The Shy switch after the name: one `ToggleCell` and its XS gap |
| `TimelineLeftColumn` | 744 | Leading switches + default name + Shy + three 100px dropdown columns |
| `GizmoHandle / GizmoRotateZone` | 8 / 16 | Viewer transform handles / outside-corner rotation radius |
| `ViewerWorkPadding` | 48 | Padded image work area |
| `StatusSeparatorHeight` | 12 | The hairline between two status bar segments. |
| `StatusActivityMinWidth` | 160 | The least room the status bar's Activity text keeps before whole segments collapse. |
| `DropdownWidthWorkspace` | 160 | The context bar's Workspace field. |
| `ContextBarHeight` | 32 | The context bar under the menu bar: `Control` + 2 × `ChromePadding` |
| `ViewerChannelWidth` | 80 | Channel dropdown |
| `ViewerModeWidth` | 108 | RAM Preview command floor (widened to its widest label) |
| `ViewerAdjustWidth` | 72 | Exposure and gamma fields |
| `ViewerDisplayViewWidth` | 176 | Display / View at most |
| `ViewerResolutionCompactWidth` | 64 | Resolution in its compact form |
| `ViewerEmptyStateWidth` | 320 | No-composition invitation column |
| `NonDefaultMark` | 4 | A value field's differs-from-default dot |
| `ViewerZoomWidth` | 64 | Fit/zoom dropdown |
| `ViewerResolutionWidth` | 96 | Resolution dropdown |
| `NodeCardWidth` | 240 | Normal card floor |
| `NodeGroupTitle` | 30 | Node group frame title band |
| `NodeLinkHit` | 12 | Node link hit stroke |
| `NodeTitleBand` | 32 | Node name and category on one row |
| `NodeSocketDot / NodeRerouteDot` | 8 / 10 | Scene port geometry |
| `NodeLinkHandleMin` | 32 | Minimum spline tangent |
| `NodeColumnGap / NodeRowGap` | 80 / 24 | Legacy unplaced-node grid |
| `NodeArrangeGap` | `Spacing::L` (16) | Rank, component and creation clearance |
| `GraphValueAxis / GraphHandleDot` | 48 / 6 | Graph editor value gutter / ease-handle dot |
| `Hairline / SelectionEdge` | 1 / 2 | Pixel-edge arithmetic / scene selection outline |
| `NodeGrid` | 16 | Node snap grid |
| `PlayheadHalfWidth / PlayheadHeight` | 5 / 6 | Timeline playhead head |
| `RulerLabelGap` | 10 | Timeline ruler labels |
| `MinorTick / MajorTick` | 4 / 8 | Timeline tick extents |
| `ViewerChecker` | 22 | Transparency Grid cell |
| `ViewerMinWidth / ViewerMinHeight` | 220 / 176 | Viewer minimum; the hosted panel adds header and footer |
| `MultilineHeight` | 78 | Expanded text editors |
| `PropertyLabelCompact` | 64 | Timeline property label |
| `DialogTextWidth / DiagnosticHeight` | 520 / 140 | Read-only explanation / job diagnostics |
| `DialogWideWidth` | 700 | Minimum width of a settings dialog with Basic and Advanced tabs (Composition dialog) |
| `DialogDocumentHeight` | 480 | Minimum height of a dialog whose body is a document (Open Source Licenses) and the opening height of Bloom's file dialog |
| `PreferencesRailWidth / PreferencesLabelWidth` | 180 / 184 | Preferences sidebar / the one label column every Preferences page shares |
| `DialogSidebarWidth` | 200 | The places sidebar of Bloom's file dialog |
| `ProgressTrack` | 6 | `KProgressBar` track height |
| `SplitHandle` | 6 | Draggable divider hit zone (`KSplitHandle`); paints one snapped hairline centered in it |
| `ScrollBarTrack / ScrollBarThumb` | 12 / 8 | Constant scrollbar width / resting thumb |
| `TooltipMaxWidth` | 320 | Tooltip wrap width |
| `SliderTrack / SliderHandle` | 4 / 12 | `KSlider` |
| `RangeTrack / RangeHandle` | 6 / 8 | `KRangeSelector` |
| `SwitchTrackWidth / SwitchTrackHeight` | 30 / 16 | `KSwitch` |
| `RadioMark / RadioDot` | 14 / 6 | `KRadioGroup` discrete mark and its selected dot (even, so it centres on the pixel grid) |
| `PickerSquare / PickerMarker / CheckerCell` | 160 / 8 / 8 | `KColorPicker` square, marker, and every alpha checkerboard cell |

`kKeyDiamondRadius` is 4.5 and `kNodeCanvasHalfExtent` is 262144 logical pixels. Ratios,
centering divisors and wheel detents are unitless computations, not pixel dimensions.

## Enforcement and migration

`bloom-quality-check` rejects, outside the kit: raw controls (including default construction
without parentheses and QPlainTextEdit/QTextEdit), forbidden paintEvent, literal dimensions and
QFont construction; the Qt convenience dialogs and spin boxes; numeric QColor construction, colour
strings (`#rrggbb`, `rgb(`) and Qt colour names; `.darker(`/`.lighter(`; palette overrides
(`setPalette`, `setColor(QPalette::...)`); `setStyleSheet`; raw `Qt::Popup`/`Qt::ToolTip` windows;
literal pen widths; literal `resize(w, h)`; and `Variant::Ghost` other than on a disclosure caret
(`expand`, `chevron`, `caret`, `disclosure`). The kit's own `.cpp` files are held to the pixel
literal rule too (only the structural values 1 and 2 pass; the tokens file is the one place a
value is defined). The allowlist is empty; adding any entry is itself a hard failure. Balanced extent-call checks detect literal arguments
after nested token calls without consuming unrelated following expressions.

Offscreen fixture-window metric audits run at DPR 1, 1.5 and 2. Whole-window references run
at DPR 1 and 1.5. Clear keyboard focus and deliver Leave before capture. Pixel assertions use
relative brightness. Goldens use a documented small tolerance; drift fails until an explicit
`--update-goldens` commit explains the visual change. References are generated from the
application and never from a design mockup.

The [validation map and capture contract](grammar1-validation.md) identify enforcement, capture generation and the completed migration.

## Automation additions

Existing object names remain stable, except that ADR 0031 retired the Viewer's tool column
(`viewerToolColumn`, `viewer<Tool>Tool`, `viewerAnchorModeSwitcher`, `toolColumnMoreButton`). The
context bar's names are `contextBar`, `contextBarRow`, `contextBarTools`,
`contextBarTool<Tool>` (Select, Hand, Zoom, Line, Pen, Text, Cryptomatte; also the shape flyout's
Rectangle, Ellipse, Polygon and Star actions), `contextBarToolShape`, `contextBarShapeFlyout`,
`contextBarAnchorOptions`, `contextBarAnchorMode`, `contextBarShapeOptions`, `contextBarShapeFill`,
`contextBarShapeFillColor`, `contextBarShapeStroke`, `contextBarShapeStrokeColor`,
`contextBarShapeStrokeWidth`, `contextBarTextOptions`, `contextBarTextFont`, `contextBarTextSize`,
`contextBarTextAlignment`, `contextBarAlign`, `contextBarDistribute`, `contextBarAlignTarget`,
`contextBarAlignTo`, `contextBar-alignAction-<arrangement>`, `contextBarOverflowButton` and
`contextBarOverflowMenu`; the shared actions are `alignAction-<arrangement>` and
`alignTargetAction-<target>` (the scripting ids), in `alignMenu` / `alignToMenu`, and the window's
toggle is `windowContextBarAction`. Other new names: `nodeReadOnlyValue`. Node row widgets expose `nodeParameterRole` and
`nodeParameterRowPitch` for geometry audits; list headings expose `headerRow`.

Dialog automation names (lane C): the Composition dialog's fields are `compositionSettingsName`,
`compositionSettingsWidth`, `compositionSettingsHeight`, `compositionSettingsRatePreset`,
`compositionSettingsRate`, `compositionSettingsStart`, `compositionSettingsDuration` and
`compositionSettingsDurationUnit`, its buttons `compositionSettingsButtons`, `compositionSettingsOk`
and `compositionSettingsCancel` (the earlier `assets*` names are retired); its preset prompts are
`compositionSavePreset`, `compositionRenamePreset` and `compositionDeletePreset`. Other dialogs:
`compositionRenamePrompt`, `assetsDeleteConfirmation`, `assetsTagsDialog`, `assetsInputColorDialog`,
`viewerSafeAreaDialog`, `projectColorSettingsDialog`, `projectUnsavedChanges`,
`projectRecoverUnsaved`, `preferencesWindow` (with `preferencesOkButton`,
`preferencesCategoryRail`, `preferencesActionsButton`, `preferencesRestoreDefaultsAction`,
`preferencesRestoreDefaultsConfirmation`, `preferencesMediaDiskCacheDirectoryReason` and
`preferencesMediaDiskCacheDirectoryClear`; an Automatic/Custom value's mode dropdown is its field's
name plus `Mode`, a switch's words its name plus `Label`),
`licensesWindow` (`licensesCloseButton`), `compositionExportDialog` (`compositionExportFirst`,
`compositionExportLast`, `compositionExportBrowse`, `compositionExportAudio`,
`compositionExportContinue`, `locateOpenH264Progress`, `cancelLocateOpenH264`,
`locateOpenH264Message`), `openh264Consent`, `exportFrameRangeDialog`
(`exportFrameRangeDestination`, `exportFrameRangeBrowse`), `exportApproval`, the failure notices
`projectSaveFailed`, `projectOpenFailed`, `projectCopyFailed`, `exportFailed` and
`projectColorSettingsFailed`, and the label colour picker `timelineLayerLabelColorPicker` (it
replaces `timelineLayerLabelColorDialog`). A prompt's heading and detail are `<dialog>Heading` and
`<dialog>Detail`.

The metric audit covers the status line, the context bar's eight tool slots,
card/control containment and socket-row alignment at DPR 1, 1.5 and 2. It also verifies the timeline menu set remains expanded at
1600 and 1920 logical-pixel window widths. Whole-window references and final captures run at
DPR 1 and 1.5. Changed geometry tests use the viewer's real padded mapping.

Every panel's single border is painted by `KPanelFrame`, an overlay raised above the panel's
children (Accent while the panel is active); the stylesheet gives the panel only its background.
Panels are square (`Radius::Panel` is 0); a non-zero radius would also have the frame fill the
corner wedges outside the curve. Header and footer share ChromePadding. Viewer declares
View, Select, Add; Composition commands live in the application's Composition menu. Only
EditorArea exposes maximize. New automation names: `viewerAddMenu`, `viewerAddMenuButton`,
`viewerFooterOverflowButton`, `viewerFooterOverflowMenu`, `viewerRoiAction`,
`viewerClearRoiAction`, `viewerLookAction`, `viewerResetViewAdjustAction`,
`viewerViewAdjustOverflowAction`, `viewerOverflowExposure`, `viewerOverflowGamma`,
`viewerRamPreviewAction`, `viewerZoomMenu`, `viewerZoomPreset<n>Action`, `viewerResolutionMenu`,
`viewerResolution<n>Action`, `viewerDisplayViewMenu`, `viewerDisplayView<n>Action`,
`viewerGuidesMenu`, `viewerCustomSafeAreasAction`, `viewerAddSolidAction`, `viewerAddTextAction`,
`viewerImportAndAddAction`, `viewerCanvasContextMenu`, `viewerContextSelectLayerAction`,
`viewerContextDuplicateLayerAction`, `viewerContextDeleteLayerAction`,
`viewerContextCompositionSettingsAction`, `viewerEmptyState`, `viewerEmptyStateTitle`,
`viewerEmptyStateHint`, `viewerNewCompositionButton`. The retired
`viewerCompositionMenuButton` and its `viewer*CompositionAction`s stay hidden, each running the
application's own command when hosted.

Rows own `Spacing::RowPadding` (1 on all edges); every control owns `ControlInset` (1)
inside its allocation, including scene proxies. `PropertyGutter` (8) separates labels and
controls independently of component gaps. KSection owns `SectionPadding` (8 on every edge),
overridable per section via `setBodyPadding`; Properties uses `Spacing::XS` (4) for its outer
side/bottom margins, outer top margin, and section body padding so every visible band reads as
exactly the 6px inter-panel `Gutter` once the area's own 1px content inset, 1px content margin
(absent on top, where the header bounds the content), section hairline, and row padding are
accounted for: 1+1+4 on the sides, 0+6 on top, 1+4+1 inside sections.
A switch is never a row's flexible field: it keeps its own size at the value column's leading
edge instead of stretching centered. Expanded RGBA rows use a blank-label KPropertyRow so controls align beneath the swatch.

Timeline switches retain ToggleCell squares (24), with IconMedium (16) glyphs: unchecked is an
empty neutral bordered box, checked uses the same Regular glyph as the column heading. Hidden
audio retains its cell; Spacing::XS separates switch boxes in both header and layer rows.
Headers and rows share the ChromePadding left inset,
column geometry, and Ui text role. Layer and nested-property disclosures are unboxed 24px hit
targets with their existing 16px chevrons and an XS gap before the text. KDiamond uses the Bold
outline at every DPR. Colour parameter rows show only the swatch; expanding the parameter exposes
the individual R/G/B/A numeric rows, so narrowing the table never crushes four inline fields.
Layer rename uses the shared KLineEdit at the name label's horizontal bounds, vertically centered
in the row; it never covers the chevron or expands as text is typed. It follows column resizing.
Return, Escape, Tab and focus loss all commit a changed name once (the kit inline-edit policy);
unchanged text creates no transaction, and an empty name is refused with its reason rather than
reverted in silence.

The timeline's graph editor REPLACES the key lanes rather than sitting beside them: in graph mode
the lane region paints only its Surface backdrop and the curve view covers it, because two views of
the same keys at once only make an artist ask which one they are editing. A `Size::HeaderRow` graph
header (`timelineGraphHeader`) over the canvas carries the segmented Value Graph / Speed Graph
switch and the Normalize check box, persisted under `timeline/graph-type` and
`timeline/graph-normalize`. The Value Graph plots the curve; the Speed Graph plots its speed (value
units per second) read off the sampler, with After Effects' horizontal influence/speed handles.
Normalize (Blender) draws every curve scaled to -1...1 in one shared window; edits are converted back
and always write real values, and the scale holds still for the length of a drag. Below the header,
the left gutter is `Size::GraphValueAxis` wide and carries the ACTIVE curve's value ticks -- the
curve of the primary keyframe selection, else the one on the selected row, else the first -- and the
gutter is painted over the curves so a line running off the left edge slides under the axis instead
of colliding with its labels. While the graph is shown the shared time axis leads with the gutter's
width (`TimelineRuler::setLeadingInset`), so the ruler, the work area and the graph move together
and a key at frame 0 always sits clear of the gutter. The background grid is After Effects': `Border`
hairlines at the gutter's value ticks and at the ruler's own labelled ticks. A component curve strokes
in `Color::ComponentX/Y/Z/W` (X red, Y green, Z blue; R/G/B/A borrow them unchanged so one hue always
means "the first component"); single-value curves take `Color::GraphCurve1..5` in curve order,
cycling. The selected curve -- every curve carrying a selected key, else the selected row's -- is
drawn brighter (`kit::emphasizedStrokeFor`) and on top. Selection is Accent, which none of the curve
tokens is. Handles are drawn only for a SELECTED key on an eased segment: an arm in the curve's own
ink to a `Size::GraphHandleDot` Accent dot. The graph toggle persists under `timeline/graph-editor`.
View exposes checkable Keyframes and Graph Editor choices; enabling either turns the other off, and
both may be off to show plain layer lanes. The one header tool button is the graph toggle
(`timelineGraphToggle`, `IconId::GraphEditor`) at the right end of the layer-table header, beside
Fullscreen: it and `Shift+F3` switch the right pane between the keyframes and the graph.
`timeline/keyframes-visible` and `timeline/snapping` persist Keyframes and Snap to Frames.
Header menu order is Add, View,
Select, with text-only menu buttons and no caret indicators; the composition dropdown is absent.
These are the unchanged shared KMenuButton/EditorChromeRowSpec controls and application theme used
by Assets, Viewer and Nodes: no Timeline-specific menu painting, stylesheet or background palette.
Right-clicking empty layer-table space opens a shared kit popup with the existing Add submenu,
then direct Keyframes, Graph Editor, Snap to Frames, Zoom to Fit, Select All and Deselect All actions.
There are no View/Select wrapper submenus or selection-only layer commands; action objects and
checked state are shared with the header. Opening the popup never changes the layer selection.
Timeline's fullscreen button sits at the right edge of the layer-table header, beside the divider.
Keys off hides both key lanes and collapsed summary
glyphs. Empty key-lane labels paint no chip. Lane snapping is `snapping && !Shift`; in the graph,
Shift is the axis lock and snapping follows Snap to Frames alone.

Timeline lanes use `LanePadding` (12) on both sides of their time axis. The layer column/lanes split
is a draggable `KSplitHandle` (`Size::SplitHandle`, 6, its hit zone; it paints one snapped
Background hairline centered in that zone, BorderHover while hovered or dragged, and ends a drag
when it loses the pointer grab), used identically for the header split so the ruler
and lane region always move together with the column headings and rows below them. It is clamped
between the layer table's own minimum (name, Blending and Parent columns all still visible) and a
maximum that leaves `PanelMinWidth` for the lanes. A fresh Timeline defaults to 37% of its live
width; an existing `timeline/layer-column-width` pixel value wins, and a double-click or Reset
Workspace returns to the 37% ratio. All timeline rows share the ListRow pitch and a zero origin;
the 28px KPropertyRow is centered within that pitch.
Selected rows use the kit selected-row recipe: the Accent wash over the row in both halves, with
the SelectionEdge Accent edge at the layer row's leading side. Every row of the layer table --
layer, group, parameter and component -- takes the list hover step (one surface rung up) under the
pointer and is clickable; a clicked group or property row wears the same selected-row recipe
(`KListSurface::setRowStates` under the rows that paint no background, `KRow::setHovered` on a layer
row) and its key lane the same wash. The layer table's columns read, as in After Effects: Video,
Audio, Solo and Lock, the Label square, the disclosure and name (which takes every extra pixel),
the Shy switch (`KRow::setCells` switches after the name), then Blending, Parent and Matte. The
column headings put the label filter (`timelineLabelFilterButton`, `IconId::LabelFilter`) over the
Label squares and the Hide Shy Layers toggle (`timelineHideShyToggle`, `IconId::Shy`) over the Shy
switches; both are plated `KIconToggle`s that wear the Accent plate while filtering or hiding. New
names: `timelineLayerLabel`, `timelineLayerShy`, `timelineLayerLabelMenu`,
`timelineSelectLabelGroupAction`, `timelineLabelFilterMenu`. Every populated and empty row uses
one snapped Background hairline separator (`kit::paintRowSeparator`, the window color, never a
lighter border), without alternating fills.
The work-area slider occupies the lane half
of the layer-column header row, directly below the ruler. Its active range uses SurfaceRaised; outside the range stays Surface, with accent endpoint handles.
Cached-frame strips use green Ok. Ruler tick labels and the current-frame readout are centered on
their needles. The Accent playhead has a solid tab with a downward-pointing tip below its readout,
joining the single-pixel line through the work area and lanes; the readout never paints over it.
The readout reserves its rectangle against tick labels. The footer navigator hides
at fit, keeping the zoom slider visible. It uses a rounded 10px neutral BorderHover thumb with no
painted background or track; hover/drag uses BorderActive. Its ends advertise resizing, and its
body advertises panning.
Clip trim edges show SizeHorCursor using the same
hit tolerance as trimming; locked layers do not advertise trimming. The vertical scrollbar appears
only when the expanded row content exceeds the viewport. Only then does a ScrollBarTrack-width
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
including transport, loop, snap and keyframe toggles, and the context bar's active tool.
KDropdown's preferred width is the measured widest item plus its icon, padding and chevron; its
minimum is the same chrome around about six average characters, so a layout short of room elides
the value instead of overflowing. `setWidthFloor()` is a real floor: the field hugs its widest
item where there is room and never shrinks below the floor. A Properties row, a node card and a
timeline column keep their dropdowns at the widest item (or the column width) when they fit; a
non-prioritised chrome row never shrinks one below its widest item. A dropdown that commits to
the document listens to `activated()` (the artist's choice), never to `currentIndexChanged()`
alone, so a refresh cannot write back.

Properties shows every section with no filter strip and no header search. Sections collapse
individually with persisted state; there is no Collapse all / Expand all affordance and no
section menu. KSection headers carry only the chevron, title, and Reset.

Node Add menu and search order is Sources, Layers, Compositing, Transform, Filters, Colour, Values,
Math, Convert, String, Logic, Time, Color, Vector, Utilities, Output. Transform holds geometric image
operations (Resize, Fit, Crop, Pad, Flip, Rotate 90); Filters holds the blurs. The UI category projection owns normalization;
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
Node parameter rows are `KPropertyRow`s with a separate fixed diamond column. Their numeric fields
use the parameter's presentation places at rest, trailing zeros kept so a column's decimal points
line up; editing retains full precision. KDiamond draws vector geometry through the painter's device transform, including canvas
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
not a data migration. The timeline colour rows use the same boundary: an in-range colour shows
normalized display numbers in its expanded R/G/B/A rows, and a signed or HDR RGB switches the whole
row to exact reference numbers with the `reference` suffix.

## Image Assets And Source Cards

Assets is one tree after the After Effects Project panel (#378): folders, compositions and
imported assets together. Above it sit a compact summary strip and, when opened, a search row; the
panel footer keeps New Composition, New Folder, Import and Delete as fast icon buttons.

**Columns.** Name, Type, Dimensions, Duration, Frame Rate, File Size, Tags and File Location. Name,
Type and Dimensions show by default; the others are chosen from the header's context menu or
View > Columns (Name always shows). Name is the header's one `QHeaderView::Stretch` section, so the
shown columns always span the viewport exactly at any panel width and no dead band is left to the
right; every other shown column fits its content (never below `ValueCellMin`) until the user drags
it. Visibility, dragged widths, order and sort are a per-user setting (`assets/header-state-v2`,
`assets/sized-columns`), never project data. Type names the kind; Dimensions is `W × H` (Size was
renamed: size reads as bytes on disk). Duration shows the After Effects timecode `H:MM:SS:FF` at
the item's rate; its tooltip carries the frame count and rate in the one duration format (#380),
`0:00:02:00 (48 frames) at 24 fps` (`(1 frame)` in the singular). A sequence plays one member per
composition frame, so it is timed at the active composition's rate (24 fps with none open) over
its whole numbered range; audio shows seconds. Frame Rate is a composition's or video's rate. File
Size is the source's bytes on disk in SI units, a sequence's members summed, measured by the asset
controller's thumbnail worker (cancellable, beside the probes that already resolve those paths;
nothing is stat'ed on the interface thread) and blank until it has run. File Location elides in the
middle so the file name stays; the full path is its tooltip.

**Rows.** One `Size::AssetRow` (26, one control height, so the inline rename field fits a row
exactly) at the kit's Ui type size. Each level indents by `Size::AssetIndent` (20: one 16-pixel
icon plus an XS gap), which is also the caret column: the Phosphor caret (Muted, monochrome) sits
in the indent in front of the row, the kind glyph at the Name cell's left and the name one indent
later, so a child's glyph starts exactly where its parent's name does and a root row has only the
one caret column in front of its glyph. `AssetRowDelegate` paints every cell: the glyph, the name in
Foreground (Muted on the Data group's rows) with a folder's muted child count after it
(`test · 1`), and every other column in Muted. `AssetTree::drawRow` paints the row background across
the whole viewport before the cells -- the alternating Background/Surface rows, the full-width
hover step and the one selected-row recipe (selection wash with the 2-pixel Accent edge at the
row's left, never at its indent) -- so nested rows are never notched.

**Glyphs.** Monochrome, in the row's ink (Foreground; Muted on secondary rows; OnAccent selected):
Composition (`film-strip`), Folder (`folder`, `folder-open` while expanded), Image or EXR
(`image`), Sequence (`images`), Video (`film-slate`), Audio (`music-notes`), Font (`text-t`) and LUT
(`sliders-horizontal`). Colour is kept for state: a missing or unshowable source replaces the glyph
with the warning glyph in Warn ink; an untagged video whose colour was assumed shows the Info glyph
in the row's ink. The tooltip states the reason, the input colour space and the colour note. A
data-only image has no mark.

Every column sorts on a header click; folders stay first and the Data group last in either
direction, sizes, times and rates sort by value and names in natural, case-insensitive order.
Columns resize and reorder by dragging. Folder rows expand with Qt's own branch and keyboard
navigation; expansion and selection survive project projections, and filtering temporarily opens
matching ancestors.

**Summary strip.** One fixed `Size::SummaryStrip` (52: two Control-height lines) for every
selection, so the tree never moves. With one item: a `KThumbnail::Cell::Strip` well
(`StripThumbnailWidth` × `StripThumbnailHeight`, 64 × 40, on Field) letterboxing the controller's
worker thumbnail at its own aspect ratio (Cryptomatte ID colours or the first data channel of a
data-only image; for a composition a frame rendered by the composition preview pipeline; otherwise
the kind glyph, Muted, never a blank cell); line 1 the name in Ui Foreground, elided, with the usage
trailing in UiSmall Faint (`Used 3 times in 2 compositions`, `Not used`); line 2 the facts in UiSmall
Muted as chips separated by thin dots: `Sequence · 2048 × 858 · 0:00:16:02 · 386 f · 24 fps · sRGB ·
Straight`. Each line's tooltip is its whole sentence (`Sequence · 2048 × 858 · 0:00:16:02 (386
frames), 24 fps`; `Colour: sRGB (from the file) · Straight alpha`). The colour facts name the input
colour space by its plain name, then the alpha, then the colour note once; a note, a missing source
or an unshowable one turns that part Warn, with the detail in its tooltip
(`Colour: sRGB · Straight alpha · Colour tags missing — assumed Rec.709 with the sRGB transfer.
Change in Assets → Input Colour`). Bloom Neutral's interchange identifiers read as plain names (`lin_rec709_scene`
is **Linear Rec.709 (sRGB)** (the working-space label, `host::workingColorSpaceLabel`), `srgb_rec709_display` **sRGB**, the synthesized Rec.709 camera space
**Rec.709 video**); another config's spaces show the names the config gives them. A space Auto read
from the file's own tags adds `(from the file)` in the tooltip; an assumed one is explained by the
note alone. Several items: `3 items selected` and their kinds (`2 sequences · 1 composition`). None:
one centred Faint line of what the project holds (`7 items · 5 sequences · 2 compositions ·
1 folder`), never an empty tile -- a fixed strip is chosen over collapsing it, so selecting never
pushes the tree down. View > Show Info hides the strip (`assets/show-info`, per user).

A composition's strip frame is its first frame, prepared on a worker by the same compile,
evaluate and display-transform pipeline the Viewer's analysis uses, at a proxy no larger than
144 device pixels on its longest edge. The `CompositionThumbnailController` keeps the newest frame
per composition with the document revision it was prepared from (at most 64 compositions), prepares
a revision once (a frame that cannot be prepared is not retried until an edit), shows the previous
frame while an edit is prepared and cancels on shutdown. Nothing is evaluated or decoded on the
interface thread. Usage counts nodes whose asset, font, LUT or composition reference names the item.

**Search.** Collapsed by default: a Search toggle (`magnifying-glass`, "Search (Ctrl+F)") at the
panel header's trailing end beside Maximize, and Ctrl+F (Cmd+F on macOS) while the panel has focus,
open a slim Control-height row directly under the header with a `KSearchField`, a Muted match count
(`3 of 7`: the assets and compositions the filter keeps, not their folders) and a Close button.
Opening focuses the field. Escape clears the text; Escape on an empty field (or Close) closes the
row and returns focus to the tree. While text filters the tree the row stays open even after focus
leaves, and the toggle stays in its Selected state, so a filter can never silently hide assets.
Typing while the tree has focus is the tree's own type-ahead to an item, not search. The header is
a priority row: as the panel narrows its menus fold one at a time into the overflow
(`dots-three-vertical`) -- Select first, then Add, then View -- and the Search toggle keeps its
place beside Maximize. Text set by any other route (a Tags cell, automation) opens the row too.

Context menus follow After Effects. An asset: New Composition from Selection, Input Colour…
(interpret footage), Replace Footage…, Reload Footage, Reveal in File Manager, Rename, Edit Tags…,
Delete. A composition: Open, Composition Settings…, Rename, Duplicate, Delete. A folder: Rename,
Delete. Each ends with New Folder, New Composition… and Import…, which is the whole menu on empty
space. Reveal selects the file in the platform's file manager (freedesktop.org
`FileManager1.ShowItems` on Linux, `open -R` on macOS, `explorer /select,` on Windows) and opens the
folder holding it where that is not available; one helper chooses the command per platform. Deleting unused items is immediate; deleting a used item, or a folder that holds items,
first asks in a warning box that names the usage, and for folders offers Delete Folders Only (the
contents move up) or Delete with Contents. Every delete is one transaction and one undo step.

Rename edits the Name cell in place through a kit line edit, from Rename, F2 or Return. Return,
Escape and clicking away commit (Escape confirms everywhere; Undo is the discard). Every accepted rename uses the shared command transaction. The
Tags column lists an asset's tags; clicking it filters by the first tag. Edit Tags… opens one kit
line edit per tag with Add Tag and Remove Tag controls; applying to multiple selected assets
replaces their sets in one transaction.

Search accepts `tag:` followed by a case-insensitive tag substring (clicking a Tags cell opens the
row with `tag:` and its first tag); plain text matches names and tags. Unmatched ancestors remain visible only when a descendant matches. Internal drags move assets,
compositions and folders together onto a folder row, into the folder of the row under the pointer,
or to the root on empty space; a folder never moves into itself or a subfolder. Rows are sorted, so
a drop names a folder, never a position. The internal selection MIME coexists with
`application/x-bloom-layer-items` (every asset and composition in selection order, for Timeline and
Viewer layer drops) and `application/x-bloom-asset` (the first asset) or
`application/x-bloom-composition` (the first composition when no asset is dragged) for Nodes. Internal drags
are tied to the originating tree and snapshot; a changed or replaced project refuses a stale drag.

File > Import, footer Import, Add > Import…, and file or folder drops onto Assets, Timeline, Viewer
and Nodes all prepare one worker import transaction, shown by the import progress dialog below; a
Timeline or Viewer drop then adds layers for what was imported. Imports land in the selected folder (a selected folder, or the folder of the selected
item); a drop lands in the folder under the pointer, or at the root on empty space.
A drop onto Assets also accepts an `smb://` URL a file manager hands Bloom for a mounted network
share, resolving it through the same GVFS mount the file manager used; an `smb://` URL naming a
share this machine has not mounted yet is still accepted (the cursor stays honest about there
being something to do) but imports nothing, reporting "Connect to `<host>/<share>` in your file
manager first" through the status bar's transient-message cell rather than dropping it silently.
Every file dialog (`kit::chooseFile()`: Import, Relink, Open Project, Save Project As, the export destination, Locate OpenH264 and the cache folder) is Bloom's own file
dialog and lists every mounted network share in its sidebar beside the standard places.
Dragging an image/sequence row to Nodes creates an Image source; dragging an Audio row creates an
Audio source. Dropping media or compositions on the Timeline or the Viewer creates the matching
wired Layers (interaction-model.md, "Layer drops"). During a Timeline drag the layer stack shows
the `timelineLayerInsertionIndicator` Accent line (`Spacing::XXS` tall) above the row the layers
will join, the lanes continue it (`timelineDropRowLine`), and over the lanes a `Spacing::XXS` wide
Accent time marker (`timelineDropTimeMarker`) shows the frame they will start on. Both lane marks
are child widgets raised above expanded property rows, so no row hides them. Image source cards and
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

**Dialogs.** Every Bloom dialog is a `kit::KDialog` (`kit/dialog.hpp`) in the platform's native
window frame; there are no frameless or custom-titlebar dialogs. A dialog is constructed with a
title, an object name and a required owner, resolved to the owner's top-level window, and states
its modality. It sits on `SurfaceRaised` with `Spacing::M` margins and `Spacing::S` between rows,
and takes its width only from `DialogTextWidth` or `DialogWideWidth` (a document also takes the
`DialogDocumentHeight` minimum height); no dialog calls `resize()` with a literal. A dialog is never
narrower or shorter than its content: it grows to its layout's minimum width and to the height its
content needs at that width.

- **Form.** `KForm` lays out two columns: `KLabel` labels, left-aligned on every platform, in one
  label column shared by every form of the dialog (sections included), and fields that grow to the
  right. Rows are added in visual order and the dialog's Tab order follows that order (a section's
  header first), whatever order the widgets were constructed in. A check box row puts its words in
  a `KCheckLabel` that toggles the box when clicked.
- **Tabs** (`kit/tab_bar.hpp`). `KTabBar` is Ui text tabs over a hairline: the current tab in
  Foreground over a `SelectionEdge` Accent underline, the others Muted, the kit keyboard-focus
  border around the current tab, Left/Right/Home/End to move, accessible as a page tab list.
  `KTabs` puts it over a page stack as tall as its tallest page; Ctrl+Tab and Ctrl+Shift+Tab step
  the tabs from any field. A refused value on a tab that is not showing puts a small Error dot
  (`NonDefaultMark`) after that tab's name, and Accept then switches to it and focuses the field.
- **Reasons.** One validator per dialog returns the first refusal: its field and its plain reason.
  The reason shows directly under that field (the inline field reason below), or above the buttons
  when it belongs to the dialog as a whole (the project changed while the dialog was open); a field
  inside a collapsed section opens it. Accept, Destructive and Alternate stay disabled while any
  reason exists; Enter on a refused dialog switches to the refused field's tab and focuses it. A
  wait that is not an error (the OpenH264 check still running) is a *pending* refusal: the same
  place, in `Muted` ink.
- **Button row.** Kit buttons in a `QDialogButtonBox`, so the platform orders them while the kit
  paints them: Accept is a Primary `KButton`, Destructive a Danger one, Reject, Alternate, Apply
  and Reset Secondary. Buttons are named `<dialog>AcceptButton`, `<dialog>RejectButton` and so on
  unless a dialog keeps an older automation name. A destructive prompt's default button is the safe
  action.
- **Helpers** (`kit/dialog_prompts.hpp`). `kit::confirm()` (synchronous or asynchronous) names the
  subject and the action in a `Title` heading with a `Muted` detail and a tone glyph (Question,
  Warning, Error, Info); with only an OK it is the error and information notice (`kit::inform()`).
  `kit::promptText()` and `kit::promptRange()` show a refused value's reason inside the prompt and
  keep OK disabled, so a prompt never closes and reopens to refuse.
- **Files** (`kit/file_chooser.hpp`). Every file dialog is `kit::chooseFile()`, and it is always
  Bloom's own dialog: Qt's widget file dialog (`QFileDialog::DontUseNativeDialog`) on Linux, macOS
  and Windows alike, never a platform, desktop-portal or Qt platform-theme chooser (owner decision,
  2026-10-30: a GTK chooser opened through a Qt platform theme landed on another Hyprland workspace
  and left Bloom looking frozen). It is a window-modal child of the owner's window, starts in the
  folder last used for the same kind of file (a user setting, never project data), applies the
  default suffix to a name typed without one, keeps the dialog's filters and lists the mounted
  network shares in its sidebar. The kit styles all of it, scoped to the dialog: the look-in and
  file-type combos take the Secondary button recipe, the name field the text-entry recipe, the
  navigation buttons and every file and place kit glyphs, the file views a `Field` well and the
  sidebar `Surface` (`DialogSidebarWidth` wide), every control one `Control` tall; it opens
  `DialogWideWidth` × `DialogDocumentHeight`.
- **Task dialog** (`kit/task_dialog.hpp`). `KTaskDialog` is the progress-then-summary modal the
  import dialog below is built on: elided heading and detail, a `KProgressBar`, Cancel that only
  asks the work to stop, then a summary with one Close that Return takes. `KTaskTiming` delays it
  and refreshes it.

Dialogs open asynchronously (window-modal, answered through a callback) unless a caller must hand a
synchronous answer back: the Composition dialog, the
unsaved-change, open, save and recovery prompts of the project host, Preferences, and prompts nested
inside an already modal dialog run with `exec()`, each documented at its call site. Edit Tags, Input Colour, Custom Safe Areas, Project Settings — Colour, Delete, Rename Composition,
Restore Defaults and every failure notice are asynchronous. The keys are in
[interaction-model.md](interaction-model.md#dialogs).

The Composition dialog (New Composition and Composition Settings, #376) is one `kit::KDialog` at
least `DialogWideWidth` wide, titled `New Composition` or `Composition Settings`, built only from kit
controls, with two tabs in a `kit::KTabs` (After Effects style; no collapsible sections). **Basic**
holds what most compositions change: Name, Preset with its buttons and summary line, Size with the
link toggle and the frame aspect beside it in muted Value text (`16:9`, `2.39:1`), Frame Rate,
Duration with its unit, and Background Colour. **Advanced** holds the rarely changed values: Pixel
Aspect Ratio, Start, the drop-frame Timecode row (shown only at 29.97 and 59.94 fps), Working
Colour Space, the Resize Anchor (Composition Settings only, shown once the size changes), and
Action / Title Safe. Both tabs are `KForm`s of the one dialog, so they share ONE label column, sized
to the widest label of either, and every field starts at the same x; the dialog is as tall as its
taller tab, so switching tabs never resizes it, and the tab last shown is shown again for the rest
of the session. Every number is right-aligned: the `KValueField`s and the Start, Duration and Custom
frame rate fields, which are entered in the Value role. Text entry uses `KLineEdit`, pixel and
percentage values `KValueField`, choices `KDropdown` (frame rate is one dropdown; its exact
`KLineEdit` appears only for Custom), the aspect lock a `KIconToggle` (`IconId::Link`), and the
preset buttons `KIconButton`s with tooltips `Save as preset…` (`IconId::Add`), `Rename preset…`
(`IconId::Rename`, Phosphor `pencil-simple`) and `Delete preset` (`IconId::Delete`). A muted
`UiSmall` summary line sits under the preset. Drop-frame is a `KCheckBox` with its clickable
`KCheckLabel`; Background Colour is a `KColorChip` at its own size with the `#RRGGBB` value beside it
in the Value role; the resize anchor is the Properties Anchor Point `KAnchorGrid` itself. The
interaction contract is in
[interaction-model.md](interaction-model.md#composition-dialog).

**Preferences window.** Preferences is a settings window rather than a dialog form, laid out the
way Blender lays out its Preferences and built from the Properties panel's own parts, so a
Preferences page and a Properties section look the same. A `PreferencesRailWidth` sidebar lists the
categories one per `ListRow`, grouped by an `Spacing::S` gap: General (and Sounds when it lands) |
Viewer, Timeline, Node Graph | Memory & Caches, Performance; a page that no group names appears in a
group of its own after them. The sidebar has no radio marks: the current category takes the kit
selected-row recipe (selection wash and the `SelectionEdge` Accent leading edge), others take the
hover wash, Up/Down, Home/End and type-ahead move through it, the keyboard-focused row takes the
Accent focus inset, and it is an accessible list. The category last shown is shown again for the
rest of the session. A small Menu `KMenuButton` at the foot of the sidebar holds **Restore
Defaults**, which still asks first; the button row keeps OK, Cancel and Apply.

Each page is a scrolling Properties panel body: `Background`, the Properties content margins and
section spacing, and `properties::makeSection` sections. Every row is a `KPropertyRow` with the
Properties row label (`UiSmall`, `Muted`, right-aligned) in ONE `PreferencesLabelWidth` label column
shared by every page, so every value starts at the same x on every page. Switches sit in the value
column with their own clickable `UiSmall` words (`KCheckLabel`) and an empty label. Dropdowns and
value fields take one width (`DropdownWidthWide`); a path field takes the rest of the row. Helper
text is wrapped `UiSmall` `Muted` under the row it explains, in the value column; `Error` ink is only
for a real refusal (a cache folder Bloom cannot use). A value whose zero means Automatic (the
memory and disk budgets, the timeline layer column width) is a mode dropdown, **Automatic** or
**Custom**, with the value field and its unit beside it only for Custom; choosing Custom brings
back the last custom value. A section whose values are read at startup says so once, with a
`Muted` `UiSmall` **Restart** badge in its header (its tooltip says the value takes effect after
restart). The cache directory is a `kit::KPathField`, the text field that elides a path in the middle at
rest (the whole path in its tooltip and while editing), with the placeholder `Default location`, a Secondary
**Browse…** and a Reset `KIconButton` that returns to the default location. The window opens with
the pages a `DialogTextWidth` column wide and as tall as the tallest page (up to three quarters of
the screen, beyond which a page scrolls), so switching pages never resizes it; it can be made
narrower down to the widest page's minimum. Changes apply on OK or Apply, never live.

**Import Progress Dialog.** Every batch import (#377,
[ADR 0029](../decisions/0029-import-progress-and-cancellation.md)) shows one application-modal
`kit::KTaskDialog` titled `Import`, at least `DialogTextWidth` wide, built only from kit parts with `Spacing::M`
margins and `Spacing::S` between rows. While importing it shows an elided `KLabel` heading (`Finding frames…`, `Finding files…`,
`Importing SC020_SH070_image · sequence`, `Importing 2 of 5: plate.png`, `Cancelling…`), an elided
`UiSmall` `KLabel` detail in `Muted` ink (`SC020_SH070_image · 522 found`, `Reading frame 120 of
522`, `Reading file`), a full-width `KProgressBar` (busy while finding, then filled by work units;
busy again while a single-unit file is read with nothing finished yet, or for longer than
`ImportProgressDialog::kItemBusyAfter`, 400 ms),
and a Secondary `KButton` Cancel at the right. It appears
only after `ImportProgressPresenter::kShowDelay` (300 ms) and refreshes every 50 ms from the
worker's latest report. The summary replaces those rows with the heading (`Import finished`,
`Import cancelled`, `Nothing was imported`), a counts `KLabel` joined by ` · `, an optional wrapped
`UiSmall` `Muted` note, and the skipped files in a frameless scroll area at most `DiagnosticHeight`
tall: each row is the file name in `Ui` (elided, the whole name in its tooltip) above its reason in
wrapped `UiSmall` `Muted`, rows `Spacing::S` apart. The one action is a Primary `KButton` Close, which also takes Return.

`KProgressBar` is read-only and never takes focus: a `Field` track with fully rounded ends,
`ProgressTrack` tall, filled with `Accent` from the start to completed / total. **Busy mode**
(`setBusy()`, or no total) is for work whose size is not known yet: a third-width `Accent` segment
sweeps across the track and back over 1.6 s, held centred when motion is reduced. A busy bar is
never an empty, static track, and a determinate bar is used only once the total is known. Disabled, the fill fades to `kDisabledOpacity`.

Automation names: `importProgressDialog`, `importProgressHeading`, `importProgressFile`,
`importProgressBar`, `importCancelButton`, `importSummaryCounts`, `importSummaryNote`,
`importSkippedList`, `importSkippedFile`, `importSkippedReason` and `importCloseButton`; the bar's
own object name is `kProgressBar`.

**Inline field reasons.** A form that refuses a value keeps its accept button disabled and shows one
plain reason directly under the offending field: a word-wrapped `KLabel` in `UiSmall` with
`Color::Error` ink, `Spacing::XXS` below the control and hidden while the field is valid. Only the
first invalid field shows a reason, so the reason always names what to fix next.

The Composition dialog and the composition Properties section expose a `KColorChip` Background
Colour for the composition itself; the Viewer shows it inside the composition frame. The Viewer's
surround is chosen separately under View > Background (Solid, Grey, Black, White), and View >
Transparency Grid swaps the frame's Background Colour for a checker (see "Viewer canvas and empty
state"). Neither alters composition pixels, the authored Background Colour, or export alpha.

## Live Value Editing

A `KValueField` is a horizontal scrub handle, including its label. Drag past the platform drag
threshold to change one single-step per logical pixel; Ctrl uses one tenth of that step and Shift
uses ten times the step. The threshold is a dead zone, so crossing it never jumps the value, and
pressing or releasing a modifier mid-drag starts a new segment where the pointer is, from the value
already reached. Each segment is measured from its own start, so wandering back lands exactly on its
base. On X11, Windows and macOS the pointer wraps at the screen edge so a scrub never runs out of
travel; Wayland gives an application no way to move the pointer, so there the scrub ends at the
screen edge (release and scrub again, or hold Shift) -- the documented fallback. Losing the pointer
grab or the window mid-scrub finishes the scrub as a commit. A disabled field shows the arrow cursor.

A press never takes keyboard focus, so frame stepping and the transport keys keep working after a
scrub or a click. A click opens an inline editor with the exact value selected. The editor takes the
field's own unit (`50%`, `10 px`), either decimal separator, arithmetic (`+ - * /` and parentheses)
and relative entries (`+= 10`, `-=`, `*=`, `/=`, applied to the value the edit started from).
Integer parameters use the field's integer mode: int64 storage, whole steps and an exact parse with
no rounding through a double, so a value above 2^53 survives. Valid text, Up/Down and Page Up/Page
Down update the session's live value and every visible readback immediately. Intermediate text such
as a minus sign does not replace the last valid value.

Typing follows "Escape confirms" above: Return, Escape, Tab and focus loss all commit, and a
pointer release or key release finishes a scrub/step. One completed gesture is one undo entry. Text that does not parse, carries another unit, or falls outside the parameter's range is
refused, never clamped and never zero: Return or Tab keeps the editor open with an `Error` border and
the reason in its tooltip and accessible description; Escape or focus loss closes it on the last
valid value and a tooltip says the typed text was not taken. Tab and Shift+Tab commit and travel to
the next or previous field, which opens ready to type; a digit typed into a keyboard-focused field
starts an edit with it. The wheel steps a field that is being edited, or a keyboard-focused one with
a modifier held (whole notches only, accumulated from high-resolution wheels); a burst of wheel steps
is one edit that ends once the wheel has been still for `kWheelGestureIdleMs`. A wheel over a field
at rest scrolls the panel behind it. A held stepper auto-repeats after `kAutoRepeatDelayMs`.

A field reflecting several objects whose values differ is mixed: an em dash at rest and an empty
editor; typing writes the value to all of them and an empty entry changes nothing. A field with a
default (`setDefaultValue`) marks a differing value with the `NonDefaultMark` Accent dot in its top
right corner and resets it -- as one complete edit -- from a double-click on its label or Reset in
its context menu. The field reads as a spin box to assistive technology, with its value, minimum,
maximum and step; `KSlider` and `KRangeSelector` read as sliders with their values.

Properties, node cards and timeline fields use the same kit gesture signals, and configure every
parameter field from the parameter's one presentation (`document::parameterPresentation`, applied by
`bloom::ui::configureValueField`): hard range (the schema's own domain), soft range (the span of a
slider beside it), step, decimals, unit and display scale. The same parameter therefore scrubs,
steps and reads identically on all three surfaces; Properties' letter spacing alone layers its
percent-of-size reading on top. The active field keeps its text/caret or scrub base while peer
fields show the live number. Colour-picker spatial drags and channel edits use the same
live/commit/cancel boundary; closing an active picker, with Escape too, accepts its edit. A
`KSlider` and a `KRangeSelector` keep the same edit-session contract: a drag (or a run of keys) is
one edit, pressing a slider's handle keeps its grab offset, and grab loss commits. A range
selector's arrow, Page and Home/End keys move its active handle, Tab moves to the other handle
before focus leaves, and handles that sit on each other are told apart by the first move's
direction. Preview rendering runs independently of these control readbacks.

## MCP Server panel

Editor `bloom.mcp-server`, "MCP Server" (ADR 0034, section 7). It holds no server state: it reads the
snapshot through the application's `McpServerAdapter` (`changed()`), issues commands through it and
never pumps. It is built from kit parts only: `KSection` bodies with `KPropertyRow` rows, `KButton`
(Primary for Start, Secondary otherwise), `KValueField` in integer mode, `KSwitch` with a
`KCheckLabel`, `KMenuButton`, `KRow` for the clients list and `kit::confirm` for Regenerate.
Inline reasons use `KLabel` in the Error tone under the control they belong to; nothing is disabled
without its reason in the tooltip and under the control.

| Object name | Part |
| --- | --- |
| `mcpServerEditor` | the panel |
| `mcpServerSection`, `mcpAccessSection`, `mcpClientsSection`, `mcpActivitySection` | `KSection`s |
| `mcpStatus`, `mcpStartStopButton`, `mcpStartReason` | status line, the one Start/Stop button, its reason |
| `mcpPortField`, `mcpPortReason` | port field (0..999999 so out-of-range values get `validateMcpPort`'s reason) and its inline reason |
| `mcpAutoStartSwitch`, `mcpAutoStartLabel` | Start when Bloom opens |
| `mcpToken`, `mcpTokenRevealButton`, `mcpTokenCopyButton`, `mcpTokenRegenerateButton`, `mcpTokenReason`, `mcpRevealTimer` | token row; the reveal timer is 30 s |
| `mcpCopyConfigButton`, `mcpCopyConfigMenu`, `mcpCopyConfig_<kind>_<token|placeholder>` | copy client config, four kinds by two disclosures |
| `mcpClientsList`, `mcpClientsHeader`, `mcpClientRow`, `mcpClientName`, `mcpClientVersion`, `mcpClientSince`, `mcpClientLastActivity`, `mcpClientInFlight`, `mcpClientLastTool`, `mcpNoClients`, `mcpProjectLine` | connected clients |
| `mcpActivityList` | activity, appended with `activity(afterId)`; a compile failure an agent edit caused is added in the error tone (`McpServerAdapter::noteCompileFailure`) |
| `windowStatusBarMcp`, `preferencesMcpLink` | Status bar indicator (shown only while running or failed), Preferences link |

Narrow panels hide the clients columns in this order: last tool, in flight, version.

### Compile failure details

When a composition cannot compile, the status bar keeps its one-sentence headline and gains a
`Details` disclosure beside it. The disclosure opens a `KPopupWindow` anchored to the bar.

| Object name | Role |
| --- | --- |
| `windowStatusBarCompileDetails` | the disclosure button, shown only while the preview failed to compile |
| `windowStatusBarCompileFailurePopup` | the popup window |
| `compileFailurePanel` | the popup body |
| `compileFailureHeadline`, `compileFailureDetail`, `compileFailureSubject` | headline (error tone), plain detail, subject in words ("Layer 'Title' · Blending Mode") |
| `compileFailureSelect` | selects the layer, or the node, through the session selection commands |
| `compileFailureCopy` | copies the raw diagnostic (code and ids); the only place a code appears |

Properties and Nodes have no per-parameter invalid state outside text entry, so they show no
marker of their own; Select moves their selection to the subject.

## Text fields

`KTextField` is the kit's one text field: a `Field` well in the control body with `Spacing::S`
padding, `Radius::Small` corners and the single border (`BorderHover` under the pointer, `Accent`
while it has focus), `Faint` placeholder ink, and an `Error` border with a reason for a refused value.
Names and prose use the `Ui` role; numbers, hex, timecode and other machine text use `Value`.
`KLineEdit` is its historical name and keeps working. `KSearchField` adds a leading magnifier and a
trailing clear button that shows only with text; its first Escape clears the text and the second is
left for the surface around it (a popup closes, a panel takes focus back). `KTextArea` is the same
well for multi-line text: a text layer's content, a job's diagnostics, a licence. The look is
generated from `kit::fieldColors()` into the application sheet; no widget carries its own sheet.

Every in-place editor -- a value field's text entry, a rename, the frame field, the Properties and
node-card text rows, the colour hex field -- installs the kit inline-edit policy (`kit::KInlineEdit`)
instead of spelling its keys: Return (Ctrl+Return in a multi-line editor), Escape, Tab and focus loss
commit; refused text stays open on Return or Tab with the reason, and closes on the last valid value
for Escape or focus loss; the editor's own context menu and a switch to another window are not
endings. Text parameters bind
through one editor binding (`properties::bindTextEdit`): typing previews live and the ending commits
exactly one transaction.

`KCheckBox` draws an `Accent` ring outside a checked box with keyboard focus, may carry its own
clickable label, and has a mixed (indeterminate) state. `KSwitch` shows hover in both states, and a
programmatic change places its thumb without animating; only an artist's toggle slides it.

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
Focus loss accepts, and so does Escape (Escape confirms everywhere; Undo is the discard). A
completed gesture is one `Edit Text` transaction. The Properties single-line and expanded text fields use the same
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
